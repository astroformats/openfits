// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/compression.h>
#include <openfits/hdu.h>
#include <openfits/header.h>
#include <openfits/limits.h>
#include <openfits/table.h>

#include "compress/image_compress.h"
#include "compress/image_tiles.h"
#include "io/thread_safe_source.h"
#include "table/table_write.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

// Compressed tables (standard §10.3): the description of a ZTABLE table from its keywords, the header of the binary
// table that it holds, the decompression of its data, and the compression of a table of the writer.
//
// Each column of a tile of rows is compressed alone: the bytes of its fields, big-endian, in the order of the rows,
// with RICE_1 for 8, 16 and 32-bit integers, GZIP_1, GZIP_2 with the bytes of its numbers shuffled, or NOCOMPRESS; a
// column of variable-length arrays has each array compressed alone in the heap, and in its cell the descriptors of the
// table and those of the compressed arrays, in that order, as CFITSIO writes them and every file has them, compressed
// with GZIP_1. Standard §10.3.6 gives the two kinds of descriptors in the other order.

namespace openfits::detail {

/// A column of a compressed table: its format in the table that it holds (ZFORMn), the algorithm of its tiles (ZCTYPn),
/// and its field in the rows of that table.
struct compressed_column
{
    column_format format{};
    tile_algorithm algorithm = tile_algorithm::gzip_2;
    std::uint64_t offset = 0;
    std::uint64_t field = 0;
};

/// A compressed table as its keywords describe it.
struct compressed_table
{
    /// The table that it holds: ZNAXIS2 rows of ZNAXIS1 bytes, and a heap of ZPCOUNT bytes from ZTHEAP, or from the
    /// end of the rows without it.
    std::uint64_t rows = 0;
    std::uint64_t row_size = 0;
    std::uint64_t heap_offset = 0;
    std::uint64_t heap_size = 0;
    /// ZTILELEN: the rows of each tile, at most the rows of the table.
    std::uint64_t tile_rows = 1;
    std::vector<compressed_column> columns{};
    /// The compressed table itself.
    table_info table{};
};

/// The compressed table of a unit of kind compressed_table.
/// @throws invalid_data_error (errc::invalid_compression) for keywords that do not describe a compressed table, and
///         unsupported_error (errc::unsupported_compression) for an algorithm that standard §10.3.5 does not give.
[[nodiscard]] compressed_table describe_compressed_table(const hdu_info& unit);

/// The header of the table that a compressed table holds, as the decompression restores it (standard §10.3.1):
/// NAXIS1, NAXIS2, PCOUNT, THEAP and each TFORMn from their Z keywords, CHECKSUM and DATASUM from ZHECKSUM and
/// ZDATASUM, without the keywords of the compression.
[[nodiscard]] header restored_table_header(const header& compressed);

/// The unit of the table that the compressed table of unit holds, whose data lie at offset 0: an hdu_info for the reads
/// of its columns from its decompressed data.
[[nodiscard]] hdu_info restored_table_unit(const hdu_info& unit, const compressed_table& table);

/// The data of the table that a compressed table holds, rows and heap, decompressed from source within limits.
/// @throws limit_error when they take more than limits.max_allocation or their heap more than limits.max_heap_bytes;
///         integrity_error (errc::corrupt_compressed_data) for compressed bytes that do not decode to their columns;
///         invalid_data_error (errc::invalid_descriptor) for a descriptor outside its heap; and the exceptions of the
///         source.
[[nodiscard]] std::vector<std::byte> decompress_table(const thread_safe_source& source, const hdu_info& unit,
                                                      const compressed_table& table, const openfits::limits& limits);

/// Compresses the table of a model of the writer, whose data, rows and heap, are data, as the unit hdu (standard
/// §10.3): tiles of options.tile_rows rows, or of the directive FZTILELN of its header, or of about 10 MB; each column
/// with the algorithm of options.columns, or of the directives FZALGn and FZALGOR, or of options.type, or the default
/// of its type. advance receives the bytes of the data of each tile compressed.
/// @throws validation_error (errc::invalid_compression) for an algorithm of options.columns that a column cannot take,
///         and usage_error (errc::invalid_argument) for options.columns of more entries than columns.
[[nodiscard]] compressed_unit compress_table(const table_model& model, std::span<const std::byte> data,
                                             const compression_options& options, std::uint64_t hdu,
                                             const std::function<void(std::uint64_t)>& advance);

} // namespace openfits::detail
