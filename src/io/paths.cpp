// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "io/paths.h"

#include <openfits/error.h>

#include "core/quote.h"
#include "core/utf8.h"

#include <algorithm>
#include <cstddef>
#include <string>

namespace openfits::detail {

void check_path_argument(std::string_view path, std::string_view what)
{
    if (path.empty()) {
        throw usage_error(errc::invalid_argument, std::string(what) + " is empty");
    }
    if (!is_valid_utf8(path)) {
        throw usage_error(errc::invalid_utf8, std::string(what) + ", " + quote(path) + ", is not valid UTF-8");
    }
}

#if defined(_WIN32)
namespace {

bool is_separator(char c) noexcept
{
    return c == '/' || c == '\\';
}

bool is_ascii_letter(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// True when text starts with prefix, an ASCII word, in any case, as the names of the object manager compare.
bool starts_with_word(std::string_view text, std::string_view prefix) noexcept
{
    return text.size() >= prefix.size() &&
           std::ranges::equal(text.substr(0, prefix.size()), prefix, [](char a, char b) {
               return (a >= 'A' && a <= 'Z' ? a - 'A' + 'a' : a) == (b >= 'A' && b <= 'Z' ? b - 'A' + 'a' : b);
           });
}

} // namespace

bool names_a_device(std::string_view path) noexcept
{
    if (path.size() < 4 || !is_separator(path[0]) || !is_separator(path[1]) || (path[2] != '.' && path[2] != '?') ||
        !is_separator(path[3])) {
        return false;
    }
    const std::string_view rest = path.substr(4);
    if (rest.size() >= 3 && is_ascii_letter(rest[0]) && rest[1] == ':' && is_separator(rest[2])) {
        return false;
    }
    if (starts_with_word(rest, "UNC") && rest.size() > 3 && is_separator(rest[3])) {
        return false;
    }
    if (starts_with_word(rest, "Volume{")) {
        const std::size_t end = rest.find('}');
        return end == std::string_view::npos || end + 1 >= rest.size() || !is_separator(rest[end + 1]);
    }
    return true;
}
#endif

} // namespace openfits::detail
