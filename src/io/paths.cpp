// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "io/paths.h"

#include <openfits/error.h>

#include "core/quote.h"
#include "core/utf8.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <new>
#include <random>
#include <string>
#include <system_error>

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

std::uint64_t random_seed()
{
    // std::random_device reports a failure with an exception type of its own choice, which depends on the standard
    // library. Every one of them becomes an io_error, except std::bad_alloc.
    try {
        std::random_device device;
        // Each call produces 32 bits.
        const std::uint64_t high = device();
        return (high << 32U) | std::uint64_t{device()};
    } catch (const std::system_error& failure) {
        throw io_error(errc::entropy_unavailable, "no random data for a temporary name: " + std::string(failure.what()),
                       failure.code());
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& failure) {
        throw io_error(errc::entropy_unavailable,
                       "no random data for a temporary name: " + std::string(failure.what()));
    }
}

std::uint64_t next_random(std::uint64_t& state) noexcept
{
    state += 0x9E37'79B9'7F4A'7C15U;
    std::uint64_t value = state;
    value = (value ^ (value >> 30U)) * 0xBF58'476D'1CE4'E5B9U;
    value = (value ^ (value >> 27U)) * 0x94D0'49BB'1331'11EBU;
    return value ^ (value >> 31U);
}

// A fixed length keeps the name within the limits of the file system whatever the name of the target.
std::string temporary_path(std::string_view target, std::uint64_t random)
{
#if defined(_WIN32)
    const std::size_t separator = target.find_last_of("/\\");
#else
    const std::size_t separator = target.find_last_of('/');
#endif
    std::string path(target.substr(0, separator + 1));
    path += ".openfits-";
    constexpr std::string_view digits = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4) {
        path += digits[(random >> static_cast<unsigned>(shift)) & 0xFU];
    }
    path += ".tmp";
    return path;
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
