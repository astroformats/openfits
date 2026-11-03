// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "table/table_write.h"

#include <openfits/error.h>

#include "core/ascii.h"
#include "core/endian.h"
#include "data/conversion.h"
#include "data/conversion_kernels.h"
#include "table/table_description.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openfits::detail {

namespace {

constexpr std::uint64_t block_size = 2880;
constexpr std::uint64_t max_uint64 = std::numeric_limits<std::uint64_t>::max();
constexpr double two_to_63 = 9223372036854775808.0;
constexpr double two_to_64 = 18446744073709551616.0;
// The largest count or offset of a P descriptor, which the standard makes a signed 32-bit integer (§7.3.5).
constexpr std::uint64_t max_p = 2147483647;

[[noreturn]] void refuse(const std::string& message, std::uint64_t hdu, std::uint64_t column)
{
    throw usage_error(errc::invalid_argument, message, {.hdu = hdu, .column = column});
}

[[noreturn]] void forbid(errc code, const std::string& message, std::uint64_t hdu, std::uint64_t column,
                         std::string keyword = {})
{
    throw validation_error(code, message, {.hdu = hdu, .keyword = std::move(keyword), .column = column});
}

std::string column_name(std::uint64_t number)
{
    return "column " + std::to_string(number);
}

std::uint64_t checked_product(std::uint64_t a, std::uint64_t b, std::uint64_t hdu)
{
    if (a != 0 && b > max_uint64 / a) {
        throw usage_error(errc::arithmetic_overflow, "the size of a table does not fit in 64 bits", {.hdu = hdu});
    }
    return a * b;
}

std::uint64_t checked_sum(std::uint64_t a, std::uint64_t b, std::uint64_t hdu)
{
    if (b > max_uint64 - a) {
        throw usage_error(errc::arithmetic_overflow, "the size of a table does not fit in 64 bits", {.hdu = hdu});
    }
    return a + b;
}

// The column type of a type of values, and the TZEROn of the convention that stores it with an offset (standard
// §5.2.5, Table 19).
std::pair<column_type, double> storage_of(numeric_type type) noexcept
{
    switch (type) {
    case numeric_type::int8:
        return {column_type::uint8, -128.0};
    case numeric_type::uint8:
        return {column_type::uint8, 0.0};
    case numeric_type::int16:
        return {column_type::int16, 0.0};
    case numeric_type::uint16:
        return {column_type::int16, 32768.0};
    case numeric_type::int32:
        return {column_type::int32, 0.0};
    case numeric_type::uint32:
        return {column_type::int32, 2147483648.0};
    case numeric_type::int64:
        return {column_type::int64, 0.0};
    case numeric_type::uint64:
        return {column_type::int64, two_to_63};
    case numeric_type::float32:
        return {column_type::float32, 0.0};
    case numeric_type::float64:
        break;
    }
    return {column_type::float64, 0.0};
}

// The BITPIX of the values that a numeric column stores, or of the parts of a complex one.
bitpix stored_bitpix(column_type type) noexcept
{
    switch (type) {
    case column_type::int16:
        return bitpix::int16;
    case column_type::int32:
        return bitpix::int32;
    case column_type::int64:
        return bitpix::int64;
    case column_type::float32:
    case column_type::complex_float32:
        return bitpix::float32;
    case column_type::float64:
    case column_type::complex_float64:
        return bitpix::float64;
    case column_type::logical:
    case column_type::bit:
    case column_type::uint8:
    case column_type::character:
        break;
    }
    return bitpix::uint8;
}

bool is_number(column_type type) noexcept
{
    switch (type) {
    case column_type::uint8:
    case column_type::int16:
    case column_type::int32:
    case column_type::int64:
    case column_type::float32:
    case column_type::float64:
        return true;
    case column_type::logical:
    case column_type::bit:
    case column_type::character:
    case column_type::complex_float32:
    case column_type::complex_float64:
        break;
    }
    return false;
}

bool is_integer(column_type type) noexcept
{
    return type == column_type::uint8 || type == column_type::int16 || type == column_type::int32 ||
           type == column_type::int64;
}

// The width of an Iw field that holds every value of an integer type, the sign included.
std::uint32_t integer_width(numeric_type type) noexcept
{
    switch (type) {
    case numeric_type::int8:
        return 4;
    case numeric_type::uint8:
        return 3;
    case numeric_type::int16:
        return 6;
    case numeric_type::uint16:
        return 5;
    case numeric_type::int32:
        return 11;
    case numeric_type::uint32:
        return 10;
    case numeric_type::int64:
    case numeric_type::uint64:
    case numeric_type::float32:
    case numeric_type::float64:
        break;
    }
    return 20;
}

// A real value as the card of a keyword: an integer when it is one, so that TZEROn = 32768 reads as the standard
// writes it, and a real otherwise.
card_value number(double value)
{
    if (value == std::trunc(value) && value >= -two_to_63 && value < two_to_63) {
        return static_cast<std::int64_t>(value);
    }
    if (value == std::trunc(value) && value >= two_to_63 && value < two_to_64) {
        return static_cast<std::uint64_t>(value);
    }
    return value;
}

std::uint64_t product_of(const std::vector<std::uint64_t>& dimensions, std::size_t from, std::uint64_t hdu)
{
    std::uint64_t product = 1;
    for (std::size_t i = from; i < dimensions.size(); ++i) {
        product = checked_product(product, dimensions[i], hdu);
    }
    return product;
}

// The bytes of the array of count elements of a type in the heap.
std::uint64_t array_bytes(column_type type, std::uint64_t count, std::uint64_t hdu)
{
    if (type == column_type::bit) {
        return (count / 8) + (count % 8 == 0 ? 0 : 1);
    }
    return checked_product(count, column_format{.type = type}.field_size(), hdu);
}

// The column type that a kind of values holds without a format of the caller's, and the scaling of its convention.
column_format derived_format(column_model& model, bool ascii, std::uint64_t hdu, std::uint64_t number)
{
    const values_view& values = model.values;
    const std::vector<std::uint64_t>& dimensions = model.info.dimensions;
    if (model.info.scaling != scaling{}) {
        refuse(column_name(number) + " has a scaling and no format, which the scaling converts its values into", hdu,
               number);
    }
    column_format format;
    if (ascii) {
        if (values.variable) {
            refuse(column_name(number) + " holds variable-length arrays, which an ASCII table cannot hold", hdu,
                   number);
        }
        switch (values.kind) {
        case values_kind::strings: {
            std::uint64_t longest = 1;
            for (const std::string& text : values.strings) {
                longest = std::max<std::uint64_t>(longest, text.size());
            }
            return column_format::parse_ascii("A" + std::to_string(longest));
        }
        case values_kind::numbers:
            if (values.type == numeric_type::float32) {
                return column_format::parse_ascii("E16.9");
            }
            if (values.type == numeric_type::float64) {
                return column_format::parse_ascii("D25.17");
            }
            return column_format::parse_ascii("I" + std::to_string(integer_width(values.type)));
        case values_kind::complex_float32:
        case values_kind::complex_float64:
        case values_kind::logicals:
        case values_kind::bits:
            break;
        }
        refuse(column_name(number) + " holds complex values, logicals or bits, which an ASCII table cannot hold", hdu,
               number);
    }
    const std::uint64_t elements = dimensions.empty() ? 1 : product_of(dimensions, 0, hdu);
    format.repeat = values.variable ? 1 : elements;
    if (values.variable) {
        format.storage = column_storage::variable_p;
    }
    switch (values.kind) {
    case values_kind::numbers: {
        const auto [type, zero] = storage_of(values.type);
        format.type = type;
        model.info.scaling.zero = zero;
        break;
    }
    case values_kind::complex_float32:
        format.type = column_type::complex_float32;
        break;
    case values_kind::complex_float64:
        format.type = column_type::complex_float64;
        break;
    case values_kind::logicals:
        format.type = column_type::logical;
        break;
    case values_kind::bits:
        format.type = column_type::bit;
        break;
    case values_kind::strings:
        format.type = column_type::character;
        if (!values.variable) {
            if (!dimensions.empty()) {
                format.repeat = elements;
            } else {
                std::uint64_t longest = 0;
                for (const std::string& text : values.strings) {
                    longest = std::max<std::uint64_t>(longest, text.size());
                }
                format.repeat = longest;
            }
        }
        break;
    }
    return format;
}

// True when values of the kind may be stored in a column of the format.
bool holds(const column_format& format, values_kind kind) noexcept
{
    if (format.field) {
        return format.type == column_type::character ? kind == values_kind::strings : kind == values_kind::numbers;
    }
    switch (kind) {
    case values_kind::numbers:
        return is_number(format.type);
    case values_kind::complex_float32:
    case values_kind::complex_float64:
        return format.type == column_type::complex_float32 || format.type == column_type::complex_float64;
    case values_kind::strings:
        return format.type == column_type::character;
    case values_kind::logicals:
        return format.type == column_type::logical;
    case values_kind::bits:
        return format.type == column_type::bit;
    }
    return false;
}

// The checks of the keywords of a column that its model gives: TSCALn, TZEROn, TNULLn, TDISPn and TDIMn.
void check_keywords(const column_model& model, bool ascii, std::uint64_t hdu, std::uint64_t number)
{
    const column_info& info = model.info;
    const column_format& format = model.format;
    const std::string n = std::to_string(number);
    if (!std::isfinite(info.scaling.scale) || info.scaling.scale == 0.0 || !std::isfinite(info.scaling.zero)) {
        forbid(errc::reserved_keyword, "TSCAL" + n + " is 0, or TSCAL" + n + " or TZERO" + n + " is not finite", hdu,
               number, "TSCAL" + n);
    }
    const bool scalable = ascii ? format.type != column_type::character
                                : is_number(format.type) || format.type == column_type::complex_float32 ||
                                      format.type == column_type::complex_float64;
    if (!scalable && info.scaling != scaling{}) {
        forbid(errc::reserved_keyword,
               "TSCAL" + n + " and TZERO" + n +
                   " are not for characters, logicals and bits (standard §7.2.2, §7.3.2), and " + column_name(number) +
                   " has a scaling",
               hdu, number, "TSCAL" + n);
    }
    if (info.null) {
        if (ascii || !is_integer(format.type)) {
            forbid(errc::reserved_keyword,
                   "TNULL" + n + " of an integer is for the integer columns of binary tables (standard §7.3.2)", hdu,
                   number, "TNULL" + n);
        }
        const std::int64_t null = *info.null;
        const bool fits = format.type == column_type::uint8   ? null >= 0 && null <= 255
                          : format.type == column_type::int16 ? std::in_range<std::int16_t>(null)
                          : format.type == column_type::int32 ? std::in_range<std::int32_t>(null)
                                                              : true;
        if (!fits) {
            forbid(errc::reserved_keyword, "TNULL" + n + ", " + std::to_string(null) + ", is not a value of the column",
                   hdu, number, "TNULL" + n);
        }
    }
    if (info.null_string) {
        if (!ascii) {
            forbid(errc::reserved_keyword, "TNULL" + n + " of a string is for ASCII tables (standard §7.2.2)", hdu,
                   number, "TNULL" + n);
        }
        if (info.null_string->size() > format.field_size() || !is_printable(*info.null_string)) {
            forbid(errc::reserved_keyword,
                   "TNULL" + n + " is longer than the field of " + column_name(number) +
                       ", or holds characters outside printable ASCII",
                   hdu, number, "TNULL" + n);
        }
    }
    if (info.display && !displays(*info.display, format)) {
        forbid(errc::invalid_display_format,
               "TDISP" + n + " = '" + info.display->to_string() + "' cannot display the values of " +
                   column_name(number) + " (standard §7.3.4)",
               hdu, number, "TDISP" + n);
    }
    if (!info.dimensions.empty()) {
        if (ascii) {
            forbid(errc::invalid_dimensions,
                   "the fields of an ASCII table hold one value, and " + column_name(number) + " has dimensions", hdu,
                   number, "TDIM" + n);
        }
        if (std::ranges::find(info.dimensions, 0U) != info.dimensions.end()) {
            forbid(errc::invalid_dimensions, "TDIM" + n + " has a dimension of 0", hdu, number, "TDIM" + n);
        }
        if (format.storage == column_storage::fixed && product_of(info.dimensions, 0, hdu) > format.repeat) {
            forbid(errc::invalid_dimensions,
                   "TDIM" + n + " gives an array larger than the " + std::to_string(format.repeat) +
                       " elements of the field (standard §7.3.2)",
                   hdu, number, "TDIM" + n);
        }
        if (format.substring_width &&
            info.dimensions !=
                std::vector<std::uint64_t>{*format.substring_width, format.repeat / *format.substring_width}) {
            forbid(errc::invalid_dimensions,
                   "the dimensions of " + column_name(number) + " are not those of its rAw form", hdu, number,
                   "TDIM" + n);
        }
    }
}

// The strings of a column of characters: each within its field, and of printable ASCII (standard §7.2.5, §7.3.3.1).
void check_strings(const column_model& model, std::uint64_t hdu, std::uint64_t number)
{
    for (std::size_t i = 0; i < model.values.strings.size(); ++i) {
        const std::string& text = model.values.strings[i];
        if (!model.values.variable && text.size() > model.string_width) {
            refuse("the string " + std::to_string(i) + " of " + column_name(number) + " has " +
                       std::to_string(text.size()) + " characters, more than the " +
                       std::to_string(model.string_width) + " of its field",
                   hdu, number);
        }
        if (!is_printable(text)) {
            forbid(errc::invalid_character,
                   "the string " + std::to_string(i) + " of " + column_name(number) +
                       " holds characters outside printable ASCII",
                   hdu, number);
        }
    }
}

// The elements of the fields of a column, or of its arrays, and their number against the rows.
void count_values(column_model& model, std::uint64_t rows, std::uint64_t hdu, std::uint64_t number)
{
    const column_format& format = model.format;
    const values_view& values = model.values;
    if (values.kind == values_kind::logicals && !values.undefined.empty() && values.undefined.size() != values.count) {
        refuse("the undefined logicals of " + column_name(number) + " do not have a byte for each value", hdu, number);
    }
    if (values.variable) {
        if (values.kind == values_kind::strings) {
            if (values.count != rows) {
                refuse(column_name(number) + " has " + std::to_string(values.count) + " strings for " +
                           std::to_string(rows) + " rows",
                       hdu, number);
            }
            // An empty string is one NUL byte, as CFITSIO writes it: its string reads refuse an empty array.
            for (const std::string& text : values.strings) {
                model.counts.push_back(std::max<std::uint64_t>(text.size(), 1));
            }
            return;
        }
        const std::span<const std::uint64_t> offsets = values.offsets;
        if (offsets.size() != rows + 1 && (rows != 0 || !offsets.empty())) {
            refuse("the offsets of the arrays of " + column_name(number) + " have " + std::to_string(offsets.size()) +
                       " entries, and the rows need " + std::to_string(rows + 1),
                   hdu, number);
        }
        for (std::size_t i = 0; i + 1 < offsets.size(); ++i) {
            if (offsets[i + 1] < offsets[i] || offsets[i + 1] > values.count) {
                refuse("the offsets of the arrays of " + column_name(number) +
                           " decrease, or pass the end of the values",
                       hdu, number);
            }
            model.counts.push_back(offsets[i + 1] - offsets[i]);
        }
        return;
    }
    std::uint64_t per_row = 1;
    if (format.field) {
        per_row = 1;
    } else if (values.kind == values_kind::strings) {
        if (format.substring_width) {
            model.string_width = *format.substring_width;
            model.strings = format.repeat / *format.substring_width;
        } else if (!model.info.dimensions.empty()) {
            model.string_width = model.info.dimensions.front();
            model.strings = product_of(model.info.dimensions, 1, hdu);
        } else {
            model.string_width = format.repeat;
            model.strings = 1;
        }
        per_row = model.strings;
    } else {
        per_row = format.repeat;
    }
    if (format.field && values.kind == values_kind::strings) {
        model.string_width = format.field_size();
        model.strings = 1;
    }
    model.elements = per_row;
    if (values.count != checked_product(rows, per_row, hdu)) {
        refuse("the values of " + column_name(number) + " are " + std::to_string(values.count) + ", and the " +
                   std::to_string(rows) + " rows need " + std::to_string(rows * per_row),
               hdu, number);
    }
}

} // namespace

