// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/export.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

/// @file
/// The checksums of header and data units (standard §4.4.2.7, Appendix J): the encoding of CHECKSUM, and what the
/// verification of a unit finds.

namespace openfits {

/// The value of the CHECKSUM keyword of a unit whose sum is sum while CHECKSUM holds '0000000000000000': the 16
/// characters of standard Appendix J.2 that encode the complement of sum, and make the sum of the unit negative zero,
/// all 32 bits set, once they replace the zeros. The sum is the 32-bit ones' complement sum of the bytes of the unit
/// taken as big-endian integers. The characters are digits, capital letters and the small letters a to r, never the
/// punctuation between them; the string starts at byte 12 of its record, in the fixed format that the encoding needs.
[[nodiscard]] OPENFITS_API std::string encode_checksum(std::uint32_t sum);

/// The sum that encode_checksum() encodes as text, by the inversion of standard Appendix J.2, for any text of 16
/// characters: its sum is taken modulo 2^32 - 1 when it is not an encoding. Nothing for text of another length.
[[nodiscard]] OPENFITS_API std::optional<std::uint32_t> decode_checksum(std::string_view text) noexcept;

/// What a checksum keyword says of its unit (standard §4.4.2.7).
enum class checksum_state : std::uint8_t
{
    absent,  ///< The keyword is missing, or its value is blank, null or undefined, which asserts nothing of the sum.
    valid,   ///< The unit matches it.
    invalid, ///< The unit does not match it: the unit changed after the keyword was computed, or the keyword holds no
             ///< sum.
};

/// The checksums of a unit, verified against its DATASUM and CHECKSUM (standard §4.4.2.7).
struct checksum_report
{
    /// DATASUM against the sum of the data records of the unit.
    checksum_state datasum = checksum_state::absent;
    /// CHECKSUM: whether the sum of the whole unit, header and data, is negative zero.
    checksum_state checksum = checksum_state::absent;
    /// The 32-bit ones' complement sum of the data records, the fill of their last block included, as DATASUM records
    /// it: 0 for a unit without data.
    std::uint32_t data_sum = 0;
    /// The sum of the whole unit: 0xFFFFFFFF, negative zero, when CHECKSUM is valid.
    std::uint32_t hdu_sum = 0;

    friend bool operator==(const checksum_report&, const checksum_report&) = default;
};

} // namespace openfits
