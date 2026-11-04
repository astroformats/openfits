// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// The paths that the application gives, in UTF-8, which the system takes in its own syntax, and the names of the
// temporary files of the library.

namespace openfits::detail {

/// Throws usage_error unless path, a path that the application gives, is not empty (errc::invalid_argument) and is
/// valid UTF-8 (errc::invalid_utf8). what names it in the messages, as in "the path".
void check_path_argument(std::string_view path, std::string_view what);

/// 64 bits of the random source of the system, from which the names of temporary files follow. Throws io_error with
/// errc::entropy_unavailable when the system gives none.
[[nodiscard]] std::uint64_t random_seed();

/// The next value of the SplitMix64 sequence of state, which gives each attempt at a temporary file a name of its own.
[[nodiscard]] std::uint64_t next_random(std::uint64_t& state) noexcept;

/// A name in the directory of target, which is absolute and so has a separator, made of random:
/// .openfits-<16 hexadecimal digits>.tmp.
[[nodiscard]] std::string temporary_path(std::string_view target, std::uint64_t random);

#if defined(_WIN32)
/// True when path, absolute and normalized as Windows normalizes a path (GetFullPathNameW()), names a device: a path of
/// the device namespace, \\.\ or \\?\, that does not go on with a file system, a drive and a separator (C:\), UNC\ or a
/// volume (Volume{...}\). \\.\NUL, which a device name such as NUL becomes in any directory, \\.\COM1, \\.\pipe\name,
/// \\.\C:, the volume itself, and \\?\GLOBALROOT\Device\Null are devices.
[[nodiscard]] bool names_a_device(std::string_view path) noexcept;
#endif

} // namespace openfits::detail
