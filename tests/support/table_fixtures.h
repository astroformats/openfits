// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include "support/fixture_builder.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Tables built by the tests themselves, without the library's writer: the mandatory records of ASCII and binary tables
// and the bytes of their rows and heaps.

namespace openfits::test {

/// The mandatory records of a binary table (standard §7.3.1, Table 17): XTENSION, BITPIX = 8, NAXIS = 2, NAXIS1,
/// NAXIS2, PCOUNT, GCOUNT = 1 and TFIELDS, then TFORMn for each format.
[[nodiscard]] inline std::vector<std::string> bintable_records(std::uint64_t row_size, std::uint64_t rows,
                                                               const std::vector<std::string>& formats,
                                                               std::uint64_t pcount = 0)
{
    std::vector<std::string> records = extension_records("BINTABLE", 8, {row_size, rows}, pcount, 1);
    records.push_back(card_record("TFIELDS", std::to_string(formats.size())));
    for (std::size_t i = 0; i < formats.size(); ++i) {
        records.push_back(card_record("TFORM" + std::to_string(i + 1), "'" + formats[i] + "'"));
    }
    return records;
}

/// A field of an ASCII table: TBCOLn and TFORMn.
struct ascii_field
{
    std::uint64_t column = 1;
    std::string format{};
};

/// The mandatory records of an ASCII table (standard §7.2.1, Table 14): XTENSION, BITPIX = 8, NAXIS = 2, NAXIS1,
/// NAXIS2, PCOUNT = 0, GCOUNT = 1 and TFIELDS, then TBCOLn and TFORMn for each field.
[[nodiscard]] inline std::vector<std::string> ascii_table_records(std::uint64_t row_size, std::uint64_t rows,
                                                                  const std::vector<ascii_field>& fields)
{
    std::vector<std::string> records = extension_records("TABLE", 8, {row_size, rows}, 0, 1);
    records.push_back(card_record("TFIELDS", std::to_string(fields.size())));
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const std::string n = std::to_string(i + 1);
        records.push_back(card_record("TBCOL" + n, std::to_string(fields[i].column)));
        records.push_back(card_record("TFORM" + n, "'" + fields[i].format + "'"));
    }
    return records;
}

/// Bytes appended in the order of a row or a heap, values big-endian as a file stores them.
class byte_writer
{
public:
    template <typename T> byte_writer& value(T number)
    {
        // Appended byte by byte: GCC 13 at -O3 on s390x takes a copy into the end of a vector just grown by resize()
        // for an overflow.
        auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(number);
        if constexpr (std::endian::native == std::endian::little) {
            std::ranges::reverse(bytes);
        }
        for (const std::byte byte : bytes) {
            bytes_.push_back(byte);
        }
        return *this;
    }

    template <typename T> byte_writer& values(const std::vector<T>& numbers)
    {
        for (const T number : numbers) {
            value(number);
        }
        return *this;
    }

    /// Text padded with fill to width characters, or cut to them.
    byte_writer& text(std::string_view characters, std::size_t width, char fill = ' ')
    {
        for (std::size_t i = 0; i < width; ++i) {
            bytes_.push_back(static_cast<std::byte>(i < characters.size() ? characters[i] : fill));
        }
        return *this;
    }

    byte_writer& bytes(const std::vector<std::uint8_t>& raw)
    {
        for (const std::uint8_t byte : raw) {
            bytes_.push_back(static_cast<std::byte>(byte));
        }
        return *this;
    }

    [[nodiscard]] const std::vector<std::byte>& data() const noexcept
    {
        return bytes_;
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return bytes_.size();
    }

private:
    std::vector<std::byte> bytes_{};
};

} // namespace openfits::test
