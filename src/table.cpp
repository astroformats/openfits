// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>
#include <openfits/image.h>
#include <openfits/table.h>

#include "core/ascii.h"
#include "core/keyword_name.h"
#include "core/quote.h"

#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace openfits {

namespace {

constexpr std::uint64_t max_uint64 = std::numeric_limits<std::uint64_t>::max();

[[noreturn]] void refuse_format(std::string_view text, std::string_view why)
{
    throw invalid_data_error(errc::invalid_column_format,
                             "the column format " + detail::quote(text) + " " + std::string(why));
}

// The letters of the types of Table 18 of standard §7.3.1, in the order of column_type.
constexpr std::array<std::pair<char, column_type>, 11> binary_letters{{
    {'L', column_type::logical},
    {'X', column_type::bit},
    {'B', column_type::uint8},
    {'I', column_type::int16},
    {'J', column_type::int32},
    {'K', column_type::int64},
    {'A', column_type::character},
    {'E', column_type::float32},
    {'D', column_type::float64},
    {'C', column_type::complex_float32},
    {'M', column_type::complex_float64},
}};

std::optional<column_type> binary_type(char letter) noexcept
{
    for (const auto& [code, type] : binary_letters) {
        if (code == letter) {
            return type;
        }
    }
    return std::nullopt;
}

char binary_letter(column_type type) noexcept
{
    for (const auto& [code, candidate] : binary_letters) {
        if (candidate == type) {
            return code;
        }
    }
    return 'B';
}

// The bytes of one element of a type of Table 18; bits are counted apart.
std::uint64_t element_bytes(column_type type) noexcept
{
    switch (type) {
    case column_type::logical:
    case column_type::bit:
    case column_type::uint8:
    case column_type::character:
        return 1;
    case column_type::int16:
        return 2;
    case column_type::int32:
    case column_type::float32:
        return 4;
    case column_type::int64:
    case column_type::float64:
    case column_type::complex_float32:
        return 8;
    case column_type::complex_float64:
        break;
    }
    return 16;
}

// The unsigned integer of the digits that start text at position, which advances past them; nothing without digits,
// and a refusal of text beyond 64 bits.
std::optional<std::uint64_t> read_digits(std::string_view text, std::size_t& position, std::string_view what)
{
    std::optional<std::uint64_t> value;
    while (position < text.size() && detail::is_digit(text[position])) {
        const auto digit = static_cast<std::uint64_t>(text[position] - '0');
        const std::uint64_t so_far = value.value_or(0);
        if (so_far > (max_uint64 - digit) / 10) {
            refuse_format(text, "has a " + std::string(what) + " beyond 64 bits");
        }
        value = (so_far * 10) + digit;
        ++position;
    }
    return value;
}

[[noreturn]] void refuse_ascii_format()
{
    throw usage_error(errc::invalid_argument,
                      "the format of a field of an ASCII table is none of Aw, Iw, Fw.d, Ew.d and Dw.d");
}

std::uint64_t checked_product(std::uint64_t a, std::uint64_t b)
{
    if (a != 0 && b > max_uint64 / a) {
        throw usage_error(errc::arithmetic_overflow, "the size of a field does not fit in 64 bits");
    }
    return a * b;
}

bool same_name(std::string_view left, std::string_view right, bool exact) noexcept
{
    left = detail::trim_trailing_spaces(left);
    right = detail::trim_trailing_spaces(right);
    if (exact) {
        return left == right;
    }
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
               return detail::to_upper(a) == detail::to_upper(b);
           });
}

} // namespace

column_format column_format::parse_binary(std::string_view text)
{
    const std::string_view form = detail::trim_spaces(text);
    std::size_t position = 0;
    const std::optional<std::uint64_t> repeat = read_digits(form, position, "repeat count");
    if (position == form.size()) {
        refuse_format(text, "has no type of standard Table 18");
    }
    column_format result;
    result.repeat = repeat.value_or(1);
    if (form[position] == 'P' || form[position] == 'Q') {
        result.storage = form[position] == 'P' ? column_storage::variable_p : column_storage::variable_q;
        ++position;
        if (result.repeat > 1) {
            refuse_format(text, "repeats a descriptor of a variable-length array, which only 0 and 1 may (standard "
                                "§7.3.1)");
        }
        if (position == form.size()) {
            refuse_format(text, "has no type for the arrays that its descriptors point to (standard §7.3.5)");
        }
    }
    const std::optional<column_type> type = binary_type(form[position]);
    if (!type) {
        refuse_format(text, "has no type of standard Table 18, in upper case");
    }
    result.type = *type;
    ++position;
    if (result.storage != column_storage::fixed) {
        if (position < form.size() && form[position] == '(') {
            ++position;
            const std::optional<std::uint64_t> most = read_digits(form, position, "emax");
            if (!most || position == form.size() || form[position] != ')') {
                refuse_format(text, "does not close the emax of its arrays with a parenthesis (standard §7.3.5)");
            }
            result.max_elements = most;
        }
    } else if (result.type == column_type::character) {
        // The rAw convention: the field holds r / w strings of w characters. A w beyond the field gives none.
        const std::optional<std::uint64_t> width = read_digits(form, position, "substring width");
        if (width == 0U) {
            refuse_format(text, "gives substrings of 0 characters");
        }
        if (width && *width <= result.repeat) {
            result.substring_width = width;
        }
    }
    // The characters after the form are left to software (standard §7.3.1, rTa).
    return result;
}

