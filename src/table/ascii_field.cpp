// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "table/ascii_field.h"

#include "core/ascii.h"
#include "core/value_text.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>

namespace openfits::detail {

namespace {

// Exponents beyond this make every mantissa overflow or underflow double; a larger one is read as this.
constexpr std::int64_t exponent_cap = 100'000;

field_number invalid()
{
    return {.problem = errc::invalid_field};
}

field_number out_of_range()
{
    return {.problem = errc::value_out_of_range};
}

// The characters of a field after leading spaces, read one at a time with the spaces inside skipped.
class field_text
{
public:
    explicit field_text(std::string_view field) noexcept : text_(trim_spaces(field)) {}

    [[nodiscard]] bool done() noexcept
    {
        skip();
        return position_ == text_.size();
    }

    [[nodiscard]] char peek() noexcept
    {
        skip();
        return position_ < text_.size() ? text_[position_] : '\0';
    }

    void advance() noexcept
    {
        ++position_;
    }

    // The digits from the position, appended to digits; their number.
    std::size_t digits(std::string& digits)
    {
        std::size_t count = 0;
        while (is_digit(peek())) {
            digits += text_[position_];
            advance();
            ++count;
        }
        return count;
    }

private:
    void skip() noexcept
    {
        while (position_ < text_.size() && text_[position_] == ' ') {
            ++position_;
        }
    }

    std::string_view text_;
    std::size_t position_ = 0;
};

} // namespace

field_number read_integer_field(std::string_view field)
{
    field_text text(field);
    if (text.done()) {
        return {.value = std::int64_t{0}};
    }
    std::string token;
    if (text.peek() == '+' || text.peek() == '-') {
        token += text.peek();
        text.advance();
    }
    if (text.digits(token) == 0 || !text.done()) {
        return invalid();
    }
    std::optional<card_value> value = integer_value(token);
    if (!value) {
        return out_of_range();
    }
    return {.value = std::move(*value)};
}

field_number read_real_field(std::string_view field, std::uint32_t decimals)
{
    field_text text(field);
    if (text.done()) {
        return {.value = 0.0};
    }
    std::string sign;
    if (text.peek() == '+' || text.peek() == '-') {
        sign = text.peek() == '-' ? "-" : "";
        text.advance();
    }
    std::string whole;
    std::string fraction;
    std::size_t digits = text.digits(whole);
    const bool point = text.peek() == '.';
    if (point) {
        text.advance();
        digits += text.digits(fraction);
    }
    if (digits == 0) {
        return invalid();
    }
    // The exponent: a letter and an optionally signed integer, or a sign and an integer (standard §7.2.5, 3).
    std::string exponent_sign = "+";
    std::string exponent;
    const char next = text.peek();
    if (next == 'E' || next == 'D' || next == 'e' || next == 'd' || next == '+' || next == '-') {
        const bool letter = next != '+' && next != '-';
        if (letter) {
            text.advance();
        }
        if (text.peek() == '+' || text.peek() == '-') {
            exponent_sign = std::string(1, text.peek());
            text.advance();
        } else if (!letter) {
            return invalid();
        }
        if (text.digits(exponent) == 0) {
            return invalid();
        }
    }
    if (!text.done()) {
        return invalid();
    }
    if (!point) {
        // The implied decimal point precedes the rightmost decimals digits, with leading zeros as needed.
        if (whole.size() <= decimals) {
            fraction = std::string(decimals - whole.size(), '0') + whole;
            whole = "0";
        } else {
            fraction = whole.substr(whole.size() - decimals);
            whole.resize(whole.size() - decimals);
        }
    }
    std::int64_t power = 0;
    for (const char c : exponent) {
        power = std::min(exponent_cap, (power * 10) + (c - '0'));
    }
    const std::string token = sign + (whole.empty() ? "0" : whole) + "." + (fraction.empty() ? "0" : fraction) + "E" +
                              exponent_sign + std::to_string(power);
    const std::optional<double> value = real_value(token);
    if (!value) {
        return out_of_range();
    }
    return {.value = *value};
}

std::optional<std::int64_t> read_plain_integer_field(std::string_view field) noexcept
{
    const std::string_view text = trim_spaces(field);
    if (text.empty()) {
        return 0;
    }
    const bool negative = text.front() == '-';
    std::size_t i = negative || text.front() == '+' ? 1 : 0;
    // 18 digits always fit, so that the sum needs no check.
    if (i == text.size() || text.size() - i > 18) {
        return std::nullopt;
    }
    std::int64_t magnitude = 0;
    for (; i < text.size(); ++i) {
        if (!is_digit(text[i])) {
            return std::nullopt;
        }
        magnitude = (magnitude * 10) + (text[i] - '0');
    }
    return negative ? -magnitude : magnitude;
}

std::optional<double> read_plain_real_field(std::string_view field, std::uint32_t decimals) noexcept
{
    const std::string_view text = trim_spaces(field);
    if (text.empty()) {
        return 0.0;
    }
    std::size_t i = 0;
    const bool negative = text.front() == '-';
    if (negative || text.front() == '+') {
        ++i;
    }
    // The significant digits, from the first that is not 0, as an integer while they fit, and the digits after the
    // point.
    std::uint64_t mantissa = 0;
    std::size_t digits = 0;
    std::size_t significant = 0;
    std::int64_t fraction = 0;
    bool point = false;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '.' && !point) {
            point = true;
            continue;
        }
        if (!is_digit(c)) {
            break;
        }
        ++digits;
        fraction += point ? 1 : 0;
        if (significant > 0 || c != '0') {
            ++significant;
            if (significant <= 19) {
                mantissa = (mantissa * 10) + static_cast<std::uint64_t>(c - '0');
            }
        }
    }
    // An implied decimal point, read by read_real_field().
    if (digits == 0 || (!point && decimals > 0)) {
        return std::nullopt;
    }
    const std::size_t letter = i;
    std::int64_t exponent = 0;
    if (i < text.size()) {
        if (text[i] != 'E' && text[i] != 'D' && text[i] != 'e' && text[i] != 'd') {
            return std::nullopt;
        }
        ++i;
        const bool negative_exponent = i < text.size() && text[i] == '-';
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            ++i;
        }
        const std::size_t first = i;
        for (; i < text.size() && is_digit(text[i]); ++i) {
            exponent = std::min<std::int64_t>((exponent * 10) + (text[i] - '0'), exponent_cap);
        }
        if (i == first || i != text.size()) {
            return std::nullopt;
        }
        exponent = negative_exponent ? -exponent : exponent;
    }
    // Clinger's fast path: a mantissa that double holds exactly, multiplied or divided by a power of ten that it holds
    // exactly too, is rounded once, to the nearest double.
    constexpr std::array<double, 23> powers{1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                            1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    const std::int64_t power = exponent - fraction;
    if (significant <= 15 && power >= -22 && power <= 22) {
        const auto magnitude = static_cast<double>(mantissa);
        const double value = power < 0 ? magnitude / powers[static_cast<std::size_t>(-power)]
                                       : magnitude * powers[static_cast<std::size_t>(power)];
        return negative ? -value : value;
    }
    // The number as plain_real_value() takes it: no plus sign, and the letter E.
    std::array<char, 64> number{};
    if (text.size() > number.size()) {
        return std::nullopt;
    }
    std::size_t size = 0;
    for (std::size_t k = text.front() == '+' ? 1 : 0; k < text.size(); ++k) {
        number[size++] = k == letter ? 'E' : text[k];
    }
    return plain_real_value(std::span<const char>(number.data(), size));
}

} // namespace openfits::detail