values_view column_values_access::view(const column_values& values) noexcept
{
    values_view view;
    switch (values.kind_) {
    case column_values::kind::numbers:
        view.kind = values_kind::numbers;
        break;
    case column_values::kind::complex_float32:
        view.kind = values_kind::complex_float32;
        break;
    case column_values::kind::complex_float64:
        view.kind = values_kind::complex_float64;
        break;
    case column_values::kind::strings:
        view.kind = values_kind::strings;
        break;
    case column_values::kind::logicals:
        view.kind = values_kind::logicals;
        break;
    case column_values::kind::bits:
        view.kind = values_kind::bits;
        break;
    }
    view.type = values.type_;
    view.bytes = values.bytes_;
    view.strings = values.strings_;
    view.undefined = values.undefined_;
    view.offsets = values.offsets_;
    view.variable = values.variable_;
    view.count = values.count_;
    return view;
}

table_model resolve_table(table_info info, std::span<const column_values> values, const table_write_options& options,
                          std::uint64_t hdu)
{
    if (info.kind != hdu_kind::table && info.kind != hdu_kind::bintable) {
        throw usage_error(errc::invalid_argument,
                          "the table of unit " + std::to_string(hdu) + " is neither an ASCII nor a binary table",
                          {.hdu = hdu});
    }
    if (options.scaling == scaling_policy::override) {
        throw usage_error(errc::invalid_argument,
                          "the values of the table of unit " + std::to_string(hdu) +
                              " are physical or stored: scaling_policy::override is for reads and images",
                          {.hdu = hdu});
    }
    if (values.size() != info.columns.size()) {
        throw usage_error(errc::invalid_argument,
                          "the table of unit " + std::to_string(hdu) + " has " + std::to_string(info.columns.size()) +
                              " columns and values for " + std::to_string(values.size()),
                          {.hdu = hdu});
    }
    if (info.columns.size() > 999) {
        throw validation_error(errc::invalid_structure,
                               "a table has at most 999 columns (standard §7.2.1, §7.3.1), and that of unit " +
                                   std::to_string(hdu) + " has " + std::to_string(info.columns.size()),
                               {.hdu = hdu, .keyword = "TFIELDS"});
    }
    const bool ascii = info.kind == hdu_kind::table;
    table_model table;
    table.options = options;
    table.columns.reserve(info.columns.size());
    std::uint64_t offset = 0;
    for (std::size_t i = 0; i < info.columns.size(); ++i) {
        const std::uint64_t number = i + 1;
        column_model model;
        model.info = std::move(info.columns[i]);
        model.values = column_values_access::view(values[i]);
        if (!model.info.format) {
            const values_kind kind = model.values.kind;
            if (options.scaling == scaling_policy::raw &&
                (kind == values_kind::numbers || kind == values_kind::complex_float32 ||
                 kind == values_kind::complex_float64)) {
                refuse("the stored values of " + column_name(number) + " are written into its format, which it lacks",
                       hdu, number);
            }
            model.info.format = derived_format(model, ascii, hdu, number);
            model.derived = true;
        } else if (ascii != model.info.format->field.has_value()) {
            forbid(errc::invalid_column_format,
                   "the format of " + column_name(number) + " is one of the other kind of table", hdu, number,
                   "TFORM" + std::to_string(number));
        }
        model.format = *model.info.format;
        column_format& format = model.format;
        if (!holds(format, model.values.kind)) {
            refuse("the values of " + column_name(number) + " are not of a kind that its format " + format.to_string() +
                       " holds",
                   hdu, number);
        }
        if (model.values.variable != (format.storage != column_storage::fixed)) {
            refuse(column_name(number) + (model.values.variable
                                              ? " holds variable-length arrays, and its format fixed fields"
                                              : " holds fixed fields, and its format variable-length arrays"),
                   hdu, number);
        }
        if (format.storage != column_storage::fixed && format.repeat != 1) {
            forbid(errc::invalid_column_format,
                   "a column of variable-length arrays has one descriptor in each row, and " + column_name(number) +
                       " has " + std::to_string(format.repeat),
                   hdu, number, "TFORM" + std::to_string(number));
        }
        check_keywords(model, ascii, hdu, number);
        count_values(model, info.rows, hdu, number);
        if (model.values.kind == values_kind::strings) {
            check_strings(model, hdu, number);
        }
        if (ascii) {
            // One space between the fields, as is common (standard §7.2.4).
            model.info.offset = offset;
            offset =
                checked_sum(checked_sum(offset, format.field_size(), hdu), i + 1 < info.columns.size() ? 1 : 0, hdu);
        }
        table.columns.push_back(std::move(model));
    }
    // The heap: the arrays of each column of variable-length arrays in the order of the rows, compact. A format derived
    // from the values takes Q descriptors when P ones cannot hold a count or an offset; a P of the caller's is refused
    // then.
    std::uint64_t heap = 0;
    for (std::size_t i = 0; i < table.columns.size(); ++i) {
        column_model& model = table.columns[i];
        column_format& format = model.format;
        if (format.storage == column_storage::fixed) {
            continue;
        }
        std::uint64_t most = 0;
        bool wide = false;
        for (const std::uint64_t count : model.counts) {
            // An empty array has no offset, which "should be set to zero" (standard §7.3.5).
            model.heap_offsets.push_back(count == 0 ? 0 : heap);
            wide = wide || count > max_p || (count > 0 && heap > max_p);
            heap = checked_sum(heap, array_bytes(format.type, count, hdu), hdu);
            most = std::max(most, count);
        }
        if (wide && format.storage == column_storage::variable_p) {
            if (!model.derived) {
                forbid(errc::invalid_column_format,
                       "the P descriptors of " + column_name(i + 1) +
                           " cannot hold the counts or offsets of its arrays in the heap, which need Q ones",
                       hdu, i + 1, "TFORM" + std::to_string(i + 1));
            }
            format.storage = column_storage::variable_q;
        }
        format.max_elements = std::max(format.max_elements.value_or(0), most);
    }
    // The offsets of the fields follow from the descriptors chosen.
    if (!ascii) {
        offset = 0;
        for (column_model& model : table.columns) {
            model.info.offset = offset;
            offset = checked_sum(offset, model.format.field_size(), hdu);
        }
    }
    for (column_model& model : table.columns) {
        model.info.format = model.format;
    }
    table.heap_size = heap;
    table.info = std::move(info);
    table.info.columns.clear();
    table.info.row_size = offset;
    table.info.heap_offset = checked_product(offset, table.info.rows, hdu);
    table.info.heap_size = heap;
    (void)checked_sum(table.info.heap_offset, checked_sum(heap, block_size, hdu), hdu);
    return table;
}

