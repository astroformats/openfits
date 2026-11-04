// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "core/value_text.h"

#include "core/ascii.h"
#include "core/quote.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <complex>
#include <limits>
#include <system_error>
#include <utility>

#if (defined(_LIBCPP_VERSION) && _LIBCPP_VERSION < 200000) || (defined(_GLIBCXX_RELEASE) && _GLIBCXX_RELEASE < 12)
#include <cerrno>
#include <cstdlib>
#include <new>
#if defined(__APPLE__)
#include <xlocale.h>
#else
#include <locale.h>
#endif
#endif

namespace openfits::detail {

namespace {

std::size_t count_digits(std::string_view text, std::size_t from) noexcept
{
    std::size_t count = 0;
    while (from + count < text.size() && is_digit(text[from + count])) {
        ++count;
    }
    return count;
}

std::size_t skip_spaces(std::string_view text, std::size_t from) noexcept
{
    while (from < text.size() && text[from] == ' ') {
        ++from;
    }
    return from;
}

void depart(std::vector<diagnostic>& departures, severity level, errc code, std::string_view section,
            std::string message)
{
    departures.push_back({.severity = level,
                          .code = code,
                          .section = std::string(section),
                          .message = std::move(message),
                          .context = {}});
}

// ---------------------------------------------------------------------------------------------------------------------
// Reals

enum class conversion : std::uint8_t
{
    converted,
    out_of_range,
    failed,
};

#if (defined(_LIBCPP_VERSION) && _LIBCPP_VERSION < 200000) || (defined(_GLIBCXX_RELEASE) && _GLIBCXX_RELEASE < 12)

// libc++ implements floating-point std::from_chars only from LLVM 20, and that of libstdc++ before GCC 12 reports a
// subnormal result as out of range without storing it. strtod_l with the "C" locale reads the same syntax without
// depending on the global locale. Only an overflow is reported as out of range: an underflow keeps the rounded value,
// a zero or a subnormal.
conversion convert(const std::string& number, double& value)
{
    const locale_t c_locale = newlocale(LC_ALL_MASK, "C", locale_t{});
    if (c_locale == locale_t{}) {
        throw std::bad_alloc();
    }
    errno = 0;
    char* end = nullptr;
    value = strtod_l(number.c_str(), &end, c_locale);
    const bool overflow = errno == ERANGE && std::isinf(value);
    freelocale(c_locale);

    if (end != number.c_str() + number.size()) {
        return conversion::failed;
    }
    return overflow ? conversion::out_of_range : conversion::converted;
}

#else

conversion convert(const std::string& number, double& value) noexcept
{
    const char* const begin = number.data();
    const char* const end = begin + number.size();
    const auto result = std::from_chars(begin, end, value, std::chars_format::general);
    if (result.ec == std::errc::result_out_of_range) {
        return conversion::out_of_range;
    }
    if (result.ec != std::errc{} || result.ptr != end) {
        return conversion::failed;
    }
    return conversion::converted;
}

#endif

// True when the magnitude of a number of the grammar is at least one: when its conversion is out of range, this tells
// an overflow from an underflow. The exponent saturates far beyond the range of any type.
bool at_least_one(std::string_view token) noexcept
{
    std::size_t i = token.empty() || is_digit(token.front()) || token.front() == '.' ? 0 : 1;
    const std::size_t integer_digits = count_digits(token, i);
    const std::string_view integer_part = token.substr(i, integer_digits);
    i += integer_digits;
    std::string_view fraction_part;
    if (i < token.size() && token[i] == '.') {
        fraction_part = token.substr(i + 1, count_digits(token, i + 1));
        i += 1 + fraction_part.size();
    }
    std::int64_t exponent = 0;
    if (i < token.size()) {
        ++i;
        const bool negative = i < token.size() && token[i] == '-';
        if (i < token.size() && (token[i] == '+' || token[i] == '-')) {
            ++i;
        }
        constexpr std::int64_t saturation = 1'000'000'000'000'000;
        for (; i < token.size(); ++i) {
            exponent = std::min((exponent * 10) + (token[i] - '0'), saturation);
        }
        exponent = negative ? -exponent : exponent;
    }

    // The decimal exponent of the first nonzero digit.
    std::int64_t leading = 0;
    if (const std::size_t first = integer_part.find_first_not_of('0'); first != std::string_view::npos) {
        leading = static_cast<std::int64_t>(integer_part.size() - first) - 1;
    } else if (const std::size_t fraction_first = fraction_part.find_first_not_of('0');
               fraction_first != std::string_view::npos) {
        leading = -static_cast<std::int64_t>(fraction_first) - 1;
    } else {
        return false;
    }
    return leading + exponent >= 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Value fields

// The comment that follows a slash: without the space after the slash, and without trailing spaces.
std::string comment_text(std::string_view after_slash)
{
    if (!after_slash.empty() && after_slash.front() == ' ') {
        after_slash.remove_prefix(1);
    }
    return std::string(trim_trailing_spaces(after_slash));
}

// A string from its opening quote at field[begin] (standard §4.2.1.1). Returns the position after its closing quote.
std::size_t read_string(std::string_view field, std::size_t begin, card_value& value,
                        std::vector<diagnostic>& departures)
{
    std::string text;
    std::size_t i = begin + 1;
    bool closed = false;
    while (i < field.size()) {
        if (field[i] == '\'') {
            if (i + 1 < field.size() && field[i + 1] == '\'') {
                text += '\'';
                i += 2;
                continue;
            }
            closed = true;
            ++i;
            break;
        }
        text += field[i];
        ++i;
    }
    if (!closed) {
        depart(departures, severity::warning, errc::invalid_string, "4.2.1.1",
               "a string has no closing quote, and runs to the end of the record");
    }
    // Trailing spaces are not significant, but the first space of a string of spaces is: ' ' is the empty string, one
    // space, and not the null string '' (standard §4.2.1.1).
    text.erase(std::max<std::size_t>(trim_trailing_spaces(text).size(), text.empty() ? 0 : 1));
    value = card_value(std::move(text));
    return i;
}

// The value of one part of a complex number, or nothing.
std::optional<card_value> complex_part(std::string_view part, bool want_integer, std::vector<diagnostic>& departures)
{
    const scanned_number scanned = scan_number(part);
    if (scanned.lowercase_exponent) {
        depart(departures, severity::warning, errc::invalid_real, "4.2.4",
               "the exponent letter of " + quote(part) + " is in lower case");
    }
    if (want_integer) {
        return integer_value(part);
    }
    if (const std::optional<double> real = real_value(part)) {
        return card_value(*real);
    }
    return std::nullopt;
}

// A complex number from its opening parenthesis at field[begin] (standard §4.2.5, §4.2.6). Returns the position after
// its closing parenthesis, or the end of the value when it has none.
std::size_t read_complex(std::string_view field, std::size_t begin, card_value& value,
                         std::vector<diagnostic>& departures)
{
    const std::size_t close = field.find(')', begin);
    if (close == std::string_view::npos) {
        const std::size_t slash = std::min(field.find('/', begin), field.size());
        const std::string_view text = trim_trailing_spaces(field.substr(begin, slash - begin));
        depart(departures, severity::warning, errc::invalid_complex, "4.2.5",
               "the complex value " + quote(text) + " has no closing parenthesis");
        return begin + text.size();
    }
    const std::string_view inner = field.substr(begin + 1, close - begin - 1);
    const std::size_t comma = inner.find(',');
    const std::string_view real_part = trim_spaces(inner.substr(0, comma));
    const std::string_view imag_part =
        comma == std::string_view::npos ? std::string_view{} : trim_spaces(inner.substr(comma + 1));
    const scanned_number real_scan = scan_number(real_part);
    const scanned_number imag_scan = scan_number(imag_part);
    if (comma == std::string_view::npos || real_scan.form == number_form::not_a_number ||
        imag_scan.form == number_form::not_a_number) {
        depart(departures, severity::warning, errc::invalid_complex, "4.2.5",
               quote(field.substr(begin, close + 1 - begin)) + " is not a complex value");
        return close + 1;
    }

    const bool integers = real_scan.form == number_form::integer && imag_scan.form == number_form::integer;
    const std::optional<card_value> real = complex_part(real_part, integers, departures);
    const std::optional<card_value> imag = complex_part(imag_part, integers, departures);
    // An integer part from 2^63 to 2^64 - 1 is held in a std::uint64_t, which a complex integer cannot hold.
    if (!real || !imag ||
        (integers && (!std::holds_alternative<std::int64_t>(real->data()) ||
                      !std::holds_alternative<std::int64_t>(imag->data())))) {
        depart(departures, severity::warning, errc::value_out_of_range, "",
               "a part of the complex value " + quote(field.substr(begin, close + 1 - begin)) + " is out of range");
        return close + 1;
    }
    if (integers) {
        value = complex_integer{.real = real->get<std::int64_t>(), .imag = imag->get<std::int64_t>()};
    } else {
        value = std::complex<double>(real->get<double>(), imag->get<double>());
    }
    return close + 1;
}

// A logical or a number, the token of the value up to the next space or slash (standard §4.2.2 to §4.2.4).
void read_token(std::string_view token, card_value& value, std::vector<diagnostic>& departures)
{
    if (token == "T" || token == "F") {
        value = token == "T";
        return;
    }
    if (token == "t" || token == "f") {
        depart(departures, severity::warning, errc::invalid_logical, "4.2.2", "a logical value is in lower case");
        value = token == "t";
        return;
    }
    const scanned_number scanned = scan_number(token);
    switch (scanned.form) {
    case number_form::integer:
        if (const std::optional<card_value> integer = integer_value(token)) {
            value = *integer;
        } else {
            // The standard limits neither integers nor reals (§4.2.3, §4.2.4): a number beyond the types of the library
            // departs from no section.
            depart(departures, severity::warning, errc::value_out_of_range, "",
                   "the integer " + quote(token) + " is beyond -2^63 to 2^64 - 1");
        }
        return;
    case number_form::real:
        if (scanned.lowercase_exponent) {
            depart(departures, severity::warning, errc::invalid_real, "4.2.4",
                   "the exponent letter of " + quote(token) + " is in lower case");
        }
        if (const std::optional<double> real = real_value(token)) {
            value = *real;
        } else {
            depart(departures, severity::warning, errc::value_out_of_range, "",
                   "the real " + quote(token) + " is beyond the range of double");
        }
        return;
    case number_form::not_a_number:
        break;
    }
    depart(departures, severity::warning, errc::invalid_value, "4.2",
           quote(token) + " is none of the forms of keyword values");
}

// The value that starts at field[begin], into result. Returns the position after it.
std::size_t read_value(std::string_view field, std::size_t begin, bool at_byte_11, value_field& result,
                       std::vector<diagnostic>& departures)
{
    if (field[begin] == '\'') {
        result.fixed_format = at_byte_11 && begin == 0;
        return read_string(field, begin, result.value, departures);
    }
    if (field[begin] == '(') {
        return read_complex(field, begin, result.value, departures);
    }
    std::size_t end = std::min(field.find_first_of(" /", begin), field.size());
    const std::string_view token = field.substr(begin, end - begin);
    if (token != "T" && token != "F" && token != "t" && token != "f" &&
        scan_number(token).form == number_form::not_a_number) {
        // A value of none of the forms runs to the slash of the comment, as a string written without its quotes does,
        // its spaces included.
        const std::size_t slash = std::min(field.find('/', end), field.size());
        end = begin + trim_trailing_spaces(field.substr(begin, slash - begin)).size();
    }
    read_token(field.substr(begin, end - begin), result.value, departures);
    // Byte 30 is the twentieth of the field.
    result.fixed_format = at_byte_11 && end == 20 && result.value.kind() != value_kind::none;
    return end;
}

} // namespace

scanned_number scan_number(std::string_view token) noexcept
{
    std::size_t i = 0;
    if (i < token.size() && (token[i] == '+' || token[i] == '-')) {
        ++i;
    }
    const std::size_t integer_digits = count_digits(token, i);
    i += integer_digits;
    bool point = false;
    std::size_t fraction_digits = 0;
    if (i < token.size() && token[i] == '.') {
        point = true;
        fraction_digits = count_digits(token, i + 1);
        i += 1 + fraction_digits;
    }
    if (integer_digits + fraction_digits == 0) {
        return {};
    }
    bool exponent = false;
    bool lowercase = false;
    if (i < token.size() && (token[i] == 'E' || token[i] == 'D' || token[i] == 'e' || token[i] == 'd')) {
        exponent = true;
        lowercase = is_lower(token[i]);
        ++i;
        if (i < token.size() && (token[i] == '+' || token[i] == '-')) {
            ++i;
        }
        const std::size_t exponent_digits = count_digits(token, i);
        if (exponent_digits == 0) {
            return {};
        }
        i += exponent_digits;
    }
    if (i != token.size()) {
        return {};
    }
    return {.form = point || exponent ? number_form::real : number_form::integer, .lowercase_exponent = lowercase};
}

std::optional<card_value> integer_value(std::string_view token) noexcept
{
    bool negative = false;
    if (!token.empty() && (token.front() == '+' || token.front() == '-')) {
        negative = token.front() == '-';
        token.remove_prefix(1);
    }
    constexpr std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t magnitude = 0;
    for (const char c : token) {
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (magnitude > (max - digit) / 10) {
            return std::nullopt;
        }
        magnitude = (magnitude * 10) + digit;
    }
    if (!negative) {
        return card_value(magnitude);
    }
    constexpr auto int64_max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (magnitude > int64_max + 1) {
        return std::nullopt;
    }
    // Conversions to signed types are modular since C++20, so the negation of 2^63 gives -2^63.
    return card_value(static_cast<std::int64_t>(0 - magnitude));
}

std::optional<double> real_value(std::string_view token)
{
    // std::from_chars takes no leading plus sign and no exponent letter D.
    std::string number(!token.empty() && token.front() == '+' ? token.substr(1) : token);
    std::ranges::replace_if(number, [](char c) { return c == 'D' || c == 'd'; }, 'E');
    double value = 0;
    switch (convert(number, value)) {
    case conversion::converted:
        return value;
    case conversion::out_of_range:
        if (at_least_one(token)) {
            return std::nullopt;
        }
        return !token.empty() && token.front() == '-' ? -0.0 : 0.0;
    case conversion::failed:
        break;
    }
    return std::nullopt;
}

std::optional<double> plain_real_value(std::span<const char> number) noexcept
{
#if (defined(_LIBCPP_VERSION) && _LIBCPP_VERSION < 200000) || (defined(_GLIBCXX_RELEASE) && _GLIBCXX_RELEASE < 12)
    (void)number;
    return std::nullopt;
#else
    double value = 0;
    const char* const end = number.data() + number.size();
    const auto result = std::from_chars(number.data(), end, value, std::chars_format::general);
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::nullopt;
    }
    return value;
#endif
}

value_field read_value_field(std::string_view field, bool at_byte_11, std::vector<diagnostic>& departures)
{
    value_field result;
    const std::size_t begin = skip_spaces(field, 0);
    if (begin == field.size() || field[begin] == '/') {
        // A value indicator followed by blanks (standard §4.1.2.3).
        result.value = undefined;
        result.value_begin = begin;
        result.value_end = begin;
        result.fixed_format = true;
        if (begin < field.size()) {
            result.comment = comment_text(field.substr(begin + 1));
        }
        return result;
    }

    const std::size_t end = read_value(field, begin, at_byte_11, result, departures);
    result.value_begin = begin;
    result.value_end = end;

    const std::size_t next = skip_spaces(field, end);
    if (next < field.size()) {
        if (field[next] == '/') {
            result.comment = comment_text(field.substr(next + 1));
        } else {
            depart(departures, severity::warning, errc::invalid_comment, "4.1.2.3",
                   "the text after the value, " + quote(field.substr(next)) + ", does not start with a slash");
            result.comment = std::string(trim_trailing_spaces(field.substr(next)));
        }
    }
    return result;
}

std::string format_integer(std::int64_t value)
{
    std::array<char, 24> buffer{};
    return {buffer.data(), std::to_chars(buffer.data(), buffer.data() + buffer.size(), value).ptr};
}

std::string format_integer(std::uint64_t value)
{
    std::array<char, 24> buffer{};
    return {buffer.data(), std::to_chars(buffer.data(), buffer.data() + buffer.size(), value).ptr};
}

std::string format_real(double value)
{
    if (!std::isfinite(value)) {
        throw validation_error(errc::invalid_real, "a NaN or an infinity cannot be a keyword value");
    }
    std::array<char, 32> buffer{};
    std::string text(buffer.data(), std::to_chars(buffer.data(), buffer.data() + buffer.size(), value).ptr);
    bool real = false;
    for (char& c : text) {
        if (c == 'e') {
            c = 'E';
            real = true;
        } else if (c == '.') {
            real = true;
        }
    }
    if (!real) {
        text += '.';
    }
    return text;
}

std::string format_string(std::string_view value, bool any_bytes)
{
    if (!any_bytes && !is_printable(value)) {
        throw validation_error(errc::invalid_character,
                               "the string " + quote(value) + " holds a character outside printable ASCII");
    }
    // Trailing spaces are not significant, but a string of spaces is the empty string, one space, and not the null
    // string (standard §4.2.1.1).
    std::string_view significant = trim_trailing_spaces(value);
    if (significant.empty() && !value.empty()) {
        significant = value.substr(0, 1);
    }
    std::string text = "'";
    for (const char c : significant) {
        text += c;
        if (c == '\'') {
            text += '\'';
        }
    }
    constexpr std::size_t fixed_length = 8;
    if (!significant.empty() && text.size() - 1 < fixed_length) {
        text.append(fixed_length - (text.size() - 1), ' ');
    }
    text += '\'';
    return text;
}

std::string format_value(const card_value& value, bool any_bytes)
{
    switch (value.kind()) {
    case value_kind::undefined:
        return {};
    case value_kind::logical:
        return value.get<bool>() ? "T" : "F";
    case value_kind::integer:
        if (const auto* integer = std::get_if<std::int64_t>(&value.data())) {
            return format_integer(*integer);
        }
        return format_integer(value.get<std::uint64_t>());
    case value_kind::real:
        return format_real(value.get<double>());
    case value_kind::complex_integer: {
        const auto& number = value.get<complex_integer>();
        return '(' + format_integer(number.real) + ", " + format_integer(number.imag) + ')';
    }
    case value_kind::complex_real: {
        const auto& number = value.get<std::complex<double>>();
        return '(' + format_real(number.real()) + ", " + format_real(number.imag()) + ')';
    }
    case value_kind::string:
        return format_string(value.get<std::string>(), any_bytes);
    case value_kind::none:
        break;
    }
    throw validation_error(errc::invalid_value, "a keyword with a value indicator has no value to write");
}

card_value value_of_text(std::string_view text)
{
    std::vector<diagnostic> departures;
    value_field field = read_value_field(text, false, departures);
    // A slash after the value would have been read as the start of a comment, and other text after it as one.
    if (field.value_end != text.size()) {
        return {};
    }
    return std::move(field.value);
}

void check_value_text(std::string_view text, bool any_bytes)
{
    std::vector<diagnostic> departures;
    const value_field field = read_value_field(text, false, departures);
    for (const diagnostic& departure : departures) {
        if (departure.code != errc::invalid_character) {
            throw validation_error(departure.code,
                                   "the value text " + quote(text) + " is not of standard §4.2: " + departure.message);
        }
    }
    if (field.value.kind() == value_kind::none || field.value_end != text.size()) {
        throw validation_error(errc::invalid_value,
                               "the value text " + quote(text) + " is not one value of the forms of standard §4.2");
    }
    if (!any_bytes && !is_printable(text)) {
        throw validation_error(errc::invalid_character,
                               "the value text " + quote(text) + " holds a character outside printable ASCII");
    }
}

} // namespace openfits::detail
