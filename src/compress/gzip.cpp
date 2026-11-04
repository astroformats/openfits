// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "compress/gzip.h"

#include <openfits/error.h>

#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <new>
#include <string>

namespace openfits::detail {

namespace {

static_assert(zlib_max_piece == std::numeric_limits<uInt>::max());

// The window of deflate, and the offsets of its window bits that select the gzip format alone, or either format with
// its header detected.
constexpr int window_bits = 15;
constexpr int gzip_format = 16;
constexpr int detected_format = 32;
// The memory level of deflateInit(), which deflateInit2() needs given.
constexpr int memory_level = 8;
// The operating system of the gzip header: unknown (RFC 1952, §2.3.1).
constexpr int unknown_system = 255;

[[noreturn]] void throw_corrupt(const std::string& message)
{
    throw integrity_error(errc::corrupt_compressed_data, message);
}

[[noreturn]] void throw_failure(int result, const z_stream& stream)
{
    if (result == Z_MEM_ERROR) {
        throw std::bad_alloc();
    }
    throw unsupported_error(errc::unsupported_compression,
                            "zlib failed with error " + std::to_string(result) +
                                (stream.msg != nullptr ? ": " + std::string(stream.msg) : ""));
}

Bytef* bytes_of(std::byte* data) noexcept
{
    return reinterpret_cast<Bytef*>(data);
}

// zlib takes its input through a pointer to non-const bytes, which it does not change.
Bytef* bytes_of(const std::byte* data) noexcept
{
    return const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
}

// The part of size bytes that zlib gets in one call.
uInt piece(std::size_t size, std::size_t max_piece) noexcept
{
    return static_cast<uInt>(std::min(size, std::clamp<std::size_t>(max_piece, 1, zlib_max_piece)));
}

// The input or output of a stream, given to zlib a piece at a time.
template <typename Byte> class buffer
{
public:
    buffer(std::span<Byte> data, std::size_t max_piece) noexcept : data_(data), max_piece_(max_piece) {}

    // Gives zlib the next piece when it has used up the previous one.
    void refill(Bytef*& next, uInt& available) noexcept
    {
        if (available == 0 && given_ < data_.size()) {
            available = piece(data_.size() - given_, max_piece_);
            next = bytes_of(data_.data() + given_);
            given_ += available;
        }
    }

    // The bytes that zlib has not reached, given the counter of the current piece.
    [[nodiscard]] std::size_t left(uInt available) const noexcept
    {
        return data_.size() - given_ + available;
    }

private:
    std::span<Byte> data_;
    std::size_t max_piece_;
    std::size_t given_ = 0;
};

class inflater
{
public:
    explicit inflater(int format = detected_format)
    {
        const int result = inflateInit2(&stream_, window_bits + format);
        if (result != Z_OK) {
            throw_failure(result, stream_);
        }
    }
    inflater(const inflater&) = delete;
    inflater& operator=(const inflater&) = delete;
    inflater(inflater&&) = delete;
    inflater& operator=(inflater&&) = delete;
    ~inflater()
    {
        (void)inflateEnd(&stream_);
    }

    z_stream& stream() noexcept
    {
        return stream_;
    }

private:
    z_stream stream_{};
};

class deflater
{
public:
    explicit deflater(int level)
    {
        const int result =
            deflateInit2(&stream_, level, Z_DEFLATED, window_bits + gzip_format, memory_level, Z_DEFAULT_STRATEGY);
        if (result != Z_OK) {
            throw_failure(result, stream_);
        }
    }
    deflater(const deflater&) = delete;
    deflater& operator=(const deflater&) = delete;
    deflater(deflater&&) = delete;
    deflater& operator=(deflater&&) = delete;
    ~deflater()
    {
        (void)deflateEnd(&stream_);
    }

    z_stream& stream() noexcept
    {
        return stream_;
    }

private:
    z_stream stream_{};
};

} // namespace

void gzip_decompress(std::span<const std::byte> input, std::span<std::byte> output, std::size_t max_piece)
{
    inflater state;
    z_stream& stream = state.stream();
    buffer<const std::byte> in(input, max_piece);
    buffer<std::byte> out(output, max_piece);
    // Once output is full, zlib gets this byte as its output, so that a stream with more data shows.
    std::byte probe{};
    bool full = false;

    for (;;) {
        in.refill(stream.next_in, stream.avail_in);
        if (!full) {
            out.refill(stream.next_out, stream.avail_out);
            if (stream.avail_out == 0) {
                full = true;
                stream.next_out = bytes_of(&probe);
                stream.avail_out = 1;
            }
        }
        const int result = inflate(&stream, Z_NO_FLUSH);
        if (full && stream.avail_out == 0) {
            throw_corrupt("the gzip data decode to more than " + std::to_string(output.size()) + " bytes");
        }
        if (result == Z_STREAM_END) {
            break;
        }
        switch (result) {
        case Z_OK:
            continue;
        case Z_BUF_ERROR:
            // No progress: the stream needs more input than there is.
            throw_corrupt("the gzip data end before the end of the stream");
        case Z_NEED_DICT:
            throw_corrupt("the gzip data need a preset dictionary");
        case Z_DATA_ERROR:
            throw_corrupt("the gzip data are corrupt" + (stream.msg != nullptr ? ": " + std::string(stream.msg) : ""));
        default:
            throw_failure(result, stream);
        }
    }

    if (const std::size_t missing = full ? 0 : out.left(stream.avail_out); missing != 0) {
        throw_corrupt("the gzip data decode to " + std::to_string(output.size() - missing) + " bytes, not " +
                      std::to_string(output.size()));
    }
}

struct gzip_encoder::state
{
    deflater stream;
    gz_header header{};
    // Whether the stream has compressed an input, and needs deflateReset() before the next.
    bool used = false;