std::vector<card> table_cards(const table_model& table)
{
    const bool ascii = table.info.kind == hdu_kind::table;
    std::vector<card> cards;
    cards.reserve(8 + (table.columns.size() * 8));
    cards.emplace_back("XTENSION", ascii ? "TABLE" : "BINTABLE",
                       ascii ? "ASCII table extension" : "binary table extension");
    cards.emplace_back("BITPIX", 8, ascii ? "ASCII characters" : "8-bit bytes");
    cards.emplace_back("NAXIS", 2, "2-dimensional table");
    cards.emplace_back("NAXIS1", table.info.row_size, ascii ? "characters in a row" : "bytes in a row");
    cards.emplace_back("NAXIS2", table.info.rows, "rows");
    cards.emplace_back("PCOUNT", table.heap_size, ascii ? "no heap" : "bytes of the heap");
    cards.emplace_back("GCOUNT", 1, "one table");
    cards.emplace_back("TFIELDS", table.columns.size(), "columns");
    for (std::size_t i = 0; i < table.columns.size(); ++i) {
        const column_info& column = table.columns[i].info;
        const column_format& format = table.columns[i].format;
        const std::string n = std::to_string(i + 1);
        if (!column.name.empty()) {
            cards.emplace_back("TTYPE" + n, column.name, "name of column " + n);
        }
        if (ascii) {
            cards.emplace_back("TBCOL" + n, column.offset + 1, "first character of column " + n);
        }
        cards.emplace_back("TFORM" + n, format.to_string(), "format of column " + n);
        if (!column.unit.empty()) {
            cards.emplace_back("TUNIT" + n, column.unit, "unit of column " + n);
        }
        if (column.scaling.scale != 1.0) {
            cards.emplace_back("TSCAL" + n, number(column.scaling.scale), "physical = TZERO + TSCAL x stored");
        }
        if (column.scaling.zero != 0.0) {
            cards.emplace_back("TZERO" + n, number(column.scaling.zero), "physical value of a stored 0");
        }
        if (column.null) {
            cards.emplace_back("TNULL" + n, *column.null, "stored value of undefined elements");
        }
        if (column.null_string) {
            cards.emplace_back("TNULL" + n, *column.null_string, "text of undefined fields");
        }
        if (column.display) {
            cards.emplace_back("TDISP" + n, column.display->to_string(), "display format of column " + n);
        }
        if (!column.dimensions.empty() && !format.substring_width) {
            cards.emplace_back("TDIM" + n, format_dimensions(column.dimensions), "dimensions of column " + n);
        }
    }
    return cards;
}

