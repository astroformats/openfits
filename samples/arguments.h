// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

// The arguments of a sample program in UTF-8, which the paths of the library are in on every platform.

#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
// shellapi.h needs the declarations of windows.h before it, so the two keep this order.
// clang-format off
#include <windows.h>
#include <shellapi.h>
// clang-format on
#endif

namespace sample {

/// The arguments after the name of the program, in UTF-8. On Windows, main() receives them in the ANSI code page, which
/// cannot represent every character of a path, so they are taken in UTF-16 from CommandLineToArgvW() and converted.
inline std::vector<std::string> arguments([[maybe_unused]] int argc, [[maybe_unused]] char** argv)
{
#if defined(_WIN32)
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == nullptr) {
        throw std::runtime_error("cannot read the command line");
    }
    std::vector<std::string> result;
    for (int i = 1; i < count; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string text(size > 0 ? static_cast<std::size_t>(size) : 0, '\0');
        if (size <= 0 ||
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, text.data(), size, nullptr, nullptr) <= 0) {
            LocalFree(static_cast<HLOCAL>(wide));
            throw std::runtime_error("an argument is not valid UTF-16");
        }
        text.pop_back(); // The terminating null character.
        result.push_back(std::move(text));
    }
    LocalFree(static_cast<HLOCAL>(wide));
    return result;
#else
    return {argv + 1, argv + argc};
#endif
}

} // namespace sample
