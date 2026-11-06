// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/io.h>

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

// GZIP_1 (standard §10.4.2): the gzip format of RFC 1952 over deflate, through zlib. zlib counts bytes in 32-bit
// integers, so the streaming API is fed in pieces, and no tile is too large for it. A decoder fills its output exactly
// or throws integrity_error with errc::corrupt_compressed_data; out of memory is std::bad_alloc, and a failure of zlib
// for another reason than the data, such as a version mismatch, unsupported_error (errc::unsupported_compression).
// Messages have no context; the caller adds it.

namespace openfits::detail {

/// The most that zlib takes or gives in one call, which its 32-bit counters can hold.
inline constexpr std::size_t zlib_max_piece = 0xFFFF'FFFF;

/// The most bytes that deflate makes of one byte, which bounds what a stream of a given size decompresses to.
inline constexpr std::size_t deflate_max_expansion = 1032;

/// Decompresses one gzip stream (RFC 1952), or a zlib stream (RFC 1950), which some writers put in GZIP tiles, into
/// exactly output.size() bytes. Bytes after the end of the stream are ignored. zlib gets the input and the output in
/// pieces of at most max_piece bytes, which only tests set lower.
void gzip_decompress(std::span<const std::byte> input, std::span<std::byte> output,
                     std::size_t max_piece = zlib_max_piece);

/// The level of zlib of GZIP_1 and GZIP_2 tiles and of the column chunks of compressed tables when the options give
/// none: 1, the fastest, which fpack and CFITSIO write. The default of zlib, 6, took 1.3 to 6 times their time to
/// encode tiles of 16 rows and of 256 x 256 pixels, for files a few percent smaller.
inline constexpr int default_tile_level = 1;

/// Compresses inputs one after another into gzip streams at a level of zlib, from 1 to 9 or -1 for its default, with
/// one deflate state, which deflateReset() makes that of a new stream for each. The gzip header of each stream is the
/// same on every host: no name, no time, and the operating system unknown (255), so that a file is reproducible.
class gzip_encoder
{
public:
    explicit gzip_encoder(int level, std::size_t max_piece = zlib_max_piece);
    gzip_encoder(const gzip_encoder&) = delete;
    gzip_encoder& operator=(const gzip_encoder&) = delete;
    gzip_encoder(gzip_encoder&&) = delete;
    gzip_encoder& operator=(gzip_encoder&&) = delete;
    ~gzip_encoder();

    /// Appends the gzip stream of input to output.
    void compress(std::span<const std::byte> input, std::vector<std::byte>& output);

private:
    // The z_stream, at an address that does not change, which zlib requires.
    struct state;
    std::unique_ptr<state> state_;
    std::size_t max_piece_;
};

/// Inflates a gzip file (RFC 1952) given in pieces: its members one after the other, as gzip(1) inflates them, each
/// checked against its CRC-32 and its length by zlib; zero bytes after the last member are ignored, as the padding of a
/// tape, and other bytes refused. Its failures are those of gzip_decompress(), with the offset in the input where the
/// inflation stopped as their context. zlib gets the input and the output in pieces of at most max_piece bytes.
class gzip_file_inflater
{
public:
    explicit gzip_file_inflater(std::size_t max_piece = zlib_max_piece);
    gzip_file_inflater(const gzip_file_inflater&) = delete;
    gzip_file_inflater& operator=(const gzip_file_inflater&) = delete;
    gzip_file_inflater(gzip_file_inflater&&) = delete;
    gzip_file_inflater& operator=(gzip_file_inflater&&) = delete;
    ~gzip_file_inflater();

    /// The next piece of the input, after the pieces given so far, which must stay valid until needs_input().
    void give(std::span<const std::byte> input) noexcept;

    /// True when the input given so far is used up.
    [[nodiscard]] bool needs_input() const noexcept;

    /// Inflates into output as far as the input given allows, and returns the number of bytes written: fewer than
    /// output.size() only when the input given is used up.
    std::size_t inflate(std::span<std::byte> output);

    /// Checks that the input, given whole, ends after a member: throws when it ends within one, or held none.
    void finish() const;

private:
    struct state;
    std::unique_ptr<state> state_;
};

/// Compresses bytes given in pieces into one gzip member (RFC 1952) at a level of zlib, from 0 to 9 or -1 for its
/// default, its header that of gzip_encoder, and writes the compressed bytes to target as they come.
class gzip_member_deflater
{
public:
    gzip_member_deflater(int level, output_sink& target, std::size_t max_piece = zlib_max_piece);
    gzip_member_deflater(const gzip_member_deflater&) = delete;
    gzip_member_deflater& operator=(const gzip_member_deflater&) = delete;
    gzip_member_deflater(gzip_member_deflater&&) = delete;
    gzip_member_deflater& operator=(gzip_member_deflater&&) = delete;
    ~gzip_member_deflater();

    /// Compresses input, which follows the bytes given so far.
    void compress(std::span<const std::byte> input);

    /// Ends the member: writes the rest of the compressed bytes and the trailer of the member.
    void finish();

private:
    struct state;
    std::unique_ptr<state> state_;
};

} // namespace openfits::detail
