// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/error.h>
#include <openfits/io.h>
#include <openfits/limits.h>

#if defined(_WIN32)
#include <sys/stat.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <istream>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

/// @file
/// Sources and sinks over C streams (`std::FILE*`) and C++ streams, and the reading of a stream that cannot seek into
/// memory.
///
/// They are defined inline on purpose: the stream functions then run in the caller's C runtime and standard library,
/// which keeps a stream from crossing runtimes on Windows. None of them supports concurrent reads, so the library
/// calls them from one thread at a time. They never close their stream, which must stay open while they are in use.

namespace openfits {

// The C stream functions of stdio_source and stdio_sink, in the caller's C runtime, and the reading of a stream into
// memory.
namespace detail {

// The position of a C stream, or -1 when it cannot seek. The C runtime of Windows gives 0 for a pipe or a device,
// where a seek succeeds and does nothing, so there the stream must also be on a regular file.
inline std::int64_t stdio_tell(std::FILE* stream) noexcept
{
#if defined(_WIN32)
    struct _stat64 status{};
    if (_fstat64(_fileno(stream), &status) != 0 || (status.st_mode & _S_IFMT) != _S_IFREG) {
        return -1;
    }
    return _ftelli64(stream);
#else
    return ftello(stream);
#endif
}

inline bool stdio_seek(std::FILE* stream, std::int64_t offset, int origin) noexcept
{
#if defined(_WIN32)
    return _fseeki64(stream, offset, origin) == 0;
#else
    return fseeko(stream, offset, origin) == 0;
#endif
}

// The error of the last call of the C runtime, from errno, which the caller sets to 0 before it.
inline std::error_code stdio_error() noexcept
{
    const int error = errno;
    return error != 0 ? std::error_code(error, std::generic_category()) : std::error_code();
}

// The bytes of a stream to its end, in one allocation of their size within limit bytes, zero for none. read fills the
// span it is given from the stream, and returns how many bytes it filled and whether the stream has not ended, which a
// full read does not tell. The stream is read in chunks, which are copied into place once its size is known, so that
// the bytes are held once in the end and twice at most on the way.
template <typename Read> std::vector<std::byte> read_stream(const Read& read, std::uint64_t limit)
{
    constexpr std::size_t chunk_size = std::size_t{1} << 20U;
    std::vector<std::vector<std::byte>> chunks;
    std::uint64_t size = 0;
    std::size_t count = 0;
    bool more = true;
    while (more) {
        std::vector<std::byte> chunk(chunk_size);
        const std::pair<std::size_t, bool> filled = read(std::span<std::byte>(chunk));
        count = filled.first;
        more = filled.second && count == chunk_size;
        size += count;
        if (limit != 0 && size > limit) {
            throw limit_error(errc::allocation_too_large,
                              "the stream holds more bytes than the allocation limit of " + std::to_string(limit));
        }
        chunks.push_back(std::move(chunk));
    }
    // Every chunk is full but the last, which holds count bytes.
    std::vector<std::byte> data(size);
    auto out = data.begin();
    for (const std::vector<std::byte>& chunk : chunks) {
        const auto length = static_cast<std::ptrdiff_t>(&chunk == &chunks.back() ? count : chunk.size());
        out = std::copy(chunk.begin(), chunk.begin() + length, out);
    }
    return data;
}

} // namespace detail

/// A C stream from its position at construction to its end. The stream must be open in binary mode and able to seek.
/// The constructor leaves the stream at its end, and reads move its position.
class stdio_source final : public input_source
{
public:
    /// @throws usage_error when stream is null.
    /// @throws io_error (errc::not_seekable) when the stream cannot seek.
    explicit stdio_source(std::FILE* stream) : stream_(stream)
    {
        if (stream_ == nullptr) {
            throw usage_error(errc::invalid_argument, "the FILE stream is null");
        }
        errno = 0;
        const std::int64_t start = detail::stdio_tell(stream_);
        if (start < 0 || !detail::stdio_seek(stream_, 0, SEEK_END)) {
            throw io_error(errc::not_seekable, "the FILE stream cannot seek", detail::stdio_error());
        }
        const std::int64_t end = detail::stdio_tell(stream_);
        if (end < start) {
            throw io_error(errc::not_seekable, "the FILE stream cannot seek", detail::stdio_error());
        }
        start_ = static_cast<std::uint64_t>(start);
        size_ = static_cast<std::uint64_t>(end - start);
    }