    explicit state(int level) : stream(level)
    {
        header.os = unknown_system;
    }
};

gzip_encoder::gzip_encoder(int level, std::size_t max_piece)
    : state_(std::make_unique<state>(level)), max_piece_(max_piece)
{}

gzip_encoder::~gzip_encoder() = default;

void gzip_encoder::compress(std::span<const std::byte> input, std::vector<std::byte>& output)
{
    z_stream& stream = state_->stream.stream();
    if (state_->used) {
        // Keeps the level and the memory of the stream, and starts a new one, which needs its header again.
        const int result = deflateReset(&stream);
        if (result != Z_OK) {
            throw_failure(result, stream);
        }
    }
    state_->used = true;
    if (const int result = deflateSetHeader(&stream, &state_->header); result != Z_OK) {
        throw_failure(result, stream);
    }
    // A compression that threw may have left input in the stream.
    stream.next_in = Z_NULL;
    stream.avail_in = 0;
    buffer<const std::byte> in(input, max_piece_);
    const std::size_t start = output.size();
    // More than deflate needs for data it cannot compress, which zlib bounds at a little more than the data
    // (deflateBound()); the output still grows if another implementation of zlib ever needs more.
    output.resize(start + input.size() + (input.size() / 8) + 64);
    std::size_t written = 0;

    for (;;) {
        in.refill(stream.next_in, stream.avail_in);
        if (written == output.size() - start) {
            output.resize(output.size() + ((output.size() - start) / 2));
        }
        const std::size_t room = output.size() - start - written;
        stream.next_out = bytes_of(output.data() + start + written);
        stream.avail_out = piece(room, max_piece_);
        const uInt offered = stream.avail_out;
        const int flush = in.left(stream.avail_in) == stream.avail_in ? Z_FINISH : Z_NO_FLUSH;
        const int result = deflate(&stream, flush);
        written += offered - stream.avail_out;
        if (result == Z_STREAM_END) {
            break;
        }
        if (result != Z_OK && result != Z_BUF_ERROR) {
            throw_failure(result, stream);
        }
    }
    output.resize(start + written);
}

namespace {

// Where a gzip file is between the members of its input.
enum class member_phase : std::uint8_t
{
    within,  // within a member, or at the start of the first
    between, // after a member, before another or zeros
    zeros,   // in the zeros after the last member
};

// The first byte of a gzip member (RFC 1952, §2.3.1).
constexpr Bytef gzip_magic = 0x1F;

} // namespace

struct gzip_file_inflater::state
{
    explicit state(std::size_t piece) : stream(gzip_format), max_piece(piece) {}

    // Gives zlib the next piece of the input when it has used up the previous one.
    void refill() noexcept
    {
        z_stream& zlib = stream.stream();
        if (zlib.avail_in == 0 && !input.empty()) {
            zlib.avail_in = piece(input.size(), max_piece);
            zlib.next_in = bytes_of(input.data());
            input = input.subspan(zlib.avail_in);
        }
    }

    [[noreturn]] void corrupt(const std::string& message) const
    {
        throw integrity_error(errc::corrupt_compressed_data, message, {.offset = consumed});
    }

