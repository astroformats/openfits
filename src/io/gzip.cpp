// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "compress/gzip.h"

#include <openfits/error.h>
#include <openfits/io.h>

#include "core/checked_math.h"
#include "io/native_file.h"
#include "io/paths.h"
#include "io/range_check.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace openfits {

namespace {

// The compressed bytes read at a time from a source whose content does not lie in memory.
constexpr std::size_t compressed_chunk = std::size_t{4} << 20;
// The inflated bytes written at a time to a temporary file, at least.
constexpr std::size_t staging_chunk = std::size_t{4} << 20;
// The smallest growth of the memory of the content, when the size that the file gives is wrong.
constexpr std::size_t growth = std::size_t{1} << 20;
// A gzip file of more bytes than this takes an ISIZE smaller than its size for one of more than 4 GiB inflated, as
// CFITSIO takes it.
constexpr std::uint64_t small_file = 10'000;

// Memory for the inflated content, which is written before it is read and so not filled with zeros first.
// The array form of std::unique_ptr is how the standard library allocates bytes without initializing them.
// NOLINTBEGIN(modernize-avoid-c-arrays)
class content_buffer
{
public:
    [[nodiscard]] std::span<std::byte> room() noexcept
    {
        return {data_.get() + size_, capacity_ - size_};
    }

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept
    {
        return {data_.get(), size_};
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return size_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return capacity_;
    }

    void advance(std::size_t count) noexcept
    {
        size_ += count;
    }

    void clear() noexcept
    {
        size_ = 0;
    }

    // Moves the bytes into memory of capacity bytes, at least size().
    void reallocate(std::size_t capacity)
    {
        std::unique_ptr<std::byte[]> moved = std::make_unique_for_overwrite<std::byte[]>(capacity);
        std::ranges::copy(bytes(), moved.get());
        data_ = std::move(moved);
        capacity_ = capacity;
    }

    [[nodiscard]] std::unique_ptr<std::byte[]> release() noexcept
    {
        capacity_ = 0;
        size_ = 0;
        return std::move(data_);
    }

private:
    std::unique_ptr<std::byte[]> data_{};
    std::size_t capacity_ = 0;
    std::size_t size_ = 0;
};
// NOLINTEND(modernize-avoid-c-arrays)

// The size of the content that the trailer of the last member gives, ISIZE (RFC 1952, §2.3.1), which is the size of a
// file of one member modulo 2^32: a size to allocate at once, never beyond what deflate makes of size bytes.
std::uint64_t content_estimate(const input_source& compressed, std::uint64_t size)
{
    if (size < 4) {
        return 0;
    }
    std::array<std::byte, 4> trailer{};
    compressed.read(size - 4, trailer);
    std::uint64_t estimate = 0;
    for (std::size_t i = 4; i-- > 0;) {
        estimate = (estimate << 8U) | std::to_integer<std::uint64_t>(trailer[i]);
    }
    const std::uint64_t most = size > std::numeric_limits<std::uint64_t>::max() / detail::deflate_max_expansion
                                   ? std::numeric_limits<std::uint64_t>::max()
                                   : size * detail::deflate_max_expansion;
    constexpr std::uint64_t wrap = std::uint64_t{1} << 32;
    while (size > small_file && estimate < size && most >= wrap && estimate <= most - wrap) {
        estimate += wrap;
    }
    return std::min(estimate, most);
}

// The path of a new temporary file in directory, given as the application gave it.
detail::native_file create_temporary(const std::string& directory)
{
    std::string base = detail::absolute_path(directory);
#if defined(_WIN32)
    if (base.back() != '\\' && base.back() != '/') {
        base += '\\';
    }
#else
    if (base.back() != '/') {
        base += '/';
    }
#endif
    // temporary_path() takes the directory of a file in it.
    base += 'x';
    std::uint64_t random = detail::random_seed();
    for (int attempt = 0; attempt < 16; ++attempt) {
        std::optional<detail::native_file> file =
            detail::native_file::create_scratch(detail::temporary_path(base, detail::next_random(random)));
        if (file) {
            return std::move(*file);
        }
    }
    throw io_error(errc::open_failed,
                   "cannot create a temporary file in " + directory + " for an inflated file: every name tried exists");
}

// The input of an inflation: the content of the source when it lies in memory, or its bytes read a chunk at a time.
class compressed_input
{
public:
    explicit compressed_input(const input_source& source) : source_(&source), size_(source.size())
    {
        if (const std::span<const std::byte> bytes = source.bytes(); !bytes.empty() && bytes.size() == size_) {
            whole_ = bytes;
        } else {
            const std::size_t chunk = std::min<std::uint64_t>(size_, compressed_chunk);
            chunk_.resize(chunk);
        }
    }

    // Gives the inflater the next bytes of the source; false at its end.
    bool feed(detail::gzip_file_inflater& inflater)
    {
        if (offset_ == size_) {
            return false;
        }
        if (!whole_.empty()) {
            inflater.give(whole_);
            offset_ = size_;
            return true;
        }
        const std::size_t count = std::min<std::uint64_t>(chunk_.size(), size_ - offset_);
        const std::span<std::byte> piece = std::span(chunk_).first(count);
        source_->read(offset_, piece);
        inflater.give(piece);
        offset_ += count;
        return true;
    }

private:
    const input_source* source_;
    std::uint64_t size_;
    std::uint64_t offset_ = 0;
    std::span<const std::byte> whole_{};
    std::vector<std::byte> chunk_{};
};

} // namespace

