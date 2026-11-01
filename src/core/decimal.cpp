// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "core/decimal.h"

#include <array>
#include <charconv>
#include <cmath>
#include <string_view>

namespace openfits::detail {

namespace {

// Every double has at most 767 significant decimal digits, so that 767 digits after the first are its exact value.
constexpr int exact_precision = 767;

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
    decimal_digits result;
    const double magnitude = std::fabs(value);
    if (magnitude == 0) {
        return result;
    }
    // d.ddd...e±x: the first digit, the decimal point, the other digits, and the exponent.
    std::array<char, 800> buffer{};
    const char* const end = std::to_chars(buffer.data(), buffer.data() + buffer.size(), magnitude,
                                          std::chars_format::scientific, exact_precision)
                                .ptr;
    const std::string_view text(buffer.data(), static_cast<std::size_t>(end - buffer.data()));
    const std::size_t letter = text.find('e');
    result.digits.reserve(letter);
    result.digits += text.front();
    result.digits.append(text.substr(2, letter - 2));
    result.digits.erase(result.digits.find_last_not_of('0') + 1);

    std::string_view exponent = text.substr(letter + 1);
    if (exponent.front() == '+') {
        exponent.remove_prefix(1);
    }
    int power = 0;
    (void)std::from_chars(exponent.data(), exponent.data() + exponent.size(), power);
    result.exponent = power + 1;
    return result;
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
