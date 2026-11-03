// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/card.h>
#include <openfits/error.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Keyword records (standard §4.1, Appendix A): one record of 80 bytes read into a card, and a card written as one
// record. Continued strings over several records (standard §4.2.1.2) are a matter of headers.

namespace openfits::detail {

/// The length of a keyword record (standard §4.1.1).
inline constexpr std::size_t record_length = 80;

/// Access to the members of a card that the reader sets and the writer reads.
struct card_access
{
    /// A card read from a record.
    [[nodiscard]] static card make(std::string keyword, card_value value, std::string comment, std::string text,
                                   std::size_t value_begin, std::size_t value_end, bool value_indicator,
                                   bool fixed_format);

    /// The value of a card read from a record is in the fixed format of its kind (standard §4.2), which the values of
    /// mandatory keywords must be.
    [[nodiscard]] static bool fixed_format(const card& value) noexcept
    {
        return value.fixed_format_;
    }
};

/// Reads a keyword record of exactly record_length bytes, as card::parse() describes. Each departure from the standard
/// is appended to departures, with the keyword in its context.
[[nodiscard]] card parse_record(std::string_view record, std::vector<diagnostic>& departures);

/// How write_value_record() and write_commentary_record() write what does not fit the standard's own forms.
struct record_options
{
    /// A keyword name that is not of standard §4.1.2.1 is written with the HIERARCH convention, rather than refused.
    bool hierarch = false;
    /// A comment that does not fit in the record is cut, rather than refused.
    bool truncate_comment = false;
    /// The long string of a reserved keyword that is not structural is continued over CONTINUE records, rather than
    /// refused (standard §4.2.1.2).
    bool continue_reserved = false;
    /// The unit of the card, which the context of the errors names.
    std::optional<std::uint64_t> hdu{};
};

/// The keyword of these functions is the name a card keeps, which they put in upper case with single spaces only: a
/// prefix HIERARCH that it still has is part of the name.
///
/// The record of a keyword with a value (standard §4.1, §4.2): the name in bytes 1 to 8 and the value indicator in
/// bytes 9 and 10; a string from byte 11, and a logical, a number or a complex value right-justified in bytes 11 to
/// 30, the fixed format, when it fits there, and from byte 11 otherwise; the comment after " / ", from byte 34 when the
/// value leaves room for it. A name of the HIERARCH convention is followed by " = " and the value.
/// @throws validation_error with the keyword in its context: errc::invalid_keyword for a name that cannot have a
/// value, or that needs the HIERARCH convention when options do not allow it; errc::value_too_long for a value that
/// does not fit; errc::comment_too_long for a comment that does not fit when options do not cut it; and the errors of
/// format_value() and, for a comment outside printable ASCII, errc::invalid_character.
[[nodiscard]] std::string write_value_record(std::string_view keyword, const card_value& value,
                                             std::string_view comment, const record_options& options);

/// The record of a commentary card: the name in bytes 1 to 8, the text in bytes 9 to 80.
/// @throws validation_error with the keyword in its context: errc::invalid_keyword for a name that is not of standard
/// §4.1.2.1, or that is CONTINUE or END, which the writer generates; errc::invalid_value_indicator for a text that
/// would read as a value indicator after any keyword but COMMENT, HISTORY and the blank one; errc::comment_too_long and
/// errc::invalid_character as write_value_record().
[[nodiscard]] std::string write_commentary_record(std::string_view keyword, std::string_view text,
                                                  const record_options& options);

/// The record of a card: the record it was read from, or the record that write_value_record() or
/// write_commentary_record() makes of it.
[[nodiscard]] std::string write_record(const card& value, const record_options& options);

} // namespace openfits::detail
