// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

// The keywords that the standard defines, as canonical names: the mandatory and reserved ones, and among them the
// structural ones, which the library writes from the model of a unit.

namespace openfits::detail {

/// The index n of an indexed keyword root + n: a positive integer without leading zeros (standard §4.1.2.1), at most
/// 999, which every index of the standard is. Nothing when name is not of that form.
[[nodiscard]] std::optional<std::uint32_t> keyword_index(std::string_view name, std::string_view root) noexcept;

/// True for the keywords that the library writes from the model of a unit: SIMPLE, BITPIX, NAXIS, NAXISn, EXTEND,
/// XTENSION, PCOUNT, GCOUNT, GROUPS, TFIELDS, TTYPEn, TFORMn, TBCOLn, TUNITn, THEAP, BSCALE, BZERO, BLANK, TSCALn,
/// TZEROn, TNULLn, TDISPn, TDIMn, the keywords of tiled compression (standard §10), CHECKSUM, DATASUM, CONTINUE and
/// END.
[[nodiscard]] bool is_structural_keyword(std::string_view name) noexcept;

/// True for the mandatory and reserved keywords of the standard, the structural ones included: those of headers (§4.4),
/// random groups (§6), tables (§7), world coordinates in their three forms of Table 22 with their alternates (§8),
/// time (§9) and compression (§10). A long string must not continue one of them (standard §4.2.1.2).
[[nodiscard]] bool is_reserved_keyword(std::string_view name);

/// The type of value that a reserved keyword takes.
enum class value_class : std::uint8_t
{
    logical,
    integer,
    number, ///< An integer or a real.
    string,
    date, ///< A string that holds a date (standard §9.1.1).
};

/// The type of value of a reserved keyword of standard §4.4.2 that is not structural, of a keyword of world coordinates
/// or time in its three forms of Table 22 with its alternates (§8, §9), or of PTYPEn, PSCALn, PZEROn, TDMINn, TDMAXn,
/// TLMINn and TLMAXn (§6.1.2, §7.2.2, §7.3.2); nothing for any other keyword, the structural ones and those of
/// compression included.
[[nodiscard]] std::optional<value_class> reserved_value_class(std::string_view name);

} // namespace openfits::detail
