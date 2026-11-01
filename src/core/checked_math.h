// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/error.h>

#include <concepts>
#include <limits>
#include <utility>

// Size and offset computations on values read from a file, and in the layout that the writer computes. A result that
// does not fit means that the file describes something that cannot exist, so an overflow is reported as invalid data,
// by the writer too: a model whose layout overflows 64 bits cannot be written.

namespace openfits::detail {

[[noreturn]] void throw_arithmetic_overflow();

/// Integer types that integer promotion leaves unchanged, so that arithmetic on them stays in the type: the types of
/// sizes, offsets and keyword values.
template <typename T>
concept wide_integer = std::integral<T> && !std::same_as<T, bool> && sizeof(T) >= sizeof(int);

/// a + b, or invalid_data_error when the sum does not fit in T.
template <wide_integer T> [[nodiscard]] constexpr T checked_add(T a, T b)
{
    if constexpr (std::unsigned_integral<T>) {
        if (b > std::numeric_limits<T>::max() - a) {
            throw_arithmetic_overflow();
        }
    } else {
        if (b > 0 ? a > std::numeric_limits<T>::max() - b : a < std::numeric_limits<T>::min() - b) {
            throw_arithmetic_overflow();
        }
    }
    return a + b;
}

/// a - b, or invalid_data_error when the difference does not fit in T.
template <wide_integer T> [[nodiscard]] constexpr T checked_subtract(T a, T b)
{
    if constexpr (std::unsigned_integral<T>) {
        if (b > a) {
            throw_arithmetic_overflow();
        }
    } else {
        if (b > 0 ? a < std::numeric_limits<T>::min() + b : a > std::numeric_limits<T>::max() + b) {
            throw_arithmetic_overflow();
        }
    }
    return a - b;
}

/// a × b, or invalid_data_error when the product does not fit in T.
template <wide_integer T> [[nodiscard]] constexpr T checked_multiply(T a, T b)
{
    if constexpr (std::unsigned_integral<T>) {
        if (a != 0 && b > std::numeric_limits<T>::max() / a) {
            throw_arithmetic_overflow();
        }
    } else {
        // The four sign cases, each compared with the bound of its own sign, so that no division overflows: the
        // quotient of the minimum by -1 does not fit.
        constexpr T max = std::numeric_limits<T>::max();
        constexpr T min = std::numeric_limits<T>::min();
        bool overflow = false;
        if (a > 0) {
            overflow = b > 0 ? a > max / b : b < min / a;
        } else if (a < 0) {
            overflow = b > 0 ? a < min / b : (b != 0 && a < max / b);
        }
        if (overflow) {
            throw_arithmetic_overflow();
        }
    }
    return a * b;
}

/// value converted to To, or invalid_data_error when To cannot represent it.
template <std::integral To, std::integral From> [[nodiscard]] constexpr To checked_cast(From value)
{
    if constexpr (std::same_as<To, From>) {
        return value;
    } else {
        if (!std::in_range<To>(value)) {
            throw_arithmetic_overflow();
        }
        return static_cast<To>(value);
    }
}

} // namespace openfits::detail
