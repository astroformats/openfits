// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

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

} // namespace openfits::detail
