// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <string>
#include <string_view>

// Keyword names (standard §4.1.2.1), the names of the HIERARCH convention, and the patterns of keyword searches.

namespace openfits::detail {

/// The characters of keyword names: A to Z, 0 to 9, the hyphen and the underscore (standard §4.1.2.1).
[[nodiscard]] constexpr bool is_keyword_char(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

/// True when name is a keyword name of the standard: one to eight characters of is_keyword_char().
[[nodiscard]] bool is_standard_keyword(std::string_view name) noexcept;

/// The canonical form of a keyword name, which lookups use: without leading and trailing spaces, without the prefix
/// HIERARCH of the convention and the spaces after it, with every run of spaces made one space, in upper case. The
/// name HIERARCH alone stays.
[[nodiscard]] std::string canonical_keyword(std::string_view name);

/// The canonical form of the name that a record of the HIERARCH convention holds after its prefix: as
/// canonical_keyword() makes it, but for the prefix, which the record already had.
[[nodiscard]] std::string canonical_hierarch_name(std::string_view name);

/// True for the canonical names of the commentary keywords of standard §4.4.2.4: COMMENT, HISTORY and the blank
/// keyword, which have no value whatever bytes 9 and 10 hold.
[[nodiscard]] bool is_commentary_keyword(std::string_view name) noexcept;

/// True when pattern holds one of the wildcards of match_keyword().
[[nodiscard]] bool has_wildcards(std::string_view pattern) noexcept;

/// True when the keyword name matches pattern, in which ? matches one character, * any run of characters, the empty
/// one included, and # a run of one or more digits, all the digits that follow. Letters match in either case, or only
/// in their own with exact_case, as the names of table columns may be compared. A pattern that equals the name matches
/// it, wildcards included.
[[nodiscard]] bool match_keyword(std::string_view pattern, std::string_view name, bool exact_case = false) noexcept;

} // namespace openfits::detail
