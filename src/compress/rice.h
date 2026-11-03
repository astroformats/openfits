// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// RICE_1 (standard §10.4.1): the integers of a tile coded in blocks of BLOCKSIZE pixels, the differences of successive
// pixels mapped to non-negative values and each block split at the number of low bits that its sum gives, the high
// bits in unary; a block of no information is one code, and one of too much is stored as it is. The stream is that of
// R. White's coder as CFITSIO writes it, byte for byte, for 1, 2 and 4 bytes per pixel; 8 bytes per pixel, which
// standard Table 37 permits and CFITSIO does not implement, follows the same rules with the constants of 64-bit
// values.
//
// The values are unsigned integers of BYTEPIX bytes, in the byte order of the host: differences wrap modulo their
// width, so that signed values of the same bits code the same. A decoder fills its output exactly, never reads beyond
// its input, and throws integrity_error with errc::corrupt_compressed_data for a stream that ends early or holds codes
// of no block; messages have no context, which the caller adds.

namespace openfits::detail {

/// The block sizes of standard Table 37.
inline constexpr std::size_t rice_block_16 = 16;
inline constexpr std::size_t rice_block_32 = 32;

/// The fewest bytes of a stream of count values of bytes bytes in blocks of block_size: the first value, and the code
/// of a block of no information for each block. A decoder refuses a shorter stream before it writes anything, which
/// bounds what a stream of a given size claims to decompress to.
[[nodiscard]] std::uint64_t rice_minimum_size(std::uint64_t count, std::size_t bytes, std::size_t block_size) noexcept;

/// Appends the RICE_1 stream of values to output, in blocks of block_size, 16 or 32.
void rice_encode(std::span<const std::uint8_t> values, std::size_t block_size, std::vector<std::byte>& output);
void rice_encode(std::span<const std::uint16_t> values, std::size_t block_size, std::vector<std::byte>& output);
void rice_encode(std::span<const std::uint32_t> values, std::size_t block_size, std::vector<std::byte>& output);
void rice_encode(std::span<const std::uint64_t> values, std::size_t block_size, std::vector<std::byte>& output);

/// Decodes the RICE_1 stream input of output.size() values, in blocks of block_size, into output. Bytes after the
/// last block are ignored, as a stream ends within its last byte.
void rice_decode(std::span<const std::byte> input, std::size_t block_size, std::span<std::uint8_t> output);
void rice_decode(std::span<const std::byte> input, std::size_t block_size, std::span<std::uint16_t> output);
void rice_decode(std::span<const std::byte> input, std::size_t block_size, std::span<std::uint32_t> output);
void rice_decode(std::span<const std::byte> input, std::size_t block_size, std::span<std::uint64_t> output);

} // namespace openfits::detail
