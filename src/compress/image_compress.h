// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/card.h>
#include <openfits/compression.h>
#include <openfits/image.h>
#include <openfits/types.h>

#include "compress/image_tiles.h"
#include "data/conversion.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

// The writes of compressed images (standard §10.1): the algorithm and the tiles that the options of a unit ask for,
// checked against its image, and the table that holds its tiles, compressed in parallel.

namespace openfits::detail {

/// A unit of the writer, compressed: the structural cards of its table, after which the cards of its header come, and
/// the bytes of its data, the rows of the table and then its heap, without their fill.
struct compressed_unit
{
    std::vector<card> cards{};
    std::vector<std::byte> data{};
};

/// The algorithm of options for values of the BITPIX stored: that of compression::automatic resolved, RICE_1 for
/// integers of up to 32 bits and for quantized floating-point values, GZIP_2 for the others.
[[nodiscard]] tile_algorithm resolve_algorithm(const compression_options& options, bitpix stored) noexcept;

/// The tiles that options give an image of the axes, for the algorithm: ZTILEn of each axis.
/// @throws usage_error (errc::invalid_argument) for a tile shape without an entry for each axis; validation_error
///         (errc::invalid_compression) for tiles that HCOMPRESS_1 cannot compress: tiles that are not two-dimensional,
///         or of fewer than 4 pixels along an axis, or that leave fewer than 4 pixels in the last tile.
[[nodiscard]] std::vector<std::uint64_t> resolve_tiles(const compression_options& options,
                                                       std::span<const std::uint64_t> axes, tile_algorithm algorithm,
                                                       std::uint64_t hdu);

/// Checks that options can compress an image of the BITPIX stored and the axes: the errors of writer::add_image() of
/// a compression.
/// @throws usage_error (errc::invalid_argument) for an option outside its values; validation_error
///         (errc::invalid_compression) for an image that the algorithm cannot compress.
void check_compression(const compression_options& options, bitpix stored, std::span<const std::uint64_t> axes,
                       std::uint64_t hdu);

/// Compresses the image of a model of the writer, whose values of type are data, which conversion converts into its
/// stored values, as the unit hdu: the primary HDU when primary, whose table then has ZSIMPLE and, without an EXTNAME
/// in its header, the name COMPRESSED_IMAGE. advance receives the bytes of the stored values of each batch of tiles
/// compressed, on the calling thread.
/// @throws the exceptions of check_compression(), of convert_write(), and validation_error
///         (errc::invalid_compression) for values that the algorithm cannot compress: PLIO_1 values beyond 0 to 2^24 -
///         1, or a tile that cannot be quantized without lossless_fallback.
[[nodiscard]] compressed_unit compress_image(const image_info& image, const write_conversion& conversion,
                                             numeric_type type, std::span<const std::byte> data,
                                             const compression_options& options, bool primary, std::uint64_t hdu,
                                             const std::function<void(std::uint64_t)>& advance);

} // namespace openfits::detail
