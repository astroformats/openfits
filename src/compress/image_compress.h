// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/card.h>
#include <openfits/compression.h>
#include <openfits/image.h>
#include <openfits/types.h>

#include "compress/image_tiles.h"
#include "data/conversion.h"
#include "data/image_write.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
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

/// What a tile became: the column of its bytes in the heap and the bytes, the elements of its array (bytes, 16-bit
/// words of PLIO_1, or values of UNCOMPRESSED_DATA), the ZSCALE and ZZERO of a quantized tile, and whether it holds
/// undefined values.
struct encoded_tile
{
    tile_payload::holder column = tile_payload::holder::compressed;
    std::vector<std::byte> bytes{};
    std::uint64_t elements = 0;
    double scale = 0.0;
    double zero = 0.0;
    bool nulls = false;
};

/// How the tiles of a compressed image are encoded, as its table records it, for tiles encoded again (the editor).
struct tile_encoding
{
    tile_algorithm algorithm = tile_algorithm::rice;
    /// ZBITPIX.
    bitpix stored = bitpix::int16;
    /// BLOCKSIZE of RICE_1.
    std::size_t block_size = 32;
    /// The scale of HCOMPRESS_1 as ZVAL1 records it: relative to the noise of each tile when positive.
    float hcompress_scale = 0.0F;
    bool quantized = false;
    quantization method = quantization::no_dither;
    std::uint32_t seed = 1;
    bool plio_offset = false;
    /// BLANK of integer data, whose tiles HCOMPRESS_1 keeps lossless.
    std::optional<std::int64_t> blank{};
};

/// Encodes the stored values of a tile again, big-endian as an uncompressed image of its BITPIX holds them: the tile at
/// index of the box size, of a compressed image whose tiles encoding encodes; a quantized tile at the spacing of its
/// levels, which its ZSCALE gives, and a tile of a quantized image without one losslessly with GZIP in
/// GZIP_COMPRESSED_DATA, as it was (standard §10.1.3, §10.2).
/// @throws validation_error (errc::invalid_compression) for values that the algorithm cannot compress, as
///         compress_image() does.
[[nodiscard]] encoded_tile encode_tile_again(const tile_encoding& encoding, std::uint64_t index,
                                             std::span<const std::uint64_t> size, std::span<const std::byte> stored,
                                             std::optional<double> spacing, std::uint64_t hdu);

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

/// Compresses the image of a model of the writer, whose values of type values gives, which conversion converts into
/// its stored values, as the unit hdu: the primary HDU when primary, whose table then has ZSIMPLE and, without an
/// EXTNAME in its header, the name COMPRESSED_IMAGE. The tiles are compressed in batches of whole bands, the runs of
/// elements that hold whole tiles, rows for an image of two axes or more, each batch of at most 64 MiB of stored values
/// but for a band larger, whose values are taken from values in one call. advance receives the bytes of the stored
/// values of each batch of tiles compressed, on the calling thread.
/// @throws the exceptions of check_compression(), of convert_write(), and validation_error
///         (errc::invalid_compression) for values that the algorithm cannot compress: PLIO_1 values beyond 0 to 2^24 -
///         1, or a tile that cannot be quantized without lossless_fallback.
[[nodiscard]] compressed_unit compress_image(const image_info& image, const write_conversion& conversion,
                                             numeric_type type, const image_values& values,
                                             const compression_options& options, bool primary, std::uint64_t hdu,
                                             const std::function<void(std::uint64_t)>& advance);

} // namespace openfits::detail
