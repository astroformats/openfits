// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <string_view>

namespace openfits::detail {

/// True when text is well-formed UTF-8 (Unicode §3.9, table 3-7) and does not contain U+0000. Overlong forms,
/// surrogates and code points above U+10FFFF are rejected.
///
/// The text of the API is UTF-8, paths included, and no path holds U+0000.
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

} // namespace openfits::detail
