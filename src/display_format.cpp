// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Display formats (standard §7.3.4): the output formats of Fortran that TDISPn values are, as the standard describes
// them, and as Fortran (ISO/IEC 1539-1:2004) defines what the standard leaves out.

#include <openfits/card.h>
#include <openfits/error.h>

#include "core/ascii.h"
#include "core/decimal.h"
#include "core/quote.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace openfits {

namespace {

struct code_letters
{
    display_code code = display_code::general;
    std::string_view letters{};
};

// Two-letter codes first, so that a parse tries them before E.
constexpr std::array<code_letters, 12> codes{{
    {.code = display_code::engineering, .letters = "EN"},
    {.code = display_code::scientific, .letters = "ES"},
    {.code = display_code::character, .letters = "A"},
    {.code = display_code::logical, .letters = "L"},
    {.code = display_code::integer, .letters = "I"},
    {.code = display_code::binary, .letters = "B"},
    {.code = display_code::octal, .letters = "O"},
    {.code = display_code::hexadecimal, .letters = "Z"},
    {.code = display_code::fixed, .letters = "F"},
    {.code = display_code::exponential, .letters = "E"},
    {.code = display_code::general, .letters = "G"},
    {.code = display_code::double_exponential, .letters = "D"},
}};

std::string_view letters_of(display_code code) noexcept
{
    for (const code_letters& entry : codes) {
        if (entry.code == code) {
            return entry.letters;
        }
    }
    return {};
}

bool is_integer_code(display_code code) noexcept
{
    return code == display_code::integer || code == display_code::binary || code == display_code::octal ||
           code == display_code::hexadecimal;
}

// The codes whose exponent can have a width of its own, Ee.
bool has_exponent_width(display_code code) noexcept
{
    return code == display_code::exponential || code == display_code::general ||
           code == display_code::double_exponential;
}

bool is_real_code(display_code code) noexcept
{
    return code == display_code::fixed || code == display_code::engineering || code == display_code::scientific ||
           has_exponent_width(code);
}

[[noreturn]] void throw_invalid(std::string_view text)
{
    throw invalid_data_error(errc::invalid_display_format,
                             detail::quote(text) + " is not a display format of the standard");
}

// True when format is a display format of the standard whose parts fit in its width: the digits of the integer codes at
// most the width; those of the real codes required and less than the width, and at least one for E, D and G; an
// exponent of at least one digit, for E, D and G only, whose e + 2 characters fit in the width. Rendering such a format
// allocates no more than its width.
bool is_valid(const display_format& format) noexcept
{
    if (letters_of(format.code).empty() || format.width == 0 || format.width > display_format::max_width) {
        return false;
    }
    if (format.exponent_digits && (!has_exponent_width(format.code) || *format.exponent_digits == 0 ||
                                   std::uint64_t{*format.exponent_digits} + 2 > format.width)) {
        return false;
    }
    if (is_integer_code(format.code)) {
        return !format.digits || *format.digits <= format.width;
    }
    if (is_real_code(format.code)) {
        return format.digits && *format.digits < format.width &&
               (!has_exponent_width(format.code) || *format.digits > 0);
    }
    // Character and logical data take no digits.
    return !format.digits;
}

// A format built by the application is checked as one read from TDISPn is.
void check(const display_format& format)
{
    if (!is_valid(format)) {
        throw_invalid(format.to_string());
    }
}

[[noreturn]] void throw_mismatch(const display_format& format, std::string_view data)
{
    throw invalid_data_error(errc::invalid_display_format,
                             "the display format " + format.to_string() + " cannot display " + std::string(data));
}

// The number at the start of text, which it removes, or nothing when there is none or it does not fit in 32 bits.
std::optional<std::uint32_t> take_number(std::string_view& text) noexcept
{
    std::uint64_t value = 0;
    std::size_t count = 0;
    while (count < text.size() && detail::is_digit(text[count])) {
        value = (value * 10) + static_cast<std::uint64_t>(text[count] - '0');
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
        ++count;
    }
    if (count == 0) {
        return std::nullopt;
    }
    text.remove_prefix(count);
    return static_cast<std::uint32_t>(value);
}

// ---------------------------------------------------------------------------------------------------------------------
// Fields

std::string asterisks(std::uint32_t width)
{
    // Not {width, '*'}, which would be the two characters of an initializer list.
    std::string field(width, '*');
    return field;
}

// text right-justified in a field of width characters, or asterisks when it does not fit.
std::string justify(const std::string& text, std::uint32_t width)
{
    if (text.size() > width) {
        return asterisks(width);
    }
    return std::string(width - text.size(), ' ') + text;
}

// A NaN or an infinity, as Fortran writes them: NaN, Inf or Infinity with its sign, right-justified.
std::string non_finite_field(double value, std::uint32_t width)
{
    if (std::isnan(value)) {
        return justify("NaN", width);
    }
    const std::string sign = value < 0 ? "-" : "";
    return justify(sign + (width >= sign.size() + 8 ? "Infinity" : "Inf"), width);
}

// The digits of magnitude in base, at least minimum of them (none for a zero of minimum zero), the letters of
// hexadecimal in upper case.
std::string unsigned_digits(std::uint64_t magnitude, int base, std::uint32_t minimum)
{
    std::array<char, 64> buffer{};
    char* const end = std::to_chars(buffer.data(), buffer.data() + buffer.size(), magnitude, base).ptr;
    std::string digits(buffer.data(), end);
    for (char& c : digits) {
        c = detail::to_upper(c);
    }
    if (magnitude == 0 && minimum == 0) {
        digits.clear();
    }
    if (digits.size() < minimum) {
        digits.insert(0, minimum - digits.size(), '0');
    }
    return digits;
}

// Iw.m, Bw.m, Ow.m and Zw.m. Only the decimal code has a sign: the others give the magnitude (standard §7.3.4).
std::string integer_field(const display_format& format, std::uint64_t magnitude, bool negative)
{
    const std::uint32_t minimum = format.digits.value_or(1);
    switch (format.code) {
    case display_code::binary:
        return justify(unsigned_digits(magnitude, 2, minimum), format.width);
    case display_code::octal:
        return justify(unsigned_digits(magnitude, 8, minimum), format.width);
    case display_code::hexadecimal:
        return justify(unsigned_digits(magnitude, 16, minimum), format.width);
    default:
        return justify((negative ? "-" : "") + unsigned_digits(magnitude, 10, minimum), format.width);
    }
}

// Fw.d of a finite value: the integer part, a 0 for none unless the field has no room for it, the point and the
// fraction.
std::string fixed_field(double value, std::uint32_t width, std::uint32_t places)
{
    const detail::fixed_digits digits = detail::round_fixed(detail::exact_digits(value), places);
    const std::string sign = std::signbit(value) ? "-" : "";
    std::string text = sign + (digits.integer.empty() ? "0" : digits.integer) + '.' + digits.fraction;
    if (text.size() > width && digits.integer.empty()) {
        text = sign + '.' + digits.fraction;
    }
    return justify(text, width);
}

// The exponent of Ew.dEe and the codes like it: the letter, the sign and e digits; the sign and e + 1 digits without
// the letter when it needs one more; nothing when it needs more.
std::optional<std::string> exponent_text(std::int64_t exponent, std::uint32_t digits, char letter)
{
    const std::uint64_t magnitude =
        exponent < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(exponent) : static_cast<std::uint64_t>(exponent);
    const std::string written = unsigned_digits(magnitude, 10, 1);
    const char sign = exponent < 0 ? '-' : '+';
    if (written.size() <= digits) {
        return std::string(1, letter) + sign + std::string(digits - written.size(), '0') + written;
    }
    if (written.size() == static_cast<std::size_t>(digits) + 1) {
        return sign + written;
    }
    return std::nullopt;
}

// The floor of a division by three, for the exponents of the engineering code.
std::int64_t floor_to_multiple_of_three(std::int64_t value) noexcept
{
    return value >= 0 ? (value / 3) * 3 : -(((-value) + 2) / 3) * 3;
}

// Ew.dEe, Dw.dEe, ESw.d and ENw.d of a finite value: a mantissa in the field that the exponent leaves, which is
// e + 2 characters wide.
std::string exponential_field(double value, const display_format& format)
{
    const std::uint32_t places = format.digits.value_or(0);
    const std::uint32_t exponent_digits = format.exponent_digits.value_or(2);
    const char letter = format.code == display_code::double_exponential ? 'D' : 'E';
    const detail::decimal_digits exact = detail::exact_digits(value);

    std::string mantissa = std::signbit(value) ? "-" : "";
    std::int64_t exponent = 0;
    switch (format.code) {
    case display_code::scientific: {
        // One digit before the point: 1 <= fraction < 10.
        const detail::decimal_digits rounded = detail::round_significant(exact, std::size_t{places} + 1);
        mantissa += rounded.digits.substr(0, 1) + '.' + rounded.digits.substr(1);
        exponent = exact.digits.empty() ? 0 : rounded.exponent - 1;
        break;
    }
    case display_code::engineering: {
        // One to three digits before the point, and an exponent that is a multiple of three.
        std::int64_t leading = exact.exponent - 1;
        std::size_t before = static_cast<std::size_t>(leading - floor_to_multiple_of_three(leading)) + 1;
        detail::decimal_digits rounded = detail::round_significant(exact, before + places);
        if (rounded.exponent != exact.exponent) {
            leading = rounded.exponent - 1;
            before = static_cast<std::size_t>(leading - floor_to_multiple_of_three(leading)) + 1;
            rounded.digits.resize(before + places, '0');
        }
        if (exact.digits.empty()) {
            before = 1;
            leading = 0;
        }
        mantissa += rounded.digits.substr(0, before) + '.' + rounded.digits.substr(before, places);
        exponent = floor_to_multiple_of_three(leading);
        break;
    }
    default: {
        // A zero before the point: 0.1 <= fraction < 1.
        const detail::decimal_digits rounded = detail::round_significant(exact, places);
        mantissa += "0." + rounded.digits;
        exponent = exact.digits.empty() ? 0 : rounded.exponent;
        break;
    }
    }

    const std::optional<std::string> exponent_part = exponent_text(exponent, exponent_digits, letter);
    const std::size_t exponent_width = std::size_t{exponent_digits} + 2;
    if (!exponent_part || format.width < exponent_width) {
        return asterisks(format.width);
    }
    const std::size_t mantissa_width = format.width - exponent_width;
    if (mantissa.size() > mantissa_width && format.code != display_code::scientific &&
        format.code != display_code::engineering) {
        // The zero before the point is optional.
        mantissa.erase(mantissa.find('0'), 1);
    }
    if (mantissa.size() > mantissa_width) {
        return asterisks(format.width);
    }
    return std::string(mantissa_width - mantissa.size(), ' ') + mantissa + *exponent_part;
}

// Gw.dEe of a finite value (standard §7.3.4): as Fw'.d' and e + 2 spaces when the value rounded to d significant
// digits has 0 to d digits before the point, as Ew.dEe otherwise; zero as F(w - e - 2).(d - 1), as Fortran writes it.
std::string general_field(double value, const display_format& format)
{
    const std::uint32_t places = format.digits.value_or(1);
    const std::uint32_t spaces = format.exponent_digits.value_or(2) + 2;
    const detail::decimal_digits exact = detail::exact_digits(value);
    std::uint32_t fraction = places - 1;
    if (!exact.digits.empty()) {
        const std::int64_t before = detail::round_significant(exact, places).exponent;
        if (before < 0 || std::cmp_greater(before, places)) {
            display_format exponential = format;
            exponential.code = display_code::exponential;
            return exponential_field(value, exponential);
        }
        fraction = places - static_cast<std::uint32_t>(before);
    }
    if (format.width < spaces) {
        return asterisks(format.width);
    }
    const std::string field = fixed_field(value, format.width - spaces, fraction);
    if (field.find('*') != std::string::npos) {
        return asterisks(format.width);
    }
    return field + std::string(spaces, ' ');
}

std::string real_field(const display_format& format, double value)
{
    if (!std::isfinite(value)) {
        return non_finite_field(value, format.width);
    }
    switch (format.code) {
    case display_code::fixed:
        return fixed_field(value, format.width, format.digits.value_or(0));
    case display_code::general:
        return general_field(value, format);
    default:
        return exponential_field(value, format);
    }
}

} // namespace