std::uint64_t padded_table_size(const table_model& table) noexcept
{
    const std::uint64_t size = table.info.heap_offset + table.heap_size;
    return (size + block_size - 1) / block_size * block_size;
}

namespace {

// The rendering of a value in the field of an ASCII table: a Fortran output format, the exponent letter of E and D
// kept with three digits where the field has room, since readers that know only the letter read that form.
std::string render_field(const display_format& format, const std::string& text)
{
    if (format.code != display_code::exponential && format.code != display_code::double_exponential) {
        return text;
    }
    const std::size_t sign = text.find_last_of("+-");
    if (sign == std::string::npos || sign == 0 || !is_digit(text[sign - 1]) || text.front() != ' ') {
        return text;
    }
    // A three-digit exponent without its letter (standard §7.2.5, 3a), and a leading space to give it.
    return text.substr(1, sign - 1) + (format.code == display_code::exponential ? "E" : "D") + text.substr(sign);
}

// Writes the field of a number into an ASCII table, as its Fortran format writes it, or TNULLn for NaN.
template <typename T>
void write_ascii_number(const column_model& model, const display_format& format, const scaling& s, T value, char* field,
                        std::uint64_t hdu, std::uint64_t number)
{
    const column_info& info = model.info;
    const auto width = static_cast<std::size_t>(format.width);
    std::string text;
    bool missing = false;
    if constexpr (std::floating_point<T>) {
        missing = std::isnan(value);
    }
    if (missing) {
        if (!info.null_string) {
            throw invalid_data_error(errc::undefined_value,
                                     "a NaN of " + column_name(number) + " of unit " + std::to_string(hdu) +
                                         " cannot be written without TNULL" + std::to_string(number),
                                     {.hdu = hdu, .column = number});
        }
        std::ranges::copy(*info.null_string, field);
        return;
    }
    const bool identity = s.scale == 1.0 && s.zero == 0.0;
    if (format.code == display_code::integer) {
        if constexpr (std::integral<T>) {
            if (identity) {
                text = std::is_signed_v<T> ? format_display(format, static_cast<std::int64_t>(value))
                                           : format_display(format, static_cast<std::uint64_t>(value));
            }
        }
        if (text.empty()) {
            const double difference = static_cast<double>(value) - s.zero;
            const double stored = difference / s.scale;
            if (!std::isfinite(stored) || !kernels::real_fits<std::int64_t>(stored, rounding::nearest)) {
                text = std::string(width, '*');
            } else {
                text = format_display(format, kernels::real_to_integer<std::int64_t>(stored, rounding::nearest));
            }
        }
    } else {
        auto stored = static_cast<double>(value);
        if (!identity) {
            const double difference = stored - s.zero;
            stored = difference / s.scale;
        }
        text = std::isfinite(stored) ? render_field(format, format_display(format, stored)) : std::string(width, '*');
    }
    if (text.find('*') != std::string::npos) {
        throw invalid_data_error(errc::value_out_of_range,
                                 "a value of " + column_name(number) + " of unit " + std::to_string(hdu) +
                                     " does not fit in its field " + model.format.to_string(),
                                 {.hdu = hdu, .column = number});
    }
    std::ranges::copy(text, field);
}

template <typename Function> decltype(auto) with_type(numeric_type type, const Function& function)
{
    switch (type) {
    case numeric_type::int8:
        return function(std::type_identity<std::int8_t>{});
    case numeric_type::uint8:
        return function(std::type_identity<std::uint8_t>{});
    case numeric_type::int16:
        return function(std::type_identity<std::int16_t>{});
    case numeric_type::uint16:
        return function(std::type_identity<std::uint16_t>{});
    case numeric_type::int32:
        return function(std::type_identity<std::int32_t>{});
    case numeric_type::uint32:
        return function(std::type_identity<std::uint32_t>{});
    case numeric_type::int64:
        return function(std::type_identity<std::int64_t>{});
    case numeric_type::uint64:
        return function(std::type_identity<std::uint64_t>{});
    case numeric_type::float32:
        return function(std::type_identity<float>{});
    case numeric_type::float64:
        break;
    }
    return function(std::type_identity<double>{});
}

void write_ascii_rows(output_sink& sink, const table_model& table, const std::function<void(std::uint64_t)>& advance,
                      std::uint64_t hdu, std::size_t transfer)
{
    const std::uint64_t row_size = table.info.row_size;
    const std::uint64_t rows = table.info.rows;
    if (row_size == 0 || rows == 0) {
        return;
    }
    const std::uint64_t per_chunk = std::max<std::uint64_t>(1, transfer / row_size);
    std::vector<char> chunk(std::min(per_chunk, rows) * row_size);
    for (std::uint64_t done = 0; done < rows;) {
        const std::uint64_t count = std::min(per_chunk, rows - done);
        std::ranges::fill(chunk, ' ');
        for (std::size_t c = 0; c < table.columns.size(); ++c) {
            const column_model& model = table.columns[c];
            const std::uint64_t number = c + 1;
            if (model.values.kind == values_kind::strings) {
                for (std::uint64_t r = 0; r < count; ++r) {
                    const std::string& text = model.values.strings[done + r];
                    std::memcpy(chunk.data() + (r * row_size) + model.info.offset, text.data(), text.size());
                }
                continue;
            }
            // Every format of an ASCII table has its field.
            if (!model.format.field) {
                continue;
            }
            const display_format& field = *model.format.field;
            // Stored values are written as they are.
            const scaling applied = table.options.scaling == scaling_policy::raw ? scaling{} : model.info.scaling;
            with_type(model.values.type, [&]<typename T>(std::type_identity<T>) {
                for (std::uint64_t r = 0; r < count; ++r) {
                    T value{};
                    std::memcpy(&value, model.values.bytes.subspan((done + r) * sizeof(T)).data(), sizeof(T));
                    write_ascii_number(model, field, applied, value, chunk.data() + (r * row_size) + model.info.offset,
                                       hdu, number);
                }
            });
        }
        const std::span<const char> written(chunk.data(), count * row_size);
        sink.write(std::as_bytes(written));
        advance(count * row_size);
        done += count;
    }
}

// The conversions of the values of a column into its stored type, made once for each write of the column.
struct element_writer
{
    const column_model* model = nullptr;
    std::uint64_t hdu = 0;
    // The n of TFORMn.
    std::uint64_t number = 0;
    // Numbers, and the parts of complex numbers when their real parts take no TZEROn.
    write_conversion conversion{};
    // The real parts of complex numbers with a TZEROn (standard §7.3.2); conversion takes the imaginary ones.
    std::optional<write_conversion> reals{};
};

element_writer make_element_writer(const column_model& model, std::uint64_t hdu, std::uint64_t number,
                                   const table_write_options& options)
{
    element_writer writer{.model = &model, .hdu = hdu, .number = number};
    const column_info& info = model.info;
    const bitpix stored = stored_bitpix(model.format.type);
    // Stored values convert into the stored type alone.
    const scaling applied = options.scaling == scaling_policy::raw ? scaling{} : info.scaling;
    const overflow_policy overflow = options.overflow;
    switch (model.values.kind) {
    case values_kind::numbers:
        writer.conversion = make_write_conversion(model.values.type, stored, applied, info.null, overflow, hdu);
        break;
    case values_kind::complex_float32:
    case values_kind::complex_float64: {
        const numeric_type part =
            model.values.kind == values_kind::complex_float32 ? numeric_type::float32 : numeric_type::float64;
        writer.conversion = make_write_conversion(part, stored, {.scale = applied.scale}, std::nullopt, overflow, hdu);
        if (applied.zero != 0.0) {
            writer.reals = make_write_conversion(part, stored, applied, std::nullopt, overflow, hdu);
        }
        break;
    }
    case values_kind::strings:
    case values_kind::logicals:
    case values_kind::bits:
        break;
    }
    return writer;
}

// The bytes of count elements of the values of a column, from the element first of the values on, written into out,
// which holds their bytes.
void write_elements(const element_writer& writer, std::uint64_t first, std::uint64_t count, std::byte* out)
{
    const values_view& values = writer.model->values;
    switch (values.kind) {
    case values_kind::numbers:
        try {
            (void)convert_write(writer.conversion, values.bytes.data() + (first * size_of(values.type)), out, count);
        } catch (invalid_data_error& failure) {
            throw invalid_data_error(failure.code(), failure.what(), {.hdu = writer.hdu, .column = writer.number});
        }
        return;
    case values_kind::complex_float32:
    case values_kind::complex_float64: {
        const std::size_t part_size = values.kind == values_kind::complex_float32 ? 4 : 8;
        const std::size_t stored_part = stored_size(stored_bitpix(writer.model->format.type));
        const std::byte* in = values.bytes.data() + (first * 2 * part_size);
        if (!writer.reals) {
            (void)convert_write(writer.conversion, in, out, 2 * count);
            return;
        }
        for (std::uint64_t i = 0; i < count; ++i) {
            (void)convert_write(*writer.reals, in + (i * 2 * part_size), out + (i * 2 * stored_part), 1);
            (void)convert_write(writer.conversion, in + (i * 2 * part_size) + part_size,
                                out + (i * 2 * stored_part) + stored_part, 1);
        }
        return;
    }
    case values_kind::logicals: {
        const auto* flags = reinterpret_cast<const std::uint8_t*>(values.bytes.data());
        for (std::uint64_t i = 0; i < count; ++i) {
            const std::uint64_t at = first + i;
            const bool missing = !values.undefined.empty() && values.undefined[at] != 0;
            out[i] = missing ? std::byte{0} : static_cast<std::byte>(flags[at] != 0 ? 'T' : 'F');
        }
        return;
    }
    case values_kind::bits: {
        // count bits from the bit first, packed into bytes from the most significant bit.
        const auto* bits = reinterpret_cast<const std::uint8_t*>(values.bytes.data());
        const std::uint64_t bytes = (count / 8) + (count % 8 == 0 ? 0 : 1);
        std::memset(out, 0, bytes);
        for (std::uint64_t i = 0; i < count; ++i) {
            if (bits[first + i] != 0) {
                out[i / 8] |= static_cast<std::byte>(0x80U >> (i % 8));
            }
        }
        return;
    }
    case values_kind::strings:
        break;
    }
}

// The bytes of the field of row of a column whose fields are not converted a chunk at a time into field: descriptors,
// strings and bits.
void write_field(const element_writer& writer, std::uint64_t row, std::byte* field)
{
    const column_model& model = *writer.model;
    const column_format& format = model.format;
    if (format.storage != column_storage::fixed) {
        const std::uint64_t count = model.counts[row];
        const std::uint64_t offset = model.heap_offsets[row];
        if (format.storage == column_storage::variable_p) {
            store_big_endian<std::uint32_t>(std::span<std::byte, 4>(field, 4), static_cast<std::uint32_t>(count));
            store_big_endian<std::uint32_t>(std::span<std::byte, 4>(field + 4, 4), static_cast<std::uint32_t>(offset));
        } else {
            store_big_endian<std::uint64_t>(std::span<std::byte, 8>(field, 8), count);
            store_big_endian<std::uint64_t>(std::span<std::byte, 8>(field + 8, 8), offset);
        }
        return;
    }
    if (model.values.kind == values_kind::strings) {
        // Each string padded with spaces to its width, as CFITSIO pads them.
        std::memset(field, ' ', format.field_size());
        for (std::uint64_t k = 0; k < model.strings; ++k) {
            const std::string& text = model.values.strings[(row * model.strings) + k];
            std::memcpy(field + (k * model.string_width), text.data(), text.size());
        }
        return;
    }
    write_elements(writer, row * model.elements, model.elements, field);
}

// True for the columns whose fields hold whole elements one after the other, which a chunk of rows converts at once.
bool converts_by_chunk(const column_model& model) noexcept
{
    return model.format.storage == column_storage::fixed && model.values.kind != values_kind::strings &&
           model.values.kind != values_kind::bits;
}

// The heap, each array of each column in the order of the rows, gathered into chunks of at most transfer bytes; an
// array larger than a chunk in pieces of whole elements, bits in whole bytes.
void write_heap(output_sink& sink, const table_model& table, const std::vector<element_writer>& writers,
                const std::function<void(std::uint64_t)>& advance, std::uint64_t hdu, std::size_t transfer)
{
    if (table.heap_size == 0) {
        return;
    }
    std::vector<std::byte> chunk(std::min<std::uint64_t>(transfer, table.heap_size));
    std::size_t used = 0;
    const auto flush = [&]() {
        if (used > 0) {
            sink.write(std::span<const std::byte>(chunk).first(used));
            advance(used);
            used = 0;
        }
    };
    for (std::size_t c = 0; c < table.columns.size(); ++c) {
        const column_model& model = table.columns[c];
        const column_format& format = model.format;
        if (format.storage == column_storage::fixed) {
            continue;
        }
        const std::uint64_t element = column_format{.type = format.type}.field_size();
        for (std::uint64_t row = 0; row < model.counts.size(); ++row) {
            const std::uint64_t count = model.counts[row];
            if (count == 0) {
                continue;
            }
            if (model.values.kind == values_kind::strings) {
                const std::string& text = model.values.strings[row];
                const std::size_t size = std::max<std::size_t>(text.size(), 1);
                if (size > chunk.size() - used) {
                    flush();
                }
                if (size > chunk.size()) {
                    sink.write(std::as_bytes(std::span<const char>(text.data(), text.size())));
                    advance(text.size());
                } else {
                    // The NUL of an empty string, or its characters.
                    chunk[used] = std::byte{0};
                    std::memcpy(chunk.data() + used, text.data(), text.size());
                    used += size;
                }
                continue;
            }
            const std::uint64_t first = model.values.offsets[row];
            const std::uint64_t bytes = array_bytes(format.type, count, hdu);
            if (bytes <= chunk.size() - used) {
                write_elements(writers[c], first, count, chunk.data() + used);
                used += bytes;
                continue;
            }
            flush();
            const std::uint64_t per_piece = format.type == column_type::bit
                                                ? std::max<std::uint64_t>(8, (chunk.size() / 8) * 8 * 8)
                                                : std::max<std::uint64_t>(1, chunk.size() / element);
            for (std::uint64_t done = 0; done < count;) {
                const std::uint64_t elements = std::min(per_piece, count - done);
                const std::uint64_t piece = array_bytes(format.type, elements, hdu);
                if (piece > chunk.size()) {
                    chunk.resize(piece);
                }
                write_elements(writers[c], first + done, elements, chunk.data());
                used = piece;
                flush();
                done += elements;
            }
        }
    }
    flush();
}

void write_binary_rows(output_sink& sink, const table_model& table, const std::function<void(std::uint64_t)>& advance,
                       std::uint64_t hdu, std::size_t transfer)
{
    std::vector<element_writer> writers;
    writers.reserve(table.columns.size());
    for (std::size_t c = 0; c < table.columns.size(); ++c) {
        writers.push_back(make_element_writer(table.columns[c], hdu, c + 1, table.options));
    }
    const std::uint64_t row_size = table.info.row_size;
    const std::uint64_t rows = table.info.rows;
    if (row_size > 0 && rows > 0) {
        const std::uint64_t per_chunk = std::max<std::uint64_t>(1, transfer / row_size);
        std::vector<std::byte> chunk(std::min(per_chunk, rows) * row_size);
        std::vector<std::byte> fields;
        for (std::uint64_t done = 0; done < rows;) {
            const std::uint64_t count = std::min(per_chunk, rows - done);
            for (std::size_t c = 0; c < table.columns.size(); ++c) {
                const column_model& model = table.columns[c];
                const std::uint64_t field_size = model.format.field_size();
                const std::uint64_t offset = model.info.offset;
                if (field_size == 0) {
                    continue;
                }
                if (!converts_by_chunk(model)) {
                    for (std::uint64_t r = 0; r < count; ++r) {
                        write_field(writers[c], done + r, chunk.data() + (r * row_size) + offset);
                    }
                    continue;
                }
                // The fields of the chunk converted at once, in place when the column is the whole row, and placed in
                // their rows otherwise.
                if (field_size == row_size) {
                    write_elements(writers[c], done * model.elements, count * model.elements, chunk.data());
                    continue;
                }
                fields.resize(count * field_size);
                write_elements(writers[c], done * model.elements, count * model.elements, fields.data());
                const std::size_t size = field_size;
                for (std::uint64_t r = 0; r < count; ++r) {
                    std::memcpy(chunk.data() + (r * row_size) + offset, fields.data() + (r * size), size);
                }
            }
            sink.write(std::span<const std::byte>(chunk).first(count * row_size));
            advance(count * row_size);
            done += count;
        }
    }
    write_heap(sink, table, writers, advance, hdu, transfer);
}

} // namespace

void write_table_data(output_sink& sink, const table_model& table, const std::function<void(std::uint64_t)>& advance,
                      std::uint64_t hdu, std::size_t transfer)
{
    transfer = std::max<std::size_t>(transfer, 1);
    const bool ascii = table.info.kind == hdu_kind::table;
    if (ascii) {
        write_ascii_rows(sink, table, advance, hdu, transfer);
    } else {
        write_binary_rows(sink, table, advance, hdu, transfer);
    }
    // The fill of the last block: spaces for an ASCII table (standard §7.2.3), zeros otherwise (§7.3.3).
    const std::uint64_t size = table.info.heap_offset + table.heap_size;
    const std::uint64_t fill = (block_size - (size % block_size)) % block_size;
    if (fill > 0) {
        std::array<std::byte, block_size> padding{};
        if (ascii) {
            padding.fill(static_cast<std::byte>(' '));
        }
        sink.write(std::span<const std::byte>(padding).first(fill));
        advance(fill);
    }
}

} // namespace openfits::detail
