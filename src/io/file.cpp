// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>
#include <openfits/io.h>

#include "io/native_file.h"
#include "io/paths.h"
#include "io/range_check.h"

#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace openfits {

namespace {

// 64 bits of the random source of the system, from which the names of the temporary files of a sink follow.
std::uint64_t random_seed()
{
    // std::random_device reports a failure with an exception type of its own choice, which depends on the standard
    // library. Every one of them becomes an io_error, except std::bad_alloc.
    try {
        std::random_device device;
        // Each call produces 32 bits.
        const std::uint64_t high = device();
        return (high << 32U) | std::uint64_t{device()};
    } catch (const std::system_error& failure) {
        throw io_error(errc::entropy_unavailable, "no random data for a temporary name: " + std::string(failure.what()),
                       failure.code());
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& failure) {
        throw io_error(errc::entropy_unavailable,
                       "no random data for a temporary name: " + std::string(failure.what()));
    }
}

// The next value of the SplitMix64 sequence of state, which gives each attempt of a sink a name of its own.
std::uint64_t next_random(std::uint64_t& state) noexcept
{
    state += 0x9E37'79B9'7F4A'7C15U;
    std::uint64_t value = state;
    value = (value ^ (value >> 30U)) * 0xBF58'476D'1CE4'E5B9U;
    value = (value ^ (value >> 27U)) * 0x94D0'49BB'1331'11EBU;
    return value ^ (value >> 31U);
}

// A name in the directory of target, which is absolute and so has a separator: .openfits-<16 hexadecimal digits>.tmp. A
// fixed length keeps it within the limits of the file system whatever the name of the target.
std::string temporary_path(std::string_view target, std::uint64_t random)
{
#if defined(_WIN32)
    const std::size_t separator = target.find_last_of("/\\");
#else
    const std::size_t separator = target.find_last_of('/');
#endif
    std::string path(target.substr(0, separator + 1));
    path += ".openfits-";
    constexpr std::string_view digits = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4) {
        path += digits[(random >> static_cast<unsigned>(shift)) & 0xFU];
    }
    path += ".tmp";
    return path;
}

} // namespace

struct file_source::state
{
    detail::native_file file;
    std::uint64_t size = 0;
};

file_source::file_source(std::string_view path)
{
    detail::check_path_argument(path, "the path");
    detail::native_file file = detail::native_file::open_for_reading(std::string(path));
    const std::uint64_t size = file.size();
    state_ = std::make_unique<const state>(state{.file = std::move(file), .size = size});
}

file_source::~file_source() = default;

std::uint64_t file_source::size() const
{
    return state_->size;
}

void file_source::read(std::uint64_t offset, std::span<std::byte> destination) const
{
    detail::check_read_range(state_->size, offset, destination.size(), state_->file.name());
    state_->file.read_at(offset, destination);
}

bool file_source::supports_concurrent_reads() const
{
    return true;
}

std::string file_source::description() const
{
    return state_->file.name();
}

struct file_device::state
{
    detail::native_file file;
    std::uint64_t size = 0;
};

file_device::file_device(std::string_view path)
{
    detail::check_path_argument(path, "the path");
    detail::native_file file = detail::native_file::open_for_update(std::string(path));
    const std::uint64_t size = file.size();
    state_ = std::make_unique<state>(state{.file = std::move(file), .size = size});
}

file_device::~file_device() = default;

std::uint64_t file_device::size() const
{
    return state_->size;
}

void file_device::read(std::uint64_t offset, std::span<std::byte> destination) const
{
    detail::check_read_range(state_->size, offset, destination.size(), state_->file.name());
    state_->file.read_at(offset, destination);
}

void file_device::write(std::uint64_t offset, std::span<const std::byte> data)
{
    detail::check_write_range(state_->size, offset, data.size(), state_->file.name());
    state_->file.write_at(offset, data);
}

bool file_device::can_resize() const
{
    return true;
}

void file_device::resize(std::uint64_t size)
{
    state_->file.resize(size);
    state_->size = size;
}

void file_device::flush()
{
    state_->file.flush_to_disk();
}

bool file_device::supports_concurrent_reads() const
{
    return true;
}

bool file_device::supports_concurrent_writes() const
{
    return true;
}

std::string file_device::description() const
{
    return state_->file.name();
}

struct file_sink::state
{
    state(std::string target_path, std::string temporary_path, detail::native_file temporary_file,
          const file_sink_options& options)
        : target(std::move(target_path)), temporary(std::move(temporary_path)), file(std::move(temporary_file)),
          flush_to_disk(options.flush_to_disk), replace(options.replace)
    {}

    state(const state&) = delete;
    state& operator=(const state&) = delete;

    // Unless finish() replaced the target with it, the temporary file holds an incomplete file.
    ~state()
    {
        discard();
    }

    void discard() noexcept
    {
        if (!temporary.empty()) {
            file.discard(temporary);
            temporary.clear();
        }
    }

    void check_writable() const
    {
        if (finished) {
            throw usage_error(errc::invalid_argument, "the file sink for " + target + " has finished");
        }
    }

    std::string target;
    std::string temporary;
    detail::native_file file;
    std::uint64_t position = 0;
    bool flush_to_disk;
    bool replace;
    bool finished = false;
};

file_sink::file_sink(std::string_view path, file_sink_options options)
{
    detail::check_path_argument(path, "the path");
    // finish() replaces the file that the path names now, whatever the current directory is by then.
    std::string target = detail::absolute_path(std::string(path));
    // A rename would replace a FIFO, a socket or a device, and fails on a directory only once the file is written.
    detail::check_replaceable(target);
    if (!options.replace && detail::exists(target)) {
        throw io_error(errc::file_exists, target + " exists, and the file sink may not replace it");
    }
    // A name taken by another file is tried again with another random number, which only a crowded directory needs.
    std::uint64_t random = random_seed();
    for (int attempt = 0; attempt < 16; ++attempt) {
        std::string temporary = temporary_path(target, next_random(random));
        std::optional<detail::native_file> file = detail::native_file::create_new(temporary, target);
        if (file) {
            // The file exists from here, and must not stay behind when the state cannot be allocated. Nothing is moved
            // before the allocation succeeds.
            try {
                state_ = std::make_unique<state>(std::move(target), std::move(temporary), std::move(*file), options);
            } catch (...) {
                file->discard(temporary);
                throw;
            }
            return;
        }
    }
    throw io_error(errc::open_failed, "cannot create a temporary file next to " + target + ": every name tried exists");
}

file_sink::~file_sink() = default;

void file_sink::write(std::span<const std::byte> data)
{
    state_->check_writable();
    state_->file.write_at(state_->position, data);
    state_->position += data.size();
}

std::uint64_t file_sink::position() const
{
    return state_->position;
}

bool file_sink::can_rewrite() const
{
    return true;
}

void file_sink::rewrite(std::uint64_t offset, std::span<const std::byte> data)
{
    state_->check_writable();
    detail::check_rewrite_range(state_->position, offset, data.size());
    state_->file.write_at(offset, data);
}

void file_sink::finish()
{
    state_->check_writable();
    state_->finished = true;
    try {
        // Another program can have changed the target while the file was written.
        detail::check_replaceable(state_->target);
        state_->file.commit(state_->temporary, state_->target, state_->flush_to_disk, state_->replace);
        state_->temporary.clear();
    } catch (...) {
        state_->discard();
        throw;
    }
}

} // namespace openfits
