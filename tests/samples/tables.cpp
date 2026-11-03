// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The tables and random groups of the samples, cell by cell against what astropy reads of them (the .cells file next to
// each sample, which tests/data/scripts/dump_cells.py writes). astropy and the reader differ on purpose in three cells:
// astropy reads the strings of the rAw convention whole, ignores the implied decimal point of an ASCII field, and gives
// an ASCII field equal to TNULLn as 0.

#include <openfits/error.h>
#include <openfits/reader.h>
#include <openfits/table.h>

#include "samples/sample_catalog.h"

#include <gtest/gtest.h>

#include <cmath>
#include <complex>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using openfits::column_type;
using openfits::reader;
using openfits::test::sample;
using openfits::test::sample_by_id;
using openfits::test::sample_path;

std::vector<std::string> split(const std::string& line)
{
    std::vector<std::string> tokens;
    if (line.empty()) {
        return tokens;
    }
    std::size_t start = 0;
    while (true) {
        const std::size_t tab = line.find('\t', start);
        tokens.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos) {
            return tokens;
        }
        start = tab + 1;
    }
}

// The lines of the dump of a sample.
std::vector<std::string> dump_of(const sample& entry)
{
    std::string path = sample_path(entry);
    path.replace(path.rfind(".fit"), path.size(), ".cells");
    std::ifstream file(path, std::ios::binary);
    std::vector<std::string> lines;
    for (std::string line; std::getline(file, line);) {
        lines.push_back(line);
    }
    return lines;
}

double number(const std::string& token)
{
    if (token == "nan") {
        return std::nan("");
    }
    if (token == "inf") {
        return std::numeric_limits<double>::infinity();
    }
    if (token == "-inf") {
        return -std::numeric_limits<double>::infinity();
    }
    std::istringstream text(token);
    text.imbue(std::locale::classic());
    double value = 0;
    text >> value;
    return value;
}

bool same(double ours, double theirs)
{
    return (std::isnan(ours) && std::isnan(theirs)) || ours == theirs;
}

// Compares a column of numbers, logicals or bits with the lines of its rows.
void compare_numbers(const reader& input, std::size_t unit, std::size_t column, const std::vector<std::string>& rows)
{
    const openfits::column_info info = input.table(unit).columns[column];
    if (!info.format) {
        ADD_FAILURE() << "column " << column << " of unit " << unit << " has no format";
        return;
    }
    const openfits::column_format& format = *info.format;
    if (format.storage != openfits::column_storage::fixed) {
        const openfits::column_arrays<double> arrays = input.read_arrays<double>(unit, column);
        ASSERT_EQ(arrays.size(), rows.size());
        for (std::size_t row = 0; row < rows.size(); ++row) {
            const std::vector<std::string> tokens = split(rows[row]);
            ASSERT_EQ(arrays[row].size(), tokens.size()) << row;
            for (std::size_t i = 0; i < tokens.size(); ++i) {
                EXPECT_TRUE(same(arrays[row][i], number(tokens[i]))) << row << ", " << i;
            }
        }
        return;
    }
    const std::uint64_t per_row = format.field ? 1 : format.repeat;
    if (format.type == column_type::complex_float32 || format.type == column_type::complex_float64) {
        const std::vector<std::complex<double>> values = input.read_column<std::complex<double>>(unit, column);
        for (std::size_t row = 0; row < rows.size(); ++row) {
            const std::vector<std::string> tokens = split(rows[row]);
            ASSERT_EQ(tokens.size(), 2 * per_row);
            for (std::size_t i = 0; i < per_row; ++i) {
                EXPECT_TRUE(same(values[(row * per_row) + i].real(), number(tokens[2 * i]))) << row;
                EXPECT_TRUE(same(values[(row * per_row) + i].imag(), number(tokens[(2 * i) + 1]))) << row;
            }
        }
        return;
    }
    // Integers exactly, as the 64-bit integers that hold them, and every other value as a double.
    const openfits::numeric_type type = info.value_type();
    const bool unsigned_64 = type == openfits::numeric_type::uint64;
    const bool integers = type != openfits::numeric_type::float32 && type != openfits::numeric_type::float64;
    std::vector<std::uint8_t> undefined(rows.size() * per_row);
    const openfits::column_read_options options{.blank = openfits::blank_policy::mask,
                                                .mask = std::span<std::uint8_t>(undefined)};
    const std::vector<double> reals = input.read_column<double>(unit, column, options);
    for (std::size_t row = 0; row < rows.size(); ++row) {
        const std::vector<std::string> tokens = split(rows[row]);
        ASSERT_EQ(tokens.size(), per_row) << row;
        for (std::size_t i = 0; i < per_row; ++i) {
            const std::size_t at = (row * per_row) + i;
            const std::string& token = tokens[i];
            if (token == "T" || token == "F") {
                EXPECT_EQ(reals[at], token == "T" ? 1.0 : 0.0) << row << ", " << i;
            } else if (undefined[at] == 0) {
                EXPECT_TRUE(same(reals[at], number(token))) << row << ", " << i << ": " << token;
            }
        }
    }
    if (integers && !format.field) {
        if (unsigned_64) {
            const std::vector<std::uint64_t> values = input.read_column<std::uint64_t>(unit, column);
            for (std::size_t row = 0; row < rows.size(); ++row) {
                EXPECT_EQ(std::to_string(values[row]), split(rows[row]).front()) << row;
            }
        } else if (type != openfits::numeric_type::uint8 || format.type == column_type::uint8) {
            const std::vector<std::int64_t> values = input.read_column<std::int64_t>(unit, column);
            for (std::size_t row = 0; row < rows.size(); ++row) {
                const std::vector<std::string> tokens = split(rows[row]);
                for (std::size_t i = 0; i < per_row; ++i) {
                    EXPECT_EQ(static_cast<double>(values[(row * per_row) + i]), number(tokens[i])) << row;
                }
            }
        }
    }
}

