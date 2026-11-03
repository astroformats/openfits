// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/card.h>
#include <openfits/error.h>

#include <cstdint>
#include <optional>
#include <string_view>

// The numbers of the fields of ASCII tables, read as Fortran reads its input formats (standard §7.2.5).

namespace openfits::detail {

/// A number read from a field: an integer, held as card_value holds integers, or a real; or the problem that leaves
/// the field without one.
struct field_number
{
    card_value value{};
    /// errc::invalid_field for text of no form of the field, errc::value_out_of_range for a number beyond 64-bit
    /// integers or double; nothing for a number.
    std::optional<errc> problem{};
};

/// The integer of an Iw field: spaces before and after it, an optional sign and digits. Spaces inside it are ignored,
/// as Fortran ignores them in its input; a blank field is 0.
[[nodiscard]] field_number read_integer_field(std::string_view field);

/// The real of an Fw.d, Ew.d or Dw.d field: an optional sign, digits with an optional decimal point, and an optional
/// exponent, E or D and an optionally signed integer, or a sign and an integer; spaces before, after and inside it
/// ignored, as Fortran ignores them, and a blank field 0. Without a decimal point, the rightmost decimals digits are
/// the fraction. The value is the decimal number rounded to the nearest double.
[[nodiscard]] field_number read_real_field(std::string_view field, std::uint32_t decimals);

/// read_integer_field() of the fields that most tables hold, without allocating: a blank field, or an optional sign
/// and up to 18 digits between spaces. Nothing for any other field, which read_integer_field() reads.
[[nodiscard]] std::optional<std::int64_t> read_plain_integer_field(std::string_view field) noexcept;

/// read_real_field() of the fields that most tables hold, without allocating: a blank field, or between spaces an
/// optional sign, digits with a decimal point, or without one when decimals is 0, and an optional exponent of a letter
/// and an optionally signed integer, of at most 64 characters, whose value is within the range of double. Nothing for
/// any other field, which read_real_field() reads.
[[nodiscard]] std::optional<double> read_plain_real_field(std::string_view field, std::uint32_t decimals) noexcept;

} // namespace openfits::detail
