// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// HCOMPRESS_1 (standard §10.4.4): R. White's H-compress of a two-dimensional tile, the H-transform, the quantization
// of its coefficients by the scale, and the quadtree coding of their bit planes, as CFITSIO writes it, byte for byte;
// and the inverse, with the optional smoothing of a lossy tile.
//
// A tile is nx rows of ny values, value (i, j) at index i x ny + j: ny is the row length, the extent of the tile along
// the first of its two axes (which varies fastest), and nx the number of rows, its extent along the second. CFITSIO's
// coder calls them so, the reverse of the FITS usage, and the stream holds nx first, then ny.
//
// Both coders compute in 64 bits whatever the type of the values. CFITSIO computes in 32 bits for integers of 8 and
// 16 bits, which gives the same streams wherever its arithmetic does not overflow, and streams that do not decode to
// the tile where it does. A tile of one row or one column, which CFITSIO never writes, leaves two quadrants without
// cells: the encoder writes each of their planes as the nybble 0 of direct coding alone, and the decoder also reads the
// quadtree of nothing that CFITSIO's coder would write there. A tile of one value is its first coefficient alone.
//
// A decoder fills its output exactly, never reads beyond its input, and throws integrity_error with
// errc::corrupt_compressed_data for a stream that is malformed, ends early, describes another tile than nx x ny, or
// holds coefficients beyond the range of the 64-bit inverse transform; messages have no context, which the caller adds.
// Bytes after the sign bits are ignored.

namespace openfits::detail {

/// The largest magnitude of a value that hcompress_encode() takes for a tile of nx x ny values at the scale: its
/// coefficients then stay below 2^60, the range that the 64-bit coder of CFITSIO writes exactly, and below 2^53 at a
/// scale above 1, which leaves the smoothing of a decoder room in 64 bits, so that hcompress_decode() reads every
/// stream that hcompress_encode() writes, smoothed or not. For any tile of up to 2^20 values along each axis, it is
/// above 2^31.
[[nodiscard]] std::uint64_t hcompress_max_magnitude(std::size_t nx, std::size_t ny, std::int64_t scale) noexcept;

/// Appends the H-compress stream of the tile values, nx x ny integers, to output, at a scale of 0 or more: 0 and 1 are
/// lossless, and a larger scale discards more of the noise.
/// @throws usage_error (errc::invalid_argument) for a tile of no value, a side beyond 2^31 - 1, values.size() other
///         than nx x ny, a scale below 0 or beyond 2^31 - 1, or a value whose magnitude exceeds
///         hcompress_max_magnitude().
void hcompress_encode(std::span<const std::int32_t> values, std::size_t nx, std::size_t ny, std::int64_t scale,
                      std::vector<std::byte>& output);
void hcompress_encode(std::span<const std::int64_t> values, std::size_t nx, std::size_t ny, std::int64_t scale,
                      std::vector<std::byte>& output);

/// Decodes the H-compress stream input of a tile of nx x ny integers into output, which holds them, smoothed when
/// smooth is true and the tile was compressed with a scale above 1. The 32-bit decoder refuses a value beyond 32 bits
/// as corrupt data.
/// @throws usage_error (errc::invalid_argument) for a tile of no value, a side beyond 2^31 - 1, or output.size() other
///         than nx x ny.
void hcompress_decode(std::span<const std::byte> input, std::size_t nx, std::size_t ny, bool smooth,
                      std::span<std::int32_t> output);
void hcompress_decode(std::span<const std::byte> input, std::size_t nx, std::size_t ny, bool smooth,
                      std::span<std::int64_t> output);

/// The scale that the header of the H-compress stream input gives, 0 and 1 for a lossless tile; 0 for input shorter
/// than a header, which hcompress_decode() refuses.
[[nodiscard]] std::int64_t hcompress_scale(std::span<const std::byte> input) noexcept;

} // namespace openfits::detail
