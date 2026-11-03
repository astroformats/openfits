// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/checksum.h>

#include "support/fixture_builder.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Units with DATASUM and CHECKSUM for the tests, the sums computed here, integer by integer, not by the library: only
// the encoding of CHECKSUM is the library's, tested on its own against standard Appendix J.

namespace openfits::test {

/// The 32-bit ones' complement sum of standard §4.4.2.7, integer by integer, each carry out of the top bit added back
/// into the bottom one; a last integer that the bytes end within completed with zeros.
[[nodiscard]] inline std::uint32_t reference_sum(std::span<const std::byte> bytes)
{
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < bytes.size(); i += 4) {
        std::uint32_t integer = 0;
        for (std::size_t j = 0; j < 4; ++j) {
            const std::uint32_t byte = i + j < bytes.size() ? std::to_integer<std::uint32_t>(bytes[i + j]) : 0U;
            integer = (integer << 8U) | byte;
        }
        const std::uint64_t total = std::uint64_t{sum} + integer;
        sum = static_cast<std::uint32_t>(total) + static_cast<std::uint32_t>(total >> 32U);
    }
    return sum;
}

/// The ones' complement sum of two sums.
[[nodiscard]] inline std::uint32_t add_reference_sums(std::uint32_t a, std::uint32_t b)
{
    const std::uint64_t total = std::uint64_t{a} + b;
    return static_cast<std::uint32_t>(total) + static_cast<std::uint32_t>(total >> 32U);
}

/// Which checksum keywords checked_unit() writes.
enum class checksum_keywords
{
    both,
    checksum_alone,
};

/// A unit whose header holds records, then CHECKSUM and DATASUM, or CHECKSUM alone, computed for it, and END; its data
/// the bytes given, filled with fill to the end of their last block.
[[nodiscard]] inline std::vector<std::byte> checked_unit(const std::vector<std::string>& records,
                                                         std::span<const std::byte> data, char fill = '\0',
                                                         checksum_keywords keywords = checksum_keywords::both)
{
    std::vector<std::byte> unit_data(data.begin(), data.end());
    while (unit_data.size() % block_bytes != 0) {
        unit_data.push_back(static_cast<std::byte>(fill));
    }
    const std::uint32_t data_sum = reference_sum(unit_data);
    std::vector<std::string> sums{card_record("CHECKSUM", "'0000000000000000'")};
    if (keywords == checksum_keywords::both) {
        sums.push_back(card_record("DATASUM", "'" + std::to_string(data_sum) + "'"));
    }
    fixture header;
    header.header(with(records, sums));
    std::vector<std::byte> unit = header.bytes();
    const std::string value = encode_checksum(add_reference_sums(reference_sum(unit), data_sum));
    // The value of CHECKSUM starts at byte 12 of its record, the record after the records given.
    const std::size_t checksum = (records.size() * record_size) + 11;
    for (std::size_t i = 0; i < value.size(); ++i) {
        unit[checksum + i] = static_cast<std::byte>(value[i]);
    }
    unit.insert(unit.end(), unit_data.begin(), unit_data.end());
    return unit;
}

} // namespace openfits::test