display_format display_format::parse(std::string_view text)
{
    std::string_view rest = detail::trim_spaces(text);
    display_format result;
    bool found = false;
    for (const code_letters& entry : codes) {
        if (rest.starts_with(entry.letters)) {
            result.code = entry.code;
            rest.remove_prefix(entry.letters.size());
            found = true;
            break;
        }
    }
    const std::optional<std::uint32_t> width = take_number(rest);
    if (!found || !width || *width == 0 || *width > max_width) {
        throw_invalid(text);
    }
    result.width = *width;

    if (is_integer_code(result.code) || is_real_code(result.code)) {
        if (rest.starts_with('.')) {
            rest.remove_prefix(1);
            result.digits = take_number(rest);
            if (!result.digits) {
                throw_invalid(text);
            }
        }
    }
    if (has_exponent_width(result.code) && rest.starts_with('E')) {
        rest.remove_prefix(1);
        result.exponent_digits = take_number(rest);
        if (!result.exponent_digits || *result.exponent_digits == 0) {
            throw_invalid(text);
        }
    }
    if (!rest.empty() || !is_valid(result)) {
        throw_invalid(text);
    }
    return result;
}

std::string display_format::to_string() const
{
    std::string text(letters_of(code));
    text += std::to_string(width);
    if (digits) {
        text += '.';
        text += std::to_string(*digits);
    }
    if (exponent_digits) {
        text += 'E';
        text += std::to_string(*exponent_digits);
    }
    return text;
}