// Compares every column of every table of a sample with its dump, but the cells named, which differ on purpose.
void compare_tables(const sample& entry)
{
    const reader input(sample_path(entry));
    const std::vector<std::string> lines = dump_of(entry);
    ASSERT_FALSE(lines.empty());
    std::size_t columns = 0;
    for (std::size_t i = 0; i < lines.size();) {
        const std::vector<std::string> head = split(lines[i]);
        ASSERT_EQ(head.size(), 5U) << lines[i];
        ASSERT_EQ(head[0], "column");
        const std::size_t unit = std::stoul(head[1]);
        const std::size_t column = std::stoul(head[2]);
        const std::size_t rows = std::stoul(head[4]);
        const std::vector<std::string> cells(lines.begin() + static_cast<std::ptrdiff_t>(i + 1),
                                             lines.begin() + static_cast<std::ptrdiff_t>(i + 1 + rows));
        i += 1 + rows;
        ++columns;
        SCOPED_TRACE(head[3]);
        const openfits::table_info table = input.table(unit);
        ASSERT_LT(column, table.columns.size());
        EXPECT_EQ(table.columns[column].name, head[3]);
        const std::optional<openfits::column_format>& maybe_format = table.columns[column].format;
        if (!maybe_format) {
            ADD_FAILURE() << "column " << column << " of unit " << unit << " has no format";
            continue;
        }
        const openfits::column_format& format = *maybe_format;
        if (format.type == column_type::character) {
            const std::vector<std::string> strings = input.read_strings(unit, column, {.trim_strings = true});
            if (format.substring_width) {
                // astropy reads the strings of the rAw convention whole: the substrings joined.
                const std::uint64_t per_row = format.repeat / *format.substring_width;
                for (std::size_t row = 0; row < rows; ++row) {
                    std::string joined;
                    for (std::uint64_t k = 0; k < per_row; ++k) {
                        joined += strings[(row * per_row) + k];
                    }
                    EXPECT_EQ(joined, cells[row]) << row;
                }
            } else {
                EXPECT_EQ(strings, cells);
            }
            continue;
        }
        if (entry.id == "D3" && head[3] == "FLUX") {
            // astropy ignores the implied decimal point of the field 12345 (standard §7.2.5).
            const std::vector<double> values = input.read_column<double>(unit, column);
            EXPECT_EQ(values[2], 12.345);
            EXPECT_EQ(cells[2], "12345.0");
            std::vector<std::string> others = cells;
            others[2] = "12.345";
            compare_numbers(input, unit, column, others);
            continue;
        }
        compare_numbers(input, unit, column, cells);
    }
    EXPECT_EQ(columns, input.table(entry.units.size() - 1).columns.size());
}

