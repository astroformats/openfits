// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

/// @file
/// The options of tiled compression (standard §10): the algorithm of an image or of the columns of a table, the shape
/// of their tiles, and the quantization of floating-point images.

namespace openfits {

/// An algorithm of tiled compression (standard §10.4, Table 36), or none.
enum class compression : std::uint8_t
{
    /// Not compressed: the unit is an image or a table of its own.
    none,
    /// The algorithm of the data: RICE_1 for integers of up to 32 bits and for floating-point values quantized with a
    /// level; GZIP_2 for 64-bit integers and for floating-point values kept as they are. For a table, that of the type
    /// of each column.
    automatic,
    /// RICE_1 (standard §10.4.1): lossless, for integers and quantized floating-point values.
    rice,
    /// GZIP_1 (standard §10.4.2): lossless, for any values.
    gzip_1,
    /// GZIP_2: GZIP_1 of the bytes of the values shuffled by significance, lossless, for any values.
    gzip_2,
    /// PLIO_1 (standard §10.4.3): lossless, for integers from 0 to 2^24 - 1, such as masks.
    plio,
    /// HCOMPRESS_1 (standard §10.4.4): for two-dimensional tiles of integers and quantized floating-point values,
    /// lossless at a scale of 0.
    hcompress,
    /// NOCOMPRESS (Table 36): the tiles of a compressed unit stored as they are.
    nocompress,
};

/// How floating-point values are quantized into integers (standard §10.2.1).
enum class quantization : std::uint8_t
{
    /// NO_DITHER: each value rounded to the nearest level (Eq. 12).
    no_dither,
    /// SUBTRACTIVE_DITHER_1: each value dithered by a random number of standard Appendix I before it is rounded
    /// (Eq. 13), which the read subtracts.
    subtractive_dither_1,
    /// SUBTRACTIVE_DITHER_2: SUBTRACTIVE_DITHER_1, with the values that are exactly 0 kept exact.
    subtractive_dither_2,
};

/// How a unit is compressed (standard §10): an image in tiles of pixels, each compressed alone (§10.1), or a table in
/// tiles of rows, each column of a tile compressed alone (§10.3).
///
/// The defaults are those of fpack, but for floating-point images, which they keep lossless: tiles of one row, RICE_1
/// for integer images and GZIP_2 for floating-point ones unless quantize_level asks for a quantization, with
/// SUBTRACTIVE_DITHER_1 and a seed taken from the data.
///
/// A value: copies are independent.
struct compression_options
{
    /// The algorithm of every tile of an image, or of every column of a table that columns gives none.
    openfits::compression type = openfits::compression::automatic;
    /// The length of the tiles of an image along each axis, ZTILEn (standard §10.1.2): an entry for each axis, 0 for
    /// the whole axis. Empty for the default: one row, the first axis whole and one element along each other; for
    /// HCOMPRESS_1, whose tiles are two-dimensional, the whole image up to 30 rows, and otherwise 16 rows, or the
    /// number from 14 to 30 that leaves the last tile at least 4 rows.
    std::vector<std::uint64_t> tile{};
    /// The quantization of the values of a floating-point image (standard §10.2): positive, the spacing of the levels
    /// is the noise of each tile divided by it, as fpack's -q measures it; negative, the spacing itself, its absolute
    /// value. Empty keeps the values as they are, which only GZIP_1, GZIP_2 and NOCOMPRESS store. Not used for
    /// integer images and tables.
    std::optional<double> quantize_level{};
    /// The dithering of the quantization.
    openfits::quantization quantize_method = openfits::quantization::subtractive_dither_1;
    /// The seed of the dithering, ZDITHER0, from 1 to 10000 (standard §10.2.1); empty for one taken from the values of
    /// the first tile, so that the same image gives the same file.
    std::optional<std::uint32_t> dither_seed{};
    /// The scale of HCOMPRESS_1 (standard §10.4.4, Table 39), 0 for lossless compression: positive, a factor of the
    /// noise of each tile, as fpack's -s takes it; negative, the scale itself, its absolute value.
    double hcompress_scale = 0.0;
    /// Smooth a lossy HCOMPRESS_1 image when it is decompressed, which the file records for the readers.
    bool hcompress_smooth = false;
    /// The pixels of a block of RICE_1, 16 or 32 (standard Table 37).
    std::uint32_t rice_block_size = 32;
    /// Compress a 64-bit integer image with RICE_1, with 8 bytes per pixel (standard Table 37), which readers built on
    /// CFITSIO do not decompress; RICE_1 refuses such an image otherwise.
    bool rice_bytepix_8 = false;
    /// The level of zlib for GZIP_1 and GZIP_2, from 1, the fastest, to 9, the smallest; empty for the default of zlib.
    std::optional<int> gzip_level{};
    /// Store a tile of a floating-point image that cannot be quantized, such as one whose range is beyond 32-bit
    /// integers, losslessly with GZIP in the column GZIP_COMPRESSED_DATA (standard §10.1.3), as fpack does; false
    /// refuses such an image.
    bool lossless_fallback = true;
    /// For a table, the algorithm of each column, in their order: compression::automatic for the default of its type,
    /// which the directives FZALGn and FZALGOR of its header give otherwise (standard §10.3.3). Empty for the default
    /// of every column.
    std::vector<openfits::compression> columns{};
    /// For a table, the rows of each tile, ZTILELEN (standard §10.3.1); 0 for the directive FZTILELN of its header, or
    /// else the default.
    std::uint64_t tile_rows = 0;
};

} // namespace openfits