    inflater stream;
    std::size_t max_piece;
    // The input given and not yet given to zlib.
    std::span<const std::byte> input{};
    member_phase phase = member_phase::within;
    // Whether zlib filled the output of its last call within a member, and may hold more of it without more input: the
    // rest of a match, which it gives at the next call.
    bool pending = false;
    // The bytes of the input that zlib has used, those after the last member included.
    std::uint64_t consumed = 0;
};

gzip_file_inflater::gzip_file_inflater(std::size_t max_piece) : state_(std::make_unique<state>(max_piece)) {}

gzip_file_inflater::~gzip_file_inflater() = default;

void gzip_file_inflater::give(std::span<const std::byte> input) noexcept
{
    state_->input = input;
}

bool gzip_file_inflater::needs_input() const noexcept
{
    return state_->stream.stream().avail_in == 0 && state_->input.empty() && !state_->pending;
}

std::size_t gzip_file_inflater::inflate(std::span<std::byte> output)
{
    state& current = *state_;
    z_stream& zlib = current.stream.stream();
    std::size_t produced = 0;
    while (produced < output.size()) {
        current.refill();
        if (zlib.avail_in == 0 && !current.pending) {
            break;
        }
        if (current.phase == member_phase::between) {
            // Another member, or the zeros that pad the file to the end, as on a tape.
            if (*zlib.next_in == gzip_magic) {
                if (const int result = inflateReset(&zlib); result != Z_OK) {
                    throw_failure(result, zlib);
                }
                current.phase = member_phase::within;
            } else if (*zlib.next_in == 0) {
                current.phase = member_phase::zeros;
            } else {
                current.corrupt("the bytes after the last gzip member are neither zeros nor another member");
            }
            continue;
        }
        if (current.phase == member_phase::zeros) {
            const std::span<const Bytef> rest(zlib.next_in, zlib.avail_in);
            const auto other = std::ranges::find_if(rest, [](Bytef value) { return value != 0; });
            current.consumed += static_cast<std::uint64_t>(other - rest.begin());
            if (other != rest.end()) {
                current.corrupt("the zeros after the last gzip member are followed by other bytes");
            }
            zlib.next_in += zlib.avail_in;
            zlib.avail_in = 0;
            continue;
        }
        const uInt offered_in = zlib.avail_in;
        zlib.next_out = bytes_of(output.data() + produced);
        zlib.avail_out = piece(output.size() - produced, current.max_piece);
        const uInt offered_out = zlib.avail_out;
        const int result = ::inflate(&zlib, Z_NO_FLUSH);
        current.consumed += offered_in - zlib.avail_in;
        produced += offered_out - zlib.avail_out;
        current.pending = zlib.avail_out == 0 && result != Z_STREAM_END;
        switch (result) {
        case Z_STREAM_END:
            current.phase = member_phase::between;
            break;
        case Z_OK:
        case Z_BUF_ERROR:
            // Z_BUF_ERROR: no progress, which needs more input or more room, both given at the next turn.
            break;
        case Z_NEED_DICT:
            current.corrupt("the gzip data need a preset dictionary");
        case Z_DATA_ERROR:
            current.corrupt("the gzip data are corrupt" + (zlib.msg != nullptr ? ": " + std::string(zlib.msg) : ""));
        default:
            throw_failure(result, zlib);
        }
    }
    return produced;
}

void gzip_file_inflater::finish() const
{
    if (state_->consumed == 0) {
        state_->corrupt("the input holds no gzip member");
    }
    if (state_->phase == member_phase::within) {
        state_->corrupt("the gzip data end before the end of their last member");
    }
}

namespace {

// The compressed bytes that a deflater gathers before it writes them to its target.
constexpr std::size_t deflated_chunk = std::size_t{256} << 10;

} // namespace

struct gzip_member_deflater::state
{
    state(int level, output_sink& sink, std::size_t piece)
        : stream(level), target(&sink), max_piece(piece), chunk(deflated_chunk)
    {
        header.os = unknown_system;
        if (const int result = deflateSetHeader(&stream.stream(), &header); result != Z_OK) {
            throw_failure(result, stream.stream());
        }
    }

    // Deflates input with flush until zlib has taken all of it, and for Z_FINISH until the member ends, writing each
    // chunk of compressed bytes to the target as it fills.
    void run(std::span<const std::byte> input, int flush)
    {
        z_stream& zlib = stream.stream();
        buffer<const std::byte> in(input, max_piece);
        for (;;) {
            in.refill(zlib.next_in, zlib.avail_in);
            const bool last = in.left(zlib.avail_in) == zlib.avail_in;
            zlib.next_out = bytes_of(chunk.data());
            zlib.avail_out = static_cast<uInt>(chunk.size());
            const int result = deflate(&zlib, last ? flush : Z_NO_FLUSH);
            if (result != Z_OK && result != Z_BUF_ERROR && result != Z_STREAM_END) {
                throw_failure(result, zlib);
            }
            const std::size_t made = chunk.size() - zlib.avail_out;
            if (made != 0) {
                target->write(std::span<const std::byte>(chunk).first(made));
            }
            // zlib has taken all of the input once it leaves room in the chunk; Z_FINISH ends with Z_STREAM_END.
            if (result == Z_STREAM_END || (flush == Z_NO_FLUSH && last && zlib.avail_out != 0)) {
                return;
            }
        }
    }

    deflater stream;
    gz_header header{};
    output_sink* target;
    std::size_t max_piece;
    std::vector<std::byte> chunk;
};

gzip_member_deflater::gzip_member_deflater(int level, output_sink& target, std::size_t max_piece)
    : state_(std::make_unique<state>(level, target, max_piece))
{}

gzip_member_deflater::~gzip_member_deflater() = default;

void gzip_member_deflater::compress(std::span<const std::byte> input)
{
    if (!input.empty()) {
        state_->run(input, Z_NO_FLUSH);
    }
}

void gzip_member_deflater::finish()
{
    state_->run({}, Z_FINISH);
}

} // namespace openfits::detail
