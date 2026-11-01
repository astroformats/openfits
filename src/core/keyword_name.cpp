// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "core/keyword_name.h"

#include "core/ascii.h"

#include <algorithm>
#include <cstddef>

namespace openfits::detail {

namespace {

constexpr std::string_view hierarch_prefix = "HIERARCH";

bool equal_ignoring_case(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size() && std::ranges::equal(a, b, [](char x, char y) { return to_upper(x) == to_upper(y); });
}

} // namespace

bool is_standard_keyword(std::string_view name) noexcept
{
    return !name.empty() && name.size() <= 8 && std::ranges::all_of(name, is_keyword_char);
}

std::string canonical_keyword(std::string_view name)
{
    name = trim_spaces(name);
    if (name.size() > hierarch_prefix.size() && name[hierarch_prefix.size()] == ' ' &&
        equal_ignoring_case(name.substr(0, hierarch_prefix.size()), hierarch_prefix)) {
        name = name.substr(hierarch_prefix.size());
    }
    return canonical_hierarch_name(name);
}

std::string canonical_hierarch_name(std::string_view name)
{
    name = trim_spaces(name);
    std::string canonical;
    canonical.reserve(name.size());
    for (const char c : name) {
        if (c != ' ' || canonical.empty() || canonical.back() != ' ') {
            canonical += to_upper(c);
        }
    }
    return canonical;
}

bool is_commentary_keyword(std::string_view name) noexcept
{
    return name.empty() || name == "COMMENT" || name == "HISTORY";
}

bool has_wildcards(std::string_view pattern) noexcept
{
    return pattern.find_first_of("*?#") != std::string_view::npos;
}

bool match_keyword(std::string_view pattern, std::string_view name) noexcept
{
    if (equal_ignoring_case(pattern, name)) {
        return true;
    }
    // Wildcard matching that keeps the position of the last * to come back to, which bounds the work by the product of
    // the lengths: names and patterns are short.
    std::size_t p = 0;
    std::size_t n = 0;
    std::size_t star = std::string_view::npos;
    std::size_t star_name = 0;
    while (n < name.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            star_name = n;
        } else if (p < pattern.size() && pattern[p] == '#' && is_digit(name[n])) {
            ++p;
            while (n < name.size() && is_digit(name[n])) {
                ++n;
            }
        } else if (p < pattern.size() && (pattern[p] == '?' || to_upper(pattern[p]) == to_upper(name[n]))) {
            ++p;
            ++n;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            n = ++star_name;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

} // namespace openfits::detail