std::string format_display(const display_format& format, std::string_view value)
{
    check(format);
    if (format.code != display_code::character && format.code != display_code::general) {
        throw_mismatch(format, "character data");
    }
    // Longer data are cut to their first characters (standard §7.3.4).
    return justify(std::string(value.substr(0, format.width)), format.width);
}

std::string format_display(const display_format& format, bool value)
{
    check(format);
    if (format.code != display_code::logical && format.code != display_code::general) {
        throw_mismatch(format, "logical data");
    }
    return justify(value ? "T" : "F", format.width);
}

std::string format_display(const display_format& format, std::int64_t value)
{
    check(format);
    if (format.code == display_code::character || format.code == display_code::logical) {
        throw_mismatch(format, "an integer");
    }
    if (is_integer_code(format.code) || format.code == display_code::general) {
        const std::uint64_t magnitude =
            value < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
        display_format integer = format;
        if (format.code == display_code::general) {
            integer.code = display_code::integer;
            integer.digits.reset();
        }
        return integer_field(integer, magnitude, value < 0);
    }
    return real_field(format, static_cast<double>(value));
}

std::string format_display(const display_format& format, std::uint64_t value)
{
    check(format);
    if (format.code == display_code::character || format.code == display_code::logical) {
        throw_mismatch(format, "an integer");
    }
    if (is_integer_code(format.code) || format.code == display_code::general) {
        display_format integer = format;
        if (format.code == display_code::general) {
            integer.code = display_code::integer;
            integer.digits.reset();
        }
        return integer_field(integer, value, false);
    }
    return real_field(format, static_cast<double>(value));
}

std::string format_display(const display_format& format, double value)
{
    check(format);
    if (!is_real_code(format.code)) {
        throw_mismatch(format, "a real");
    }
    return real_field(format, value);
}

std::string format_display(const display_format& format, std::complex<double> value)
{
    check(format);
    if (!is_real_code(format.code)) {
        throw_mismatch(format, "a complex value");
    }
    // The recommended form, in 2w + 3 characters (standard §7.3.4).
    return '(' + real_field(format, value.real()) + ',' + real_field(format, value.imag()) + ')';
}

} // namespace openfits
