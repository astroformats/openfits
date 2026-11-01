// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>

// Character classes of the text of headers, which is ASCII (standard §4.1.1): nothing here depends on the locale.

namespace openfits::detail {

[[nodiscard]] constexpr bool is_digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

[[nodiscard]] constexpr bool is_upper(char c) noexcept
{
    return c >= 'A' && c <= 'Z';
}

[[nodiscard]] constexpr bool is_lower(char c) noexcept
{
    return c >= 'a' && c <= 'z';
}

/// The restricted set of ASCII-text characters of standard §4.1.2.3, 0x20 to 0x7E.
[[nodiscard]] constexpr bool is_printable(char c) noexcept
{
    return c >= ' ' && c <= '~';
}

[[nodiscard]] constexpr char to_upper(char c) noexcept
{
    return is_lower(c) ? static_cast<char>(c - 'a' + 'A') : c;
}

/// True when every character of text is printable.
[[nodiscard]] constexpr bool is_printable(std::string_view text) noexcept
{
    return std::ranges::all_of(text, [](char c) { return is_printable(c); });
}

/// text without leading spaces.
[[nodiscard]] constexpr std::string_view trim_leading_spaces(std::string_view text) noexcept
{
    const std::size_t first = text.find_first_not_of(' ');
    return first == std::string_view::npos ? std::string_view{} : text.substr(first);
}

/// text without trailing spaces.
[[nodiscard]] constexpr std::string_view trim_trailing_spaces(std::string_view text) noexcept
{
    const std::size_t last = text.find_last_not_of(' ');
    return last == std::string_view::npos ? std::string_view{} : text.substr(0, last + 1);
}

/// text without leading and trailing spaces.
[[nodiscard]] constexpr std::string_view trim_spaces(std::string_view text) noexcept
{
    return trim_trailing_spaces(trim_leading_spaces(text));
}

} // namespace openfits::detail