    [[nodiscard]] std::uint64_t size() const override
    {
        return size_;
    }

    void read(std::uint64_t offset, std::span<std::byte> destination) const override
    {
        if (offset > size_ || destination.size() > size_ - offset) {
            throw io_error(errc::end_of_data, "the read goes past the end of the FILE stream", {}, {.offset = offset});
        }
        if (destination.empty()) {
            return;
        }
        std::clearerr(stream_);
        errno = 0;
        if (!detail::stdio_seek(stream_, static_cast<std::int64_t>(start_ + offset), SEEK_SET)) {
            throw io_error(errc::read_failed, "cannot seek in the FILE stream", detail::stdio_error(),
                           {.offset = offset});
        }
        if (std::fread(destination.data(), 1, destination.size(), stream_) != destination.size()) {
            // The end of the stream comes early when it is shorter than when the source was made.
            const errc code = std::feof(stream_) != 0 ? errc::end_of_data : errc::read_failed;
            throw io_error(code, "cannot read the FILE stream", detail::stdio_error(), {.offset = offset});
        }
    }

private:
    std::FILE* stream_;
    std::uint64_t start_ = 0;
    std::uint64_t size_ = 0;
};

/// A C stream written from its position at construction. The stream must be open in binary mode, or the system may
/// change the bytes written, such as line ends on Windows. It supports rewrite() when it can seek, which a pipe cannot.
/// A stream opened in append mode writes every byte at its end, so it must not be given to a sink that rewrites.
class stdio_sink final : public output_sink
{
public:
    /// @throws usage_error when stream is null.
    explicit stdio_sink(std::FILE* stream) : stream_(stream)
    {
        if (stream_ == nullptr) {
            throw usage_error(errc::invalid_argument, "the FILE stream is null");
        }
        const std::int64_t start = detail::stdio_tell(stream_);
        rewritable_ = start >= 0;
        start_ = rewritable_ ? static_cast<std::uint64_t>(start) : 0;
    }

    void write(std::span<const std::byte> data) override
    {
        errno = 0;
        if (std::fwrite(data.data(), 1, data.size(), stream_) != data.size()) {
            throw io_error(errc::write_failed, "cannot write to the FILE stream", detail::stdio_error(),
                           {.offset = position_});
        }
        position_ += data.size();
    }

    [[nodiscard]] std::uint64_t position() const override
    {
        return position_;
    }

    [[nodiscard]] bool can_rewrite() const override
    {
        return rewritable_;
    }

    void rewrite(std::uint64_t offset, std::span<const std::byte> data) override
    {
        if (!rewritable_) {
            throw usage_error(errc::invalid_argument, "the FILE stream cannot seek, so it cannot rewrite");
        }
        if (offset > position_ || data.size() > position_ - offset) {
            throw usage_error(errc::invalid_argument, "a rewrite must stay within the bytes already written");
        }
        // A seek flushes what the stream buffered, and the C library of Windows reports a failure of that flush only
        // through the error indicator of the stream, which is cleared first.
        errno = 0;
        std::clearerr(stream_);
        if (!detail::stdio_seek(stream_, static_cast<std::int64_t>(start_ + offset), SEEK_SET) ||
            std::fwrite(data.data(), 1, data.size(), stream_) != data.size() ||
            !detail::stdio_seek(stream_, static_cast<std::int64_t>(start_ + position_), SEEK_SET) ||
            std::ferror(stream_) != 0) {
            throw io_error(errc::write_failed, "cannot rewrite the FILE stream", detail::stdio_error(),
                           {.offset = offset});
        }
    }

