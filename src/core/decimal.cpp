// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "core/decimal.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <string_view>

namespace openfits::detail {

namespace {

// Every double has at most 767 significant decimal digits, so that 767 digits after the first are its exact value.
constexpr int exact_precision = 767;

// The digits after the first that hold the exact value of a finite nonzero magnitude, and one more: magnitude is
// M x 2^E with M odd, an integer of as many digits as its logarithm says for E >= 0, and M x 5^-E x 10^E otherwise,
// whose digits are those of M x 5^-E. The estimate from logarithms may be one short, which the digit more makes up; a
// digit too many is a trailing zero.
int exact_precision_of(double magnitude) noexcept
{
    const auto bits = std::bit_cast<std::uint64_t>(magnitude);
    const auto biased = static_cast<int>(bits >> 52U);
    std::uint64_t mantissa = bits & ((std::uint64_t{1} << 52U) - 1);
    int exponent = -1074;
    if (biased != 0) {
        mantissa |= std::uint64_t{1} << 52U;
        exponent = biased - 1075;
    }
    const int zeros = std::countr_zero(mantissa);
    mantissa >>= static_cast<unsigned>(zeros);
    exponent += zeros;
    const double logarithm =
        exponent >= 0 ? std::log10(magnitude)
                      : std::log10(static_cast<double>(mantissa)) + (static_cast<double>(-exponent) * std::log10(5.0));
    return std::min(exact_precision, static_cast<int>(logarithm) + 1);
}

// The digits of the magnitude of a finite nonzero value at precision digits after the first, correctly rounded, with
// their trailing zeros.
decimal_digits scientific_digits(double magnitude, int precision)
{
    decimal_digits result;
    // d.ddd...e±x: the first digit, the decimal point, the other digits, and the exponent.
    std::array<char, 800> buffer{};
    const char* const end =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), magnitude, std::chars_format::scientific, precision)
            .ptr;
    const std::string_view text(buffer.data(), static_cast<std::size_t>(end - buffer.data()));
    const std::size_t letter = text.find('e');
    result.digits.reserve(letter);
    result.digits += text.front();
    if (letter > 2) {
        result.digits.append(text.substr(2, letter - 2));
    }
    std::string_view exponent = text.substr(letter + 1);
    if (exponent.front() == '+') {
        exponent.remove_prefix(1);
    }
    int power = 0;
    (void)std::from_chars(exponent.data(), exponent.data() + exponent.size(), power);
    result.exponent = power + 1;
    return result;
}

// The digits that round to at most most digits as the exact ones do: see rounding_digits().
decimal_digits deciding_digits(double magnitude, decimal_digits leading, std::size_t most)
{
    const std::size_t last = leading.digits.find_last_not_of('0');
    if (leading.digits[last] == '5' && last <= most) {
        decimal_digits exact = scientific_digits(magnitude, exact_precision_of(magnitude));
        exact.digits.erase(exact.digits.find_last_not_of('0') + 1);
        return exact;
    }
    leading.digits.erase(last + 1);
    return leading;
}

// The integer digits of 0.digits × 10^exponent, and its digits after the point down to the last place, with zeros past
// its last digit.
fixed_digits layout(const std::string& digits, std::int64_t exponent, std::size_t places)
{
    const auto digit_at = [&digits](std::int64_t index) {
        return index >= 0 && static_cast<std::size_t>(index) < digits.size() ? digits[static_cast<std::size_t>(index)]
                                                                             : '0';
    };
    fixed_digits result;
    for (std::int64_t i = 0; i < exponent; ++i) {
        result.integer += digit_at(i);
    }
    result.fraction.reserve(places);
    for (std::size_t j = 0; j < places; ++j) {
        result.fraction += digit_at(exponent + static_cast<std::int64_t>(j));
    }
    return result;
}

} // namespace

decimal_digits exact_digits(double value)
{
    const double magnitude = std::fabs(value);
    if (magnitude == 0) {
        return {};
    }
    decimal_digits result = scientific_digits(magnitude, exact_precision_of(magnitude));
    result.digits.erase(result.digits.find_last_not_of('0') + 1);
    return result;
}

decimal_digits rounding_digits(double value, std::size_t most)
{
    const double magnitude = std::fabs(value);
    if (magnitude == 0) {
        return {};
    }
    if (most + 3 > static_cast<std::size_t>(exact_precision)) {
        return exact_digits(value);
    }
    return deciding_digits(magnitude, scientific_digits(magnitude, static_cast<int>(most) + 2), most);
}

decimal_digits fixed_rounding_digits(double value, std::size_t places)
{
    const double magnitude = std::fabs(value);
    if (magnitude == 0) {
        return {};
    }
    // The exponent, from one correctly rounded digit: a carry into a new digit makes it one more than that of the
    // exact digits, which round_fixed() rounds alike, its kept digits one more as well.
    const std::int64_t exponent = scientific_digits(magnitude, 0).exponent;
    const std::int64_t kept = exponent + static_cast<std::int64_t>(places) + 1;
    if (kept + 3 > exact_precision) {
        return exact_digits(value);
    }
    if (kept <= 0) {
        // Beyond the last place by more than a digit: zero whatever the digits.
        return scientific_digits(magnitude, 0);
    }
    return rounding_digits(value, static_cast<std::size_t>(kept));
}

decimal_digits round_significant(const decimal_digits& value, std::size_t count)
{
    decimal_digits result{.digits = value.digits.substr(0, count), .exponent = value.exponent};
    const bool round_up = value.digits.size() > count && value.digits[count] >= '5';
    result.digits.resize(count, '0');
    if (!round_up) {
        return result;
    }
    for (std::size_t i = count; i > 0; --i) {
        char& digit = result.digits[i - 1];
        if (digit != '9') {
            ++digit;
            return result;
        }
        digit = '0';
    }
    // Every digit was a nine: the value is now a power of ten.
    if (count > 0) {
        result.digits.front() = '1';
    }
    ++result.exponent;
    return result;
}

fixed_digits round_fixed(const decimal_digits& value, std::size_t places)
{
    if (value.digits.empty()) {
        return layout({}, 0, places);
    }
    const std::int64_t kept = value.exponent + static_cast<std::int64_t>(places);
    if (kept > 0) {
        const decimal_digits rounded = round_significant(value, static_cast<std::size_t>(kept));
        return layout(rounded.digits, rounded.exponent, places);
    }
    // The first digit is beyond the last place: the value rounds to one unit of the last place, or to zero.
    if (kept == 0 && value.digits.front() >= '5') {
        return layout("1", value.exponent + 1, places);
    }
    return layout({}, 0, places);
}

} // namespace openfits::detail