column_format column_format::parse_ascii(std::string_view text)
{
    const std::string_view form = detail::trim_spaces(text);
    if (form.empty()) {
        refuse_format(text, "is empty");
    }
    column_format result;
    display_format field;
    switch (form.front()) {
    case 'A':
        result.type = column_type::character;
        field.code = display_code::character;
        break;
    case 'I':
        result.type = column_type::int64;
        field.code = display_code::integer;
        break;
    case 'F':
        result.type = column_type::float64;
        field.code = display_code::fixed;
        break;
    case 'E':
        result.type = column_type::float64;
        field.code = display_code::exponential;
        break;
    case 'D':
        result.type = column_type::float64;
        field.code = display_code::double_exponential;
        break;
    default:
        refuse_format(text, "is none of Aw, Iw, Fw.d, Ew.d and Dw.d of standard Table 15, in upper case");
    }
    std::size_t position = 1;
    const std::optional<std::uint64_t> width = read_digits(form, position, "width");
    if (!width || *width == 0 || *width > display_format::max_width) {
        refuse_format(text, "has no width from 1 to " + std::to_string(display_format::max_width));
    }
    field.width = static_cast<std::uint32_t>(*width);
    const bool real = result.type == column_type::float64;
    if (real) {
        if (position == form.size() || form[position] != '.') {
            refuse_format(text, "has no number of decimals, which Fw.d, Ew.d and Dw.d give (standard Table 15)");
        }
        ++position;
        const std::optional<std::uint64_t> decimals = read_digits(form, position, "number of decimals");
        if (!decimals || *decimals >= *width) {
            refuse_format(text, "has no number of decimals below its width");
        }
        field.digits = static_cast<std::uint32_t>(*decimals);
    }
    if (position != form.size()) {
        refuse_format(text, "has characters after its format");
    }
    result.repeat = result.type == column_type::character ? *width : 1;
    result.field = field;
    return result;
}

std::string column_format::to_string() const
{
    if (field) {
        const display_format& format = *field;
        std::string text;
        switch (format.code) {
        case display_code::character:
            if (type != column_type::character || format.digits) {
                refuse_ascii_format();
            }
            return "A" + std::to_string(format.width);
        case display_code::integer:
            if (type != column_type::int64 || format.digits) {
                refuse_ascii_format();
            }
            return "I" + std::to_string(format.width);
        case display_code::fixed:
            text = "F";
            break;
        case display_code::exponential:
            text = "E";
            break;
        case display_code::double_exponential:
            text = "D";
            break;
        case display_code::logical:
        case display_code::binary:
        case display_code::octal:
        case display_code::hexadecimal:
        case display_code::engineering:
        case display_code::scientific:
        case display_code::general:
            refuse_ascii_format();
        }
        if (type != column_type::float64 || !format.digits || format.exponent_digits) {
            refuse_ascii_format();
        }
        return text + std::to_string(format.width) + "." + std::to_string(*format.digits);
    }
    std::string text = std::to_string(repeat);
    if (storage != column_storage::fixed) {
        if (repeat > 1 || substring_width) {
            throw usage_error(errc::invalid_argument,
                              "a column of variable-length arrays has 0 or 1 descriptors, and no substrings");
        }
        text += storage == column_storage::variable_p ? 'P' : 'Q';
    }
    text += binary_letter(type);
    if (storage != column_storage::fixed && max_elements) {
        text += "(" + std::to_string(*max_elements) + ")";
    }
    if (substring_width) {
        if (type != column_type::character || *substring_width == 0 || *substring_width > repeat) {
            throw usage_error(errc::invalid_argument,
                              "substrings are of characters, and have from 1 to the characters of the field");
        }
        text += std::to_string(*substring_width);
    }
    return text;
}

