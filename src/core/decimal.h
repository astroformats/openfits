// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// The exact decimal value of a double, and its rounding half away from zero, for display formats (standard §7.3.4),
// which round "by the normal rules of arithmetic" rather than to even.

namespace openfits::detail {

/// The magnitude of a finite double as decimal digits: 0.d1d2d3... × 10^exponent.
struct decimal_digits
{
    /// The significant digits, without leading zeros; empty for zero.
    std::string digits{};
    /// The decimal exponent, such that the value is 0.digits × 10^exponent; 0 for zero.
    std::int64_t exponent = 0;
};

/// The exact decimal digits of the magnitude of a finite value, without trailing zeros.
[[nodiscard]] decimal_digits exact_digits(double value);

/// value rounded half away from zero to count significant digits, at least one: exactly count digits, padded with
/// zeros, and a carry into a new leading digit raises the exponent. Zero gives count zeros.
[[nodiscard]] decimal_digits round_significant(const decimal_digits& value, std::size_t count);

/// The integer and fractional digits of a value rounded to a number of places after the decimal point.
struct fixed_digits
{
    /// The digits before the point, empty when the integer part is zero.
    std::string integer{};
    /// Exactly the number of places of digits after the point.
    std::string fraction{};
};

/// value rounded half away from zero to the digits down to 10^-places.
[[nodiscard]] fixed_digits round_fixed(const decimal_digits& value, std::size_t places);

} // namespace openfits::detail