    void finish() override
    {
        errno = 0;
        if (std::fflush(stream_) != 0) {
            throw io_error(errc::write_failed, "cannot flush the FILE stream", detail::stdio_error());
        }
    }

private:
    std::FILE* stream_;
    std::uint64_t start_ = 0;
    std::uint64_t position_ = 0;
    bool rewritable_ = false;
};

/// A C++ input stream from its position at construction to its end. The stream must be able to seek, and binary on
/// platforms where that matters; the source seeks with offsets from the start, through the seekoff() of the stream
/// buffer. The source clears the stream's state and moves its position. When the stream's exceptions() mask is set, the
/// stream's own exceptions pass through unchanged.
///
/// A stream buffer cannot be asked what it reads, so the source takes the stream's word that it can seek. On Windows a
/// file stream on a pipe or a device, such as MSVC's, reports a position and seeks there without effect: it passes for
/// a stream that can seek, and gives other bytes than those asked for. Read a pipe with read_into_memory(), or give
/// stdio_source its FILE*, which is checked.
class istream_source final : public input_source
{
public:
    /// @throws io_error (errc::not_seekable) when the stream cannot seek.
    explicit istream_source(std::istream& stream) : stream_(stream)
    {
        stream_.clear();
        const std::streamoff start = stream_.tellg();
        if (start >= 0) {
            stream_.seekg(0, std::ios::end);
        }
        const std::streamoff end = stream_.fail() ? -1 : static_cast<std::streamoff>(stream_.tellg());
        if (start < 0 || end < start) {
            throw io_error(errc::not_seekable, "the input stream cannot seek");
        }
        start_ = static_cast<std::uint64_t>(start);
        size_ = static_cast<std::uint64_t>(end - start);
    }

    [[nodiscard]] std::uint64_t size() const override
    {
        return size_;
    }

    void read(std::uint64_t offset, std::span<std::byte> destination) const override
    {
        if (offset > size_ || destination.size() > size_ - offset) {
            throw io_error(errc::end_of_data, "the read goes past the end of the input stream", {}, {.offset = offset});
        }
        if (destination.empty()) {
            return;
        }
        stream_.clear();
        // From the start, with seekoff(), as the size was measured: a buffer may implement it without seekpos().
        stream_.seekg(static_cast<std::streamoff>(start_ + offset), std::ios::beg);
        if (stream_.fail()) {
            throw io_error(errc::read_failed, "cannot seek in the input stream", {}, {.offset = offset});
        }
        const auto count = static_cast<std::streamsize>(destination.size());
        stream_.read(reinterpret_cast<char*>(destination.data()), count);
        if (stream_.gcount() != count) {
            // The end of the stream comes early when it is shorter than when the source was made.
            const errc code = stream_.eof() && !stream_.bad() ? errc::end_of_data : errc::read_failed;
            throw io_error(code, "cannot read the input stream", {}, {.offset = offset});
        }
    }

private:
    std::istream& stream_;
    std::uint64_t start_ = 0;
    std::uint64_t size_ = 0;
};

/// A C++ output stream written from its position at construction. The stream must be binary on platforms where that
/// matters. It supports rewrite() when the stream can seek, through the seekoff() of the stream buffer. A stream opened
/// with std::ios::app writes every byte at its end, so it must not be given to a sink that rewrites.
/// When the stream's exceptions() mask is set, the stream's own exceptions pass through unchanged.
///
/// As for istream_source, the sink takes the stream's word that it can seek. On Windows a file stream on a pipe or a
/// device passes for one that can, and its rewrites are appended, which makes a file that cannot be read. Write to a
/// pipe through a callback_sink without a rewrite function, or a stdio_sink, which checks its FILE*.
class ostream_sink final : public output_sink
{
public:
    explicit ostream_sink(std::ostream& stream) : stream_(stream)
    {
        const std::streamoff start = stream_.tellp();
        rewritable_ = start >= 0;
        start_ = rewritable_ ? static_cast<std::uint64_t>(start) : 0;
    }

