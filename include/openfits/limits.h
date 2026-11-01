// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/export.h>

#include <cstdint>

/// @file
/// Safety limits for reading untrusted files.

namespace openfits {

/// Safety limits for reading files from untrusted sources. Zero means no limit.
///
/// Every size that a file declares, in its headers, its descriptors and its compressed tiles, is checked against these
/// limits before anything of that size is allocated or read. The defaults accept every file a real application is
/// expected to write, the largest in use among them: a frame of the Euclid VIS camera, with 433 units; an image
/// sequence of Siril, with one unit per frame; a mosaic of 168,000 x 168,000 pixels, 225 GB as floating-point values.
/// Lower them for services that open files from unknown parties.
struct limits
{
    /// Number of header and data units that a reader walks: a unit beyond it is unavailable.
    std::uint64_t max_hdus = 100'000;
    /// Size of one header, in bytes, the block of its END card included. A header of a million cards takes 80 MB of
    /// text, and about 200 MB once parsed.
    std::uint64_t max_header_bytes = std::uint64_t{128} << 20;
    /// Size of the headers of every unit that a reader has walked, which it keeps, in bytes.
    std::uint64_t max_total_header_bytes = std::uint64_t{1} << 30;
    /// Size of a single allocation, in bytes.
    std::uint64_t max_allocation = std::uint64_t{16} << 30;
    /// Size of the heap of a binary table, in bytes: its PCOUNT (standard §7.3.5), or the heap that a compressed table
    /// declares for the table it holds (ZPCOUNT, standard §10.3). The variable-length arrays and the compressed data
    /// of a table whose heap is larger are unavailable.
    std::uint64_t max_heap_bytes = std::uint64_t{1} << 40;
    /// Size of one tile of a compressed image or table once decompressed, in bytes, which its ZTILEn and ZBITPIX
    /// declare (standard §10.1, §10.3). A compressed tile is also refused when its stored bytes cannot decompress to
    /// that size, whatever this limit.
    std::uint64_t max_tile_bytes = std::uint64_t{4} << 30;
    /// Number of tiles of a compressed image or table.
    std::uint64_t max_tiles = std::uint64_t{1} << 26;
    /// Number of elements of one variable-length array (standard §7.3.5): every count of a P descriptor, and the
    /// compressed bytes of a tile of max_tile_bytes.
    std::uint64_t max_vla_elements = std::uint64_t{1} << 32;
    /// Size of a gzip-compressed file once inflated into memory, in bytes.
    std::uint64_t max_inflated_bytes = std::uint64_t{16} << 30;
    /// Number of diagnostics that a reader lists. When a file has more, the last one listed stands for itself and those
    /// after it: it says how many they are, and has the severity of the most serious of them, with the code of the
    /// first of those.
    std::uint64_t max_diagnostics = 10'000;
};

} // namespace openfits