std::uint64_t column_format::field_size() const
{
    if (field) {
        return field->width;
    }
    switch (storage) {
    case column_storage::variable_p:
        return checked_product(repeat, 8);
    case column_storage::variable_q:
        return checked_product(repeat, 16);
    case column_storage::fixed:
        break;
    }
    if (type == column_type::bit) {
        return (repeat / 8) + (repeat % 8 == 0 ? 0 : 1);
    }
    return checked_product(repeat, element_bytes(type));
}

numeric_type column_info::value_type() const
{
    if (!format) {
        throw usage_error(errc::invalid_argument,
                          "the column has no format, which the type of its values follows from");
    }
    const auto integral = [](double value) { return std::isfinite(value) && value == std::trunc(value); };
    switch (format->type) {
    case column_type::character:
        throw usage_error(errc::invalid_argument, "the column holds characters, which are no numbers");
    case column_type::logical:
    case column_type::bit:
        return numeric_type::uint8;
    case column_type::complex_float32:
        return numeric_type::float32;
    case column_type::complex_float64:
        return numeric_type::float64;
    case column_type::int64:
        if (format->field) {
            return integral(scaling.scale) && integral(scaling.zero) ? numeric_type::int64 : numeric_type::float64;
        }
        break;
    case column_type::float64:
    case column_type::uint8:
    case column_type::int16:
    case column_type::int32:
    case column_type::float32:
        break;
    }
    // The stored types of numbers are those of images, and so are their physical values.
    image_info image;
    image.scaling = scaling;
    switch (format->type) {
    case column_type::uint8:
        image.bitpix = bitpix::uint8;
        break;
    case column_type::int16:
        image.bitpix = bitpix::int16;
        break;
    case column_type::int32:
        image.bitpix = bitpix::int32;
        break;
    case column_type::int64:
        image.bitpix = bitpix::int64;
        break;
    case column_type::float32:
        image.bitpix = bitpix::float32;
        break;
    default:
        image.bitpix = bitpix::float64;
        break;
    }
    return image.value_type();
}

std::optional<std::size_t> table_info::find_column(std::string_view name, bool exact) const
{
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (same_name(columns[i].name, name, exact)) {
            return i;
        }
    }
    return std::nullopt;
}

std::vector<std::size_t> table_info::matching_columns(std::string_view pattern, bool exact) const
{
    std::vector<std::size_t> found;
    const std::string_view wanted = detail::trim_trailing_spaces(pattern);
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (detail::match_keyword(wanted, detail::trim_trailing_spaces(columns[i].name), exact)) {
            found.push_back(i);
        }
    }
    return found;
}

column_values::column_values(std::span<const std::complex<float>> values) noexcept
    : kind_(kind::complex_float32), bytes_(std::as_bytes(values)), count_(values.size())
{}

column_values::column_values(std::span<const std::complex<double>> values) noexcept
    : kind_(kind::complex_float64), bytes_(std::as_bytes(values)), count_(values.size())
{}

column_values::column_values(std::span<const std::string> values) noexcept
    : kind_(kind::strings), strings_(values), count_(values.size())
{}

column_values column_values::logicals(std::span<const std::uint8_t> values,
                                      std::span<const std::uint8_t> undefined_mask) noexcept
{
    column_values result;
    result.kind_ = kind::logicals;
    result.bytes_ = std::as_bytes(values);
    result.undefined_ = undefined_mask;
    result.count_ = values.size();
    return result;
}

column_values column_values::bits(std::span<const std::uint8_t> values) noexcept
{
    column_values result;
    result.kind_ = kind::bits;
    result.bytes_ = std::as_bytes(values);
    result.count_ = values.size();
    return result;
}

column_values column_values::arrays(std::span<const std::complex<float>> values,
                                    std::span<const std::uint64_t> offsets) noexcept
{
    column_values result(values);
    result.offsets_ = offsets;
    result.variable_ = true;
    return result;
}

column_values column_values::arrays(std::span<const std::complex<double>> values,
                                    std::span<const std::uint64_t> offsets) noexcept
{
    column_values result(values);
    result.offsets_ = offsets;
    result.variable_ = true;
    return result;
}

column_values column_values::arrays(std::span<const std::string> values) noexcept
{
    column_values result(values);
    result.variable_ = true;
    return result;
}

table_info without_structural(table_info table)
{
    table.header = without_structural(table.header);
    return table;
}

} // namespace openfits
