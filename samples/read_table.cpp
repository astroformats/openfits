// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Reading a table. This program opens a FITS file, lists the columns of a table, ASCII or binary, with their formats,
// units and the types that hold their values, and reads some of them: the first rows of each column as their display
// formats show them, a numeric column as double over a range of rows in one pass with the others, its undefined
// values marked in a mask, and the arrays of a column of variable-length arrays, when the table has one.
//
// Usage: read_table <path> [<unit>]

#include <openfits/openfits.h>

#include "arguments.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string_view type_name(openfits::numeric_type type)
{
    constexpr std::array<std::string_view, 10> names{"int8",   "uint8", "int16",  "uint16",  "int32",
                                                     "uint32", "int64", "uint64", "float32", "float64"};
    return names.at(static_cast<std::size_t>(type));
}

// The format of a column, which every column of a table that the reader describes has.
const openfits::column_format& format_of(const openfits::column_info& column)
{
    if (!column.format) {
        throw std::logic_error("a column without a format");
    }
    return *column.format;
}

bool is_numeric(const openfits::column_info& column)
{
    const openfits::column_format& format = format_of(column);
    return format.type != openfits::column_type::character && format.type != openfits::column_type::complex_float32 &&
           format.type != openfits::column_type::complex_float64;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const std::vector<std::string> positional = sample::arguments(argc, argv);
        if (positional.empty() || positional.size() > 2) {
            std::cerr << "usage: read_table <path> [<unit>]\n";
            return 2;
        }
        const openfits::reader file(positional[0]);
        const std::optional<std::size_t> found =
            positional.size() > 1 ? std::optional<std::size_t>(std::stoull(positional[1])) : file.first_table();
        if (!found) {
            std::cerr << positional[0] << " holds no table\n";
            return 1;
        }
        const std::size_t unit = *found;

        // The description of the table: its kind, its rows and its columns.
        const openfits::table_info table = file.table(unit);
        std::cout << "unit " << unit << ": " << (table.kind == openfits::hdu_kind::table ? "ASCII" : "binary")
                  << " table of " << table.rows << " rows of " << table.row_size << " bytes, " << table.columns.size()
                  << " columns\n";
        for (std::size_t c = 0; c < table.columns.size(); ++c) {
            const openfits::column_info& column = table.columns[c];
            std::cout << "  " << c << " " << (column.name.empty() ? "(no name)" : column.name) << " "
                      << format_of(column).to_string();
            if (!column.unit.empty()) {
                std::cout << " [" << column.unit << "]";
            }
            if (is_numeric(column)) {
                std::cout << " as " << type_name(column.value_type());
            }
            if (format_of(column).storage != openfits::column_storage::fixed) {
                std::cout << ", variable-length arrays";
            }
            std::cout << "\n";
        }

        // The first rows of each column, as their display formats show them.
        const openfits::row_range first_rows{.first = 0, .count = std::min<std::uint64_t>(table.rows, 3)};
        for (std::size_t c = 0; c < table.columns.size(); ++c) {
            const openfits::column_info& column = table.columns[c];
            const bool arrays_of_numbers = format_of(column).storage != openfits::column_storage::fixed &&
                                           format_of(column).type != openfits::column_type::character;
            if (arrays_of_numbers) {
                continue;
            }
            std::cout << "  " << (column.name.empty() ? std::to_string(c) : column.name) << ":";
            for (const std::string& text : file.read_strings(unit, c, {.rows = first_rows, .trim_strings = true})) {
                std::cout << " '" << text << "'";
            }
            std::cout << "\n";
        }

        // The first column of numbers of one element as double, through read_columns(), which reads several columns
        // in one pass: its undefined values, TNULLn or NaN, marked in a mask.
        const auto numeric = std::ranges::find_if(table.columns, [](const openfits::column_info& column) {
            const openfits::column_type type = format_of(column).type;
            return is_numeric(column) && type != openfits::column_type::logical && type != openfits::column_type::bit &&
                   format_of(column).storage == openfits::column_storage::fixed &&
                   (format_of(column).field || format_of(column).repeat == 1);
        });
        if (numeric != table.columns.end()) {
            const auto index = static_cast<std::size_t>(numeric - table.columns.begin());
            const std::uint64_t rows = table.rows;
            std::vector<double> values(rows);
            std::vector<std::uint8_t> undefined(rows);
            std::vector<openfits::column_destination> destinations{
                openfits::column_destination::of(index, std::span<double>(values), std::span<std::uint8_t>(undefined))};
            (void)file.read_columns(unit, destinations, {.blank = openfits::blank_policy::mask});
            const auto marked = std::ranges::count(undefined, std::uint8_t{1});
            std::cout << "column " << index << " as double: " << rows << " values, " << marked << " undefined";
            if (rows > 0) {
                const auto [low, high] = std::ranges::minmax(values);
                std::cout << ", from " << low << " to " << high;
            }
            std::cout << "\n";
        }

        // The arrays of the first column of variable-length arrays of numbers.
        const auto variable = std::ranges::find_if(table.columns, [](const openfits::column_info& column) {
            return format_of(column).storage != openfits::column_storage::fixed && is_numeric(column);
        });
        if (variable != table.columns.end()) {
            const auto index = static_cast<std::size_t>(variable - table.columns.begin());
            const openfits::column_arrays<double> arrays = file.read_arrays<double>(unit, index);
            std::cout << "column " << index << ": " << arrays.size() << " arrays, " << arrays.values.size()
                      << " elements in all";
            if (arrays.size() > 0) {
                std::cout << ", the first of " << arrays[0].size();
            }
            std::cout << "\n";
        }
        return 0;
    } catch (const std::exception& failure) {
        (void)std::fputs("read_table: ", stderr);
        (void)std::fputs(failure.what(), stderr);
        (void)std::fputs("\n", stderr);
        return 1;
    }
}
