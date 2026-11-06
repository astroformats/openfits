// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/card.h>
#include <openfits/error.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The text forms of keyword values (standard §4.2, Appendix A): reading them from a value field, and writing them.
// Departures from the standard that leave the meaning clear are read as intended and reported, each as a diagnostic
// without context, which the caller completes.

namespace openfits::detail {

/// The form of a numeric token, a run of characters without spaces.
enum class number_form : std::uint8_t
{
    not_a_number,
    integer, ///< [sign] digits (standard §4.2.3).
    real,    ///< A decimal number with a decimal point, an exponent, or both (standard §4.2.4).
};

/// What scan_number() finds in a token.
struct scanned_number
{
    number_form form = number_form::not_a_number;
    /// The exponent letter is e or d, which the standard does not allow (standard §4.2.4): the value is read all the
    /// same.
    bool lowercase_exponent = false;
};

/// The form of token in the grammar of standard §4.2.3 and §4.2.4, Appendix A.
[[nodiscard]] scanned_number scan_number(std::string_view token) noexcept;

/// The value of a token of number_form::integer: a std::int64_t, a std::uint64_t from 2^63 to 2^64 - 1, or nothing
/// beyond -2^63 to 2^64 - 1. Leading zeros are not significant.
[[nodiscard]] std::optional<card_value> integer_value(std::string_view token) noexcept;

/// The value of a token of number_form::integer or number_form::real, rounded to the nearest double, or nothing when
/// its magnitude is beyond the range of double. A magnitude below the smallest subnormal becomes a zero of the same
/// sign.
[[nodiscard]] std::optional<double> real_value(std::string_view token);

/// The value of a real of the form of std::from_chars, without a plus sign before it and with the exponent letter E,
/// rounded to the nearest double, without allocating. Nothing when number holds another text or a value that
/// std::from_chars reports out of range, and in standard libraries whose std::from_chars of double is missing or
/// reports subnormals out of range: real_value() reads such numbers.
[[nodiscard]] std::optional<double> plain_real_value(std::span<const char> number) noexcept;

/// What read_value_field() finds in a value field.
struct value_field
{
    /// The value, none when the field holds none of the forms of the standard.
    card_value value{};
    /// The value as written, field.substr(value_begin, value_end - value_begin); empty for an undefined value.
    std::size_t value_begin = 0;
    std::size_t value_end = 0;
    /// The value is in the fixed format of its kind (standard §4.2): a string from byte 11, a logical in byte 30, a
    /// number right-justified in bytes 11 to 30.
    bool fixed_format = false;
    /// The comment, without its slash, the space after it, and trailing spaces.
    std::string comment{};
};

/// Reads a value field: what follows the value indicator, bytes 11 to 80 of a record when at_byte_11, so that the fixed
/// format can be told. Each departure from the standard is appended to departures.
[[nodiscard]] value_field read_value_field(std::string_view field, bool at_byte_11,
                                           std::vector<diagnostic>& departures);

/// Where read_value_field_into() finds the value of a value field, as value_field has it.
struct value_extent
{
    std::size_t value_begin = 0;
    std::size_t value_end = 0;
    bool fixed_format = false;
};

/// read_value_field() with the value and the comment written into value and comment, which it replaces: a reader
/// makes the cards of a header in their place, without values and comments made apart and moved into them.
value_extent read_value_field_into(std::string_view field, bool at_byte_11, std::vector<diagnostic>& departures,
                                   card_value& value, std::string& comment);

/// The text of an integer (standard §4.2.3).
[[nodiscard]] std::string format_integer(std::int64_t value);
[[nodiscard]] std::string format_integer(std::uint64_t value);

/// The text of a real (standard §4.2.4): the shortest that reads back as value, with an upper-case exponent letter, and
/// a decimal point when it has no exponent, so that it reads as a real.
/// @throws validation_error with errc::invalid_real for a NaN or an infinity.
[[nodiscard]] std::string format_real(double value);

/// The text of a string (standard §4.2.1.1): in quotes, each quote doubled, and padded with spaces to eight characters
/// unless it is empty, the null string, as readers that expect the fixed format of old want it.
/// @throws validation_error with errc::invalid_character for a character outside printable ASCII, unless any_bytes.
[[nodiscard]] std::string format_string(std::string_view value, bool any_bytes = false);

/// The text of any value, as a value field holds it: format_integer(), format_real() and format_string() for those
/// kinds, T or F, (real, imaginary) for complex values, and nothing for an undefined value.
/// @throws validation_error as the functions above, and with errc::invalid_value for a value of value_kind::none.
[[nodiscard]] std::string format_value(const card_value& value, bool any_bytes = false);

/// The value of text alone, the value text of a card without a record: what read_value_field() reads when the text is
/// one value and nothing after it, departures from the standard included; none otherwise.
[[nodiscard]] card_value value_of_text(std::string_view text);

/// Checks that text is one value of the forms of standard §4.2 and nothing else, as a writer writes a value text: a
/// departure that a reader tolerates, such as a lowercase exponent letter, is refused, and a byte outside printable
/// ASCII only when not any_bytes.
/// @throws validation_error with the code of the first departure, errc::invalid_value for a text of no form or with
/// more than one value.
void check_value_text(std::string_view text, bool any_bytes);

} // namespace openfits::detail
