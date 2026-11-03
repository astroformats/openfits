// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include "compress/gzip.h"
#include "compress/rice.h"
#include "support/fixture_builder.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Compressed images and tables built by the tests from records and bytes (standard §10), as fixture_builder.h builds
// other units: the tiles compressed by the codecs of the library, which have tests of their own, and laid out here.

namespace openfits::test {

/// The descriptor of a P column (standard §7.3.5): count and offset, big-endian.
[[nodiscard]] inline std::vector<std::byte> p_descriptor(std::uint32_t count, std::uint32_t offset)
{
    return big_endian(std::vector<std::uint32_t>{count, offset});
}

/// A field of one big-endian value.
template <typename T> [[nodiscard]] std::vector<std::byte> field_of(T value)
{
    return big_endian(std::vector<T>{value});
}

/// The bytes of a row: its fields one after the other.
[[nodiscard]] inline std::vector<std::byte> row_of(std::initializer_list<std::vector<std::byte>> fields)
{
    std::vector<std::byte> row;
    for (const std::vector<std::byte>& field : fields) {
        row.insert(row.end(), field.begin(), field.end());
    }
    return row;
}

/// The RICE_1 stream of 16-bit values in blocks of 32.
[[nodiscard]] inline std::vector<std::byte> rice16(const std::vector<std::int16_t>& values)
{
    std::vector<std::uint16_t> bits(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        bits[i] = static_cast<std::uint16_t>(values[i]);
    }
    std::vector<std::byte> stream;
    detail::rice_encode(std::span<const std::uint16_t>(bits), 32, stream);
    return stream;
}

/// The RICE_1 stream of 32-bit values in blocks of 32.
[[nodiscard]] inline std::vector<std::byte> rice32(const std::vector<std::int32_t>& values)
{
    std::vector<std::uint32_t> bits(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        bits[i] = static_cast<std::uint32_t>(values[i]);
    }
    std::vector<std::byte> stream;
    detail::rice_encode(std::span<const std::uint32_t>(bits), 32, stream);
    return stream;
}

/// The gzip stream of bytes.
[[nodiscard]] inline std::vector<std::byte> gzip(std::span<const std::byte> bytes)
{
    std::vector<std::byte> stream;
    detail::gzip_encoder(6).compress(bytes, stream);
    return stream;
}

/// A binary table of the columns given by their TTYPEn and TFORMn, with the bytes of each row, all of one size, and
/// the heap after the rows; the records more after the columns. The keywords of a compressed image or table go in
/// more.
inline fixture& binary_table_unit(fixture& file, const std::vector<std::pair<std::string, std::string>>& columns,
                                  const std::vector<std::vector<std::byte>>& rows, const std::vector<std::byte>& heap,
                                  const std::vector<std::string>& more)
{
    const std::uint64_t row_size = rows.empty() ? 0 : rows.front().size();
    std::vector<std::string> records = extension_records("BINTABLE", 8, {row_size, rows.size()}, heap.size());
    records.push_back(card_record("TFIELDS", std::to_string(columns.size())));
    for (std::size_t c = 0; c < columns.size(); ++c) {
        const std::string n = std::to_string(c + 1);
        records.push_back(card_record("TTYPE" + n, "'" + columns[c].first + "'"));
        records.push_back(card_record("TFORM" + n, "'" + columns[c].second + "'"));
    }
    records.insert(records.end(), more.begin(), more.end());
    std::vector<std::byte> data;
    for (const std::vector<std::byte>& row : rows) {
        data.insert(data.end(), row.begin(), row.end());
    }
    data.insert(data.end(), heap.begin(), heap.end());
    file.header(records);
    return file.data(data);
}

/// The records of a compressed image of ZBITPIX bitpix and axes, in tiles of tile, compressed with algorithm: ZIMAGE
/// and the keywords of standard §10.1.1 and §10.1.2.
[[nodiscard]] inline std::vector<std::string> image_keywords(int bitpix, const std::vector<std::uint64_t>& axes,
                                                             const std::vector<std::uint64_t>& tile,
                                                             const std::string& algorithm)
{
    std::vector<std::string> records{card_record("ZIMAGE", "T"), card_record("ZBITPIX", std::to_string(bitpix)),
                                     card_record("ZNAXIS", std::to_string(axes.size()))};
    for (std::size_t a = 0; a < axes.size(); ++a) {
        records.push_back(card_record("ZNAXIS" + std::to_string(a + 1), std::to_string(axes[a])));
    }
    for (std::size_t a = 0; a < tile.size(); ++a) {
        records.push_back(card_record("ZTILE" + std::to_string(a + 1), std::to_string(tile[a])));
    }
    records.push_back(card_record("ZCMPTYPE", "'" + algorithm + "'"));
    return records;
}

} // namespace openfits::test
