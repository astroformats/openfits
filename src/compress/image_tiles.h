// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/compression.h>
#include <openfits/hdu.h>
#include <openfits/header.h>
#include <openfits/image.h>
#include <openfits/limits.h>
#include <openfits/reader.h>
#include <openfits/table.h>
#include <openfits/types.h>

#include "core/diagnostic_log.h"
#include "io/thread_safe_source.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Compressed images (standard §10.1): the description of a ZIMAGE table from its keywords and columns, the header of
// the image that it holds, the decoding of its tiles into the stored values of that image, and the reads of its values.

namespace openfits::detail {

/// The algorithms of standard Table 36.
enum class tile_algorithm : std::uint8_t
{
    rice,
    gzip_1,
    gzip_2,
    plio,
    hcompress,
    nocompress,
};

/// The algorithm of a value of ZCMPTYPE, ZCTYPn or ZMASKCMP, without its trailing spaces: the names of Table 36, and
/// RICE_ONE of old files; nothing for another.
[[nodiscard]] std::optional<tile_algorithm> algorithm_of(std::string_view name) noexcept;

/// The value of ZCMPTYPE of an algorithm.
[[nodiscard]] std::string_view algorithm_name(tile_algorithm algorithm) noexcept;

/// A column of the table of a compressed image (standard §10.1.3): its index, the offset of its field in a row and
/// its format.
struct tile_column
{
    std::size_t index = 0;
    std::uint64_t offset = 0;
    column_format format{};
};

/// A compressed image as the keywords and the columns of its table describe it.
struct compressed_image
{
    /// The image that it holds: ZBITPIX, ZNAXISn, BSCALE and BZERO, and BLANK of integer data; and its header as the
    /// decompression restores it.
    image_info image{};
    /// ZTILEn, one for each axis, at most the axis.
    std::vector<std::uint64_t> tile{};
    tile_algorithm algorithm = tile_algorithm::rice;
    /// BLOCKSIZE and BYTEPIX of RICE_1 (Table 37).
    std::size_t block_size = 32;
    std::size_t bytepix = 4;
    /// SMOOTH of HCOMPRESS_1, which the decompression applies.
    bool smooth = false;
    /// Whether the values of a floating-point image are quantized (standard §10.2), and how.
    bool quantized = false;
    quantization method = quantization::no_dither;
    /// ZDITHER0, 1 without it.
    std::uint32_t seed = 1;
    /// The integer of null pixels of quantized tiles that no ZBLANK column gives: the ZBLANK keyword, else BLANK.
    std::optional<std::int64_t> null{};
    /// ZSCALE and ZZERO given as keywords, constants for every tile, as old files have them.
    std::optional<double> scale{};
    std::optional<double> zero{};
    /// The columns, those that exist: COMPRESSED_DATA, GZIP_COMPRESSED_DATA, UNCOMPRESSED_DATA, ZSCALE, ZZERO, ZBLANK
    /// and NULL_PIXEL_MASK.
    tile_column compressed{};
    std::optional<tile_column> gzip_compressed{};
    std::optional<tile_column> uncompressed{};
    std::optional<tile_column> scales{};
    std::optional<tile_column> zeros{};
    std::optional<tile_column> nulls{};
    std::optional<tile_column> mask{};
    /// ZMASKCMP, the algorithm of the null pixel masks.
    tile_algorithm mask_algorithm = tile_algorithm::plio;
    /// PLIO_1 under BZERO 32768, whose values CFITSIO stores plus 32768, the unsigned values of a 16-bit image.
    bool plio_offset = false;
    /// The table: one row for each tile, of row_size bytes, then the heap from heap_offset, of heap_size bytes.
    std::uint64_t rows = 0;
    std::uint64_t row_size = 0;
    std::uint64_t heap_offset = 0;
    std::uint64_t heap_size = 0;
};

/// The compressed image of a unit of kind compressed_image, with the restored header of its image unless with_header
/// is false.
/// @throws invalid_data_error (errc::invalid_compression) for keywords or columns that do not describe a compressed
///         image, unsupported_error (errc::unsupported_compression) for an algorithm of no name of Table 36, and the
///         exceptions of the description of the table.
[[nodiscard]] compressed_image describe_compressed_image(const hdu_info& unit, bool with_header = true);

/// Reports to log the forms of old compressed images that the reader takes as CFITSIO takes them, the tolerance rows
/// of compression: ZCMPTYPE = 'RICE_ONE', the info of RICE_1; ZVAL1 and ZVAL2 of RICE_1 reversed, and ZNAME2 =
/// 'NOISEBIT', the warnings of BYTEPIX read otherwise than written; ZSCALE and ZZERO as keywords, the info of constants
/// for every tile. The header is that of the unit index, whose first record lies at header_offset.
/// @throws the exceptions of log in strict mode.
void analyze_compressed_image(const header& cards, std::uint64_t index, std::uint64_t header_offset,
                              diagnostic_log& log);

/// The header of the image of a compressed image as its decompression restores it (standard §10.1.2): SIMPLE, or
/// XTENSION, BITPIX, NAXIS, NAXISn, then EXTEND and BLOCKED, or PCOUNT and GCOUNT, from their Z keywords or their
/// defaults, then CHECKSUM and DATASUM from ZHECKSUM and ZDATASUM, then every other card in its order but the keywords
/// of the table and of the compression and an EXTNAME = 'COMPRESSED_IMAGE'.
[[nodiscard]] header restored_image_header(const header& table);

/// The values of one tile and its row, as the table gives them: the bytes of the column that holds it, in the heap, and
/// its ZSCALE, ZZERO and ZBLANK.
struct tile_payload
{
    /// The column whose bytes hold the tile: COMPRESSED_DATA, or for an empty cell of it GZIP_COMPRESSED_DATA or
    /// UNCOMPRESSED_DATA.
    enum class holder : std::uint8_t
    {
        compressed,
        gzip_compressed,
        uncompressed,
    };

    holder column = holder::compressed;
    std::span<const std::byte> bytes{};
    /// The type of the elements of UNCOMPRESSED_DATA.
    column_type uncompressed_type = column_type::uint8;
    std::span<const std::byte> mask{};
    double scale = 1.0;
    double zero = 0.0;
    std::optional<std::int64_t> null{};
};

/// Decodes the tile at index, counted from 0, of image, whose box has size elements along each axis, from payload into
/// tile: its stored values, big-endian, as an uncompressed image of ZBITPIX holds them.
/// @throws integrity_error (errc::corrupt_compressed_data) for compressed bytes that do not decode to the values of the
///         tile, and invalid_data_error (errc::invalid_compression) for a tile of no form that its image allows.
void decode_tile(const compressed_image& image, std::uint64_t index, std::span<const std::uint64_t> size,
                 const tile_payload& payload, std::span<std::byte> tile);

/// Reads the values of the compressed image of unit, as reader::read_pixels() does, with the tiles of the cache of
/// options kept under owner, the identity of the reader.
/// @throws the exceptions of reader::read_pixels(), and limit_error (errc::too_many_tiles, errc::tile_too_large) for an
///         image beyond limits.
pixel_read_result read_compressed_pixels(const thread_safe_source& source, const hdu_info& unit,
                                         const compressed_image& image, numeric_type type,
                                         std::span<std::byte> destination, const pixel_read_options& options,
                                         const openfits::limits& limits, const std::shared_ptr<const void>& owner);

} // namespace openfits::detail