TEST(samples_tables, d2_reads_as_astropy_reads_it)
{
    const sample& entry = sample_by_id("D2");
    compare_tables(entry);
    const reader input(sample_path(entry));
    const openfits::table_info table = input.table(1);
    EXPECT_EQ(table.heap_offset, 141U * 5 + 104);
    EXPECT_EQ(table.heap_size, 105U);
    EXPECT_EQ(input.hdu(1).name(), "CATALOG");
    // The rAw convention, read as TDIMn = '(4,3)' would say.
    EXPECT_EQ(table.columns[21].dimensions, (std::vector<std::uint64_t>{4, 3}));
    EXPECT_EQ(input.read_strings(1, "WORDS", {.rows = openfits::row_range{.first = 0, .count = 1}}),
              (std::vector<std::string>{"abcd", "efgh", "ijkl"}));
}

TEST(samples_tables, b16_reads_as_the_table_it_holds)
{
    // The cells of the table that fpack compressed, as astropy reads that table before its compression.
    const sample& entry = sample_by_id("B16");
    compare_tables(entry);
    const reader input(sample_path(entry));
    const openfits::table_info table = input.table(1);
    EXPECT_EQ(table.kind, openfits::hdu_kind::bintable);
    EXPECT_EQ(table.rows, 100U);
    EXPECT_EQ(input.hdu(1).kind(), openfits::hdu_kind::compressed_table);
    EXPECT_FALSE(table.header.index_of("ZTABLE").has_value());
    EXPECT_EQ(table.header.value<std::int64_t>("NAXIS2"), 100);
}

TEST(samples_tables, d3_reads_as_astropy_reads_it)
{
    const sample& entry = sample_by_id("D3");
    compare_tables(entry);
    const reader input(sample_path(entry));
    // A blank field is 0 (standard §7.2.5), and the TNULLn string undefined, which astropy gives as 0.
    std::vector<std::uint8_t> mask(4);
    std::vector<double> counts(4);
    (void)input.read_column(1, "COUNT", std::span<double>(counts),
                            {.blank = openfits::blank_policy::mask, .mask = std::span<std::uint8_t>(mask)});
    EXPECT_EQ(counts[2], 0.0);
    EXPECT_EQ(mask, (std::vector<std::uint8_t>{0, 0, 0, 1}));
}

TEST(samples_tables, d4_reads_as_astropy_reads_it)
{
    const sample& entry = sample_by_id("D4");
    const reader input(sample_path(entry));
    const std::vector<std::string> lines = dump_of(entry);
    const std::vector<std::string> head = split(lines.at(0));
    ASSERT_EQ(head.at(0), "groups");
    const std::size_t count = std::stoul(head.at(2));
    const std::size_t parameters = std::stoul(head.at(3));
    const openfits::groups_info groups = input.groups(0);
    ASSERT_EQ(groups.count, count);
    ASSERT_EQ(groups.parameters.size(), parameters);
    for (std::size_t p = 0; p < parameters; ++p) {
        const std::vector<double> values = input.read_parameter<double>(0, p);
        for (std::size_t g = 0; g < count; ++g) {
            EXPECT_EQ(values[g], number(split(lines[1 + (2 * g)])[p])) << p << ", " << g;
        }
    }
    for (std::size_t g = 0; g < count; ++g) {
        const std::vector<double> array = input.read_group<double>(0, g);
        const std::vector<std::string> tokens = split(lines[2 + (2 * g)]);
        ASSERT_EQ(array.size(), tokens.size());
        for (std::size_t e = 0; e < array.size(); ++e) {
            EXPECT_EQ(array[e], number(tokens[e])) << g << ", " << e;
        }
    }
    // The parameters of one name summed (standard §6.1.2), as astropy sums them.
    for (std::size_t i = 1 + (2 * count); i < lines.size(); ++i) {
        const std::vector<std::string> tokens = split(lines[i]);
        ASSERT_EQ(tokens.at(0), "parameter");
        const std::vector<double> values = input.read_parameter<double>(0, tokens.at(1));
        for (std::size_t g = 0; g < count; ++g) {
            EXPECT_EQ(values[g], number(tokens.at(2 + g))) << tokens.at(1);
        }
    }
}

} // namespace