// The content in memory, or in a temporary file.
// NOLINTBEGIN(modernize-avoid-c-arrays)
struct gzip_source::state
{
    std::unique_ptr<std::byte[]> memory{};
    std::optional<detail::native_file> file{};
    std::uint64_t size = 0;
    std::string description{};
};
// NOLINTEND(modernize-avoid-c-arrays)

gzip_source::gzip_source(const input_source& compressed, const limits& limits, const inflate_options& options)
{
    const bool spill = !options.temporary_directory.empty();
    if (spill) {
        detail::check_path_argument(options.temporary_directory, "the temporary directory");
    }
    std::string name = compressed.description();
    if (name.empty()) {
        name = "a gzip-compressed source";
    }

    // The most bytes held in memory, and the limit that sets it.
    std::uint64_t memory_limit = std::numeric_limits<std::size_t>::max();
    errc limit_code = errc::allocation_too_large;
    if (limits.max_allocation != 0 && limits.max_allocation < memory_limit) {
        memory_limit = limits.max_allocation;
    }
    if (limits.max_inflated_bytes != 0 && limits.max_inflated_bytes <= memory_limit) {
        memory_limit = limits.max_inflated_bytes;
        limit_code = errc::inflated_too_large;
    }
    const std::uint64_t memory_cap = spill ? std::min(memory_limit, options.memory_threshold) : memory_limit;

    const std::uint64_t compressed_size = compressed.size();
    detail::gzip_file_inflater inflater;
    compressed_input input(compressed);
    content_buffer content;
    std::optional<detail::native_file> file;
    std::uint64_t written = 0;
    // The content goes into a temporary file from here, through the memory of the content, which stages it.
    const auto start_file = [&]() {
        file = create_temporary(options.temporary_directory);
        if (content.capacity() < staging_chunk) {
            content.reallocate(staging_chunk);
        }
        file->write_at(0, content.bytes());
        written = content.size();
        content.clear();
    };
    if (memory_cap == 0) {
        start_file();
    } else {
        const std::uint64_t estimate = content_estimate(compressed, compressed_size);
        const std::size_t capacity = std::clamp<std::uint64_t>(estimate, 1, memory_cap);
        content.reallocate(capacity);
    }

    // A few bytes inflated when the memory is full, which tell whether the content goes on.
    std::array<std::byte, 4096> probe{};
    for (;;) {
        if (inflater.needs_input()) {
            if (!input.feed(inflater)) {
                break;
            }
            continue;
        }
        if (!content.room().empty()) {
            content.advance(inflater.inflate(content.room()));
            continue;
        }
        if (file) {
            file->write_at(written, content.bytes());
            written += content.size();
            content.clear();
            continue;
        }
        const std::size_t more = inflater.inflate(probe);
        if (more == 0) {
            continue;
        }
        const std::uint64_t needed = content.size() + more;
        if (needed <= memory_cap) {
            const std::uint64_t capacity = content.capacity();
            const std::size_t larger = std::min(memory_cap, std::max({needed, capacity * 2, capacity + growth}));
            content.reallocate(larger);
        } else if (spill) {
            start_file();
        } else {
            throw limit_error(limit_code, "the content of " + name + " inflates to more than " +
                                              (limit_code == errc::inflated_too_large ? "limits::max_inflated_bytes, "
                                                                                      : "limits::max_allocation, ") +
                                              std::to_string(memory_limit) +
                                              " bytes, and no temporary directory is given");
        }
        std::ranges::copy(std::span(probe).first(more), content.room().begin());
        content.advance(more);
    }
    inflater.finish();

    auto result = std::make_unique<state>();
    result->description = name + ", inflated";
    if (file) {
        file->write_at(written, content.bytes());
        result->size = written + content.size();
        result->file = std::move(file);
    } else {
        // Memory much larger than the content, where the size that the file gives was wrong, is given back.
        if (content.capacity() - content.size() > std::max(growth, content.size() / 8)) {
            content.reallocate(content.size());
        }
        result->size = content.size();
        result->memory = content.release();
    }
    state_ = std::move(result);
}

gzip_source::~gzip_source() = default;

std::uint64_t gzip_source::size() const
{
    return state_->size;
}

void gzip_source::read(std::uint64_t offset, std::span<std::byte> destination) const
{
    detail::check_read_range(state_->size, offset, destination.size(), state_->description);
    if (state_->file) {
        state_->file->read_at(offset, destination);
        return;
    }
    std::ranges::copy(bytes().subspan(detail::checked_cast<std::size_t>(offset), destination.size()),
                      destination.begin());
}

bool gzip_source::supports_concurrent_reads() const
{
    return true;
}

std::string gzip_source::description() const
{
    return state_->description;
}

std::span<const std::byte> gzip_source::bytes() const
{
    if (state_->file) {
        return {};
    }
    return {state_->memory.get(), detail::checked_cast<std::size_t>(state_->size)};
}

struct gzip_sink::state
{
    state(output_sink& sink, int level) : deflater(level, sink), target(&sink) {}

    detail::gzip_member_deflater deflater;
    output_sink* target;
    std::uint64_t written = 0;
};

gzip_sink::gzip_sink(output_sink& target, int level)
{
    if (level < -1 || level > 9) {
        throw usage_error(errc::invalid_argument, "the gzip level " + std::to_string(level) + " is not from -1 to 9");
    }
    state_ = std::make_unique<state>(target, level);
}

gzip_sink::~gzip_sink() = default;

void gzip_sink::write(std::span<const std::byte> data)
{
    state_->deflater.compress(data);
    state_->written += data.size();
}

std::uint64_t gzip_sink::position() const
{
    return state_->written;
}

void gzip_sink::finish()
{
    state_->deflater.finish();
    state_->target->finish();
}

} // namespace openfits