    void write(std::span<const std::byte> data) override
    {
        stream_.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (stream_.fail()) {
            throw io_error(errc::write_failed, "cannot write to the output stream", {}, {.offset = position_});
        }
        position_ += data.size();
    }

    [[nodiscard]] std::uint64_t position() const override
    {
        return position_;
    }

    [[nodiscard]] bool can_rewrite() const override
    {
        return rewritable_;
    }

    void rewrite(std::uint64_t offset, std::span<const std::byte> data) override
    {
        if (!rewritable_) {
            throw usage_error(errc::invalid_argument, "the output stream cannot seek, so it cannot rewrite");
        }
        if (offset > position_ || data.size() > position_ - offset) {
            throw usage_error(errc::invalid_argument, "a rewrite must stay within the bytes already written");
        }
        // From the start, with seekoff(), as the position was taken: a buffer may implement it without seekpos().
        stream_.seekp(static_cast<std::streamoff>(start_ + offset), std::ios::beg);
        stream_.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        stream_.seekp(static_cast<std::streamoff>(start_ + position_), std::ios::beg);
        if (stream_.fail()) {
            throw io_error(errc::write_failed, "cannot rewrite the output stream", {}, {.offset = offset});
        }
    }

    void finish() override
    {
        stream_.flush();
        if (stream_.fail()) {
            throw io_error(errc::write_failed, "cannot flush the output stream");
        }
    }

private:
    std::ostream& stream_;
    std::uint64_t start_ = 0;
    std::uint64_t position_ = 0;
    bool rewritable_ = false;
};

/// The bytes of a C stream from its position to its end, read in order into memory, for a stream that cannot seek, such
/// as a pipe or the standard input, opened in binary mode: the source that a reader of such a stream needs. The read
/// waits for the end of the stream. The bytes end up in one allocation of their size.
/// @throws usage_error when stream is null.
/// @throws limit_error (errc::allocation_too_large) when the stream holds more than limits.max_allocation bytes,
///         zero being no limit.
/// @throws io_error (errc::read_failed) when the stream fails.
[[nodiscard]] inline std::unique_ptr<memory_source> read_into_memory(std::FILE* stream, const limits& limits = {})
{
    if (stream == nullptr) {
        throw usage_error(errc::invalid_argument, "the FILE stream is null");
    }
    std::vector<std::byte> data = detail::read_stream(
        [stream](std::span<std::byte> chunk) {
            errno = 0;
            const std::size_t count = std::fread(chunk.data(), 1, chunk.size(), stream);
            if (std::ferror(stream) != 0) {
                throw io_error(errc::read_failed, "cannot read the FILE stream", detail::stdio_error());
            }
            return std::pair<std::size_t, bool>(count, std::feof(stream) == 0);
        },
        limits.max_allocation);
    return std::make_unique<memory_source>(std::move(data));
}

/// The bytes of a C++ input stream from its position to its end, read in order into memory, as for a C stream: for a
/// stream that cannot seek, and binary on platforms where that matters. When the stream's exceptions() mask is set, the
/// stream's own exceptions pass through unchanged, such as that of failbit at the end of the stream.
/// @throws limit_error (errc::allocation_too_large) when the stream holds more than limits.max_allocation bytes,
///         zero being no limit.
/// @throws io_error (errc::read_failed) when the stream fails.
[[nodiscard]] inline std::unique_ptr<memory_source> read_into_memory(std::istream& stream, const limits& limits = {})
{
    std::vector<std::byte> data = detail::read_stream(
        [&stream](std::span<std::byte> chunk) {
            stream.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
            if (stream.bad()) {
                throw io_error(errc::read_failed, "cannot read the input stream");
            }
            return std::pair<std::size_t, bool>(static_cast<std::size_t>(stream.gcount()), !stream.eof());
        },
        limits.max_allocation);
    return std::make_unique<memory_source>(std::move(data));
}

} // namespace openfits
