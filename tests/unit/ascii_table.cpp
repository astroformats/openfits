// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/table.h>

#include "support/fixture_builder.h"
#include "support/table_fixtures.h"
#include "support/throws.h"
#include "table/ascii_field.h"

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using openfits::blank_policy;
using openfits::card_value;
using openfits::errc;
using openfits::invalid_data_error;
using openfits::reader;
using openfits::detail::read_integer_field;
using openfits::detail::read_real_field;
using openfits::test::ascii_field;
using openfits::test::ascii_table_records;
using openfits::test::byte_writer;
using openfits::test::card_record;
using openfits::test::fixture;
using openfits::test::throws;
using openfits::test::with;

double real_of(std::string_view field, std::uint32_t decimals = 0)
{
    const openfits::detail::field_number number = read_real_field(field, decimals);
    EXPECT_FALSE(number.problem) << field;
    return number.value.kind() == openfits::value_kind::real ? number.value.get<double>() : -12345.0;
}

TEST(ascii_field, reads_integers)
{
    // Standard §7.2.5: an optional sign and digits, spaces before and after; a blank field is 0.
    EXPECT_EQ(read_integer_field("   42").value, card_value(42));
    EXPECT_EQ(read_integer_field("-17   ").value, card_value(-17));
    EXPECT_EQ(read_integer_field("+5").value, card_value(5));
    EXPECT_EQ(read_integer_field("     ").value, card_value(0));
    EXPECT_EQ(read_integer_field("").value, card_value(0));
    EXPECT_EQ(read_integer_field("-9223372036854775808").value, card_value(std::numeric_limits<std::int64_t>::min()));
    EXPECT_EQ(read_integer_field("18446744073709551615").value, card_value(std::numeric_limits<std::uint64_t>::max()));
    // Spaces inside are ignored, as Fortran ignores them in its input.
    EXPECT_EQ(read_integer_field(" 1 2 3").value, card_value(123));
    EXPECT_EQ(read_integer_field("- 4").value, card_value(-4));
    for (const std::string_view bad : {"1.0", "1e3", "abc", "+", "-", "1-", "++1", "0x10"}) {
        EXPECT_EQ(read_integer_field(bad).problem, errc::invalid_field) << bad;
    }
    EXPECT_EQ(read_integer_field("18446744073709551616").problem, errc::value_out_of_range);
}

TEST(ascii_field, reads_reals)
{
    EXPECT_EQ(real_of("  12.345"), 12.345);
    EXPECT_EQ(real_of("-0.5   "), -0.5);
    EXPECT_EQ(real_of(".25"), 0.25);
    EXPECT_EQ(real_of("3."), 3.0);
    EXPECT_EQ(real_of("1.5E3"), 1500.0);
    EXPECT_EQ(real_of("1.5D-2"), 0.015);
    EXPECT_EQ(real_of("-2.5e+1"), -25.0);
    // An exponent introduced by its sign alone (standard §7.2.5, 3a).
    EXPECT_EQ(real_of("1.5+3"), 1500.0);
    EXPECT_EQ(real_of("0.12345678901234567-300"), 0.12345678901234567e-300);
    EXPECT_EQ(real_of("     "), 0.0);
    EXPECT_EQ(real_of("1 2.5"), 12.5);
    // The decimal number rounded once to the nearest double.
    EXPECT_EQ(real_of("0.1"), 0.1);
    EXPECT_EQ(real_of("1.2345E+05"), 123450.0);
    for (const std::string_view bad : {"1.2.3", "1,5", "E3", "1E", "1E+", "1+", ".", "+", "abc", "1.5F3", "1.5E3.0"}) {
        EXPECT_EQ(read_real_field(bad, 0).problem, errc::invalid_field) << bad;
    }
    EXPECT_EQ(read_real_field("1E400", 0).problem, errc::value_out_of_range);
    EXPECT_EQ(real_of("1E-400"), 0.0);
}

TEST(ascii_field, reads_the_implied_decimal_point)
{
    // Standard §7.2.5, 2: without a decimal point, it precedes the rightmost d digits, leading zeros as needed.
    EXPECT_EQ(real_of("12345", 3), 12.345);
    EXPECT_EQ(real_of("   -5", 3), -0.005);
    EXPECT_EQ(real_of("12345E2", 3), 1234.5);
    EXPECT_EQ(real_of("12345", 0), 12345.0);
    EXPECT_EQ(real_of("1.5", 3), 1.5);
    EXPECT_EQ(real_of("7", 5), 0.00007);
}

TEST(ascii_field, reads_the_plain_forms_as_the_full_reads_do)
{
    // The plain forms give what the full reads give, and leave every other field to them.
    struct integer_case
    {
        std::string_view field;
        bool plain = true;
    };
    for (const integer_case& entry :
         {integer_case{.field = "   42"}, integer_case{.field = "-17   "}, integer_case{.field = "+5"},
          integer_case{.field = "     "}, integer_case{.field = ""}, integer_case{.field = "999999999999999999"},
          integer_case{.field = "-123456789012345678"}, integer_case{.field = "1234567890123456789", .plain = false},
          integer_case{.field = " 1 2 3", .plain = false}, integer_case{.field = "1.0", .plain = false},
          integer_case{.field = "+", .plain = false}, integer_case{.field = "-", .plain = false},
          integer_case{.field = "abc", .plain = false}, integer_case{.field = "1-", .plain = false}}) {
        const std::optional<std::int64_t> plain = openfits::detail::read_plain_integer_field(entry.field);
        if (plain) {
            EXPECT_EQ(read_integer_field(entry.field).value, card_value(*plain)) << entry.field;
        }
        EXPECT_EQ(plain.has_value(), entry.plain) << entry.field;
    }
    struct real_case
    {
        std::string_view field;
        std::uint32_t decimals = 0;
        bool plain = true;
    };
    for (const real_case& entry : {real_case{.field = "  12.345"},
                                   real_case{.field = "-0.5   "},
                                   real_case{.field = ".25"},
                                   real_case{.field = "3."},
                                   real_case{.field = "1.5E3"},
                                   real_case{.field = "1.5D-2"},
                                   real_case{.field = "-2.5e+1"},
                                   real_case{.field = "+0.1"},
                                   real_case{.field = "     "},
                                   real_case{.field = "12345"},
                                   real_case{.field = "0.25039999999999999D+02"},
                                   real_case{.field = "4.9E-324"},
                                   real_case{.field = "1.5", .decimals = 3},
                                   real_case{.field = "12345", .decimals = 3, .plain = false},
                                   real_case{.field = "1.5+3", .decimals = 0, .plain = false},
                                   real_case{.field = "1 2.5", .decimals = 0, .plain = false},
                                   real_case{.field = "1E400", .decimals = 0, .plain = false},
                                   real_case{.field = "1E-400", .decimals = 0, .plain = false},
                                   real_case{.field = "1.2.3", .decimals = 0, .plain = false},
                                   real_case{.field = "E3", .decimals = 0, .plain = false},
                                   real_case{.field = "1E", .decimals = 0, .plain = false},
                                   real_case{.field = "1.5F3", .decimals = 0, .plain = false},
                                   real_case{.field = "-", .decimals = 0, .plain = false},
                                   real_case{.field = ".", .decimals = 0, .plain = false}}) {
        const std::optional<double> plain = openfits::detail::read_plain_real_field(entry.field, entry.decimals);
        if (plain) {
            EXPECT_EQ(real_of(entry.field, entry.decimals), *plain) << entry.field;
        }
#if !((defined(_LIBCPP_VERSION) && _LIBCPP_VERSION < 200000) || (defined(_GLIBCXX_RELEASE) && _GLIBCXX_RELEASE < 12))
        EXPECT_EQ(plain.has_value(), entry.plain) << entry.field;
#endif
    }
    // Fields of 1 to 19 digits, with the point anywhere and exponents of every size, read to the same double.
    std::uint64_t state = 7;
    for (int i = 0; i < 20000; ++i) {
        state = (state * 6364136223846793005U) + 1442695040888963407U;
        std::string digits = std::to_string(state >> 1U).substr(0, 1 + ((state >> 7U) % 19));
        const std::size_t point = (state >> 13U) % (digits.size() + 1);
        std::string field = (state & 1U) != 0 ? "-" : "";
        field += digits.substr(0, point) + "." + digits.substr(point);
        if ((state >> 20U) % 3 == 0) {
            field += ((state >> 23U) % 2 == 0 ? "E-" : "D+") + std::to_string((state >> 25U) % 40);
        }
        const std::optional<double> plain = openfits::detail::read_plain_real_field(field, 0);
        if (plain) {
            const double full = real_of(field);
            ASSERT_TRUE(std::bit_cast<std::uint64_t>(*plain) == std::bit_cast<std::uint64_t>(full))
                << field << ": " << *plain << ", " << full;
        }
    }
}

reader open(const fixture& file)
{
    return reader(std::make_unique<openfits::memory_source>(std::vector<std::byte>(file.bytes())));
}

// An ASCII table of three rows: a name (A8), a count (I5), a flux (F8.3, implied decimals in the second row), a
// magnitude (E10.3) and a distance (D12.4), one space between the fields.
fixture catalogue(const std::vector<std::string>& more = {})
{
    byte_writer rows;
    rows.text("M 31    ", 8).text(" ", 1).text("   42", 5).text(" ", 1).text("  12.500", 8).text(" ", 1);
    rows.text(" 3.440E+00", 10).text(" ", 1).text("   7.7800D+5", 12);
    rows.text("M 33    ", 8).text(" ", 1).text("  -17", 5).text(" ", 1).text("   12345", 8).text(" ", 1);
    rows.text("    5.72-1", 10).text(" ", 1).text("NULL        ", 12);
    rows.text("        ", 8).text(" ", 1).text("     ", 5).text(" ", 1).text("        ", 8).text(" ", 1);
    rows.text("          ", 10).text(" ", 1).text("      2.73D1", 12);
    fixture file;
    file.primary();
    file.header(with(ascii_table_records(
                         47, 3,
                         {ascii_field{.column = 1, .format = "A8"}, ascii_field{.column = 10, .format = "I5"},
                          ascii_field{.column = 16, .format = "F8.3"}, ascii_field{.column = 25, .format = "E10.3"},
                          ascii_field{.column = 36, .format = "D12.4"}}),
                     with({card_record("TTYPE1", "'NAME'"), card_record("TTYPE2", "'COUNT'"),
                           card_record("TNULL5", "'NULL'")},
                          more)))
        .data(rows.data());
    return file;
}

TEST(ascii_table, describes_an_ascii_table)
{
    const reader input = open(catalogue());
    const openfits::table_info table = input.table(1);
    EXPECT_EQ(table.kind, openfits::hdu_kind::table);
    EXPECT_EQ(table.rows, 3U);
    EXPECT_EQ(table.row_size, 47U);
    ASSERT_EQ(table.columns.size(), 5U);
    EXPECT_EQ(table.columns[0].format, openfits::column_format::parse_ascii("A8"));
    EXPECT_EQ(table.columns[1].offset, 9U);
    EXPECT_EQ(
        table.columns[2].format.value_or(openfits::column_format{}).field.value_or(openfits::display_format{}).digits,
        3U);
    EXPECT_EQ(table.columns[4].null_string, "NULL");
    EXPECT_EQ(table.columns[1].value_type(), openfits::numeric_type::int64);
    EXPECT_EQ(table.columns[2].value_type(), openfits::numeric_type::float64);
    EXPECT_EQ(table.heap_size, 0U);
}

TEST(ascii_table, reads_the_numbers_of_its_fields)
{
    const reader input = open(catalogue());
    EXPECT_EQ(input.read_column<std::int32_t>(1, "COUNT"), (std::vector<std::int32_t>{42, -17, 0}));
    EXPECT_EQ(input.read_column<double>(1, 2), (std::vector<double>{12.5, 12.345, 0.0}));
    EXPECT_EQ(input.read_column<double>(1, 3), (std::vector<double>{3.44, 0.572, 0.0}));
    const std::vector<double> distances = input.read_column<double>(1, 4);
    EXPECT_EQ(distances[0], 778000.0);
    EXPECT_TRUE(std::isnan(distances[1]));
    EXPECT_EQ(distances[2], 27.3);
    EXPECT_TRUE(
        throws<invalid_data_error>(errc::undefined_value, [&] { (void)input.read_column<std::int64_t>(1, 4); }));
    // A TNULLn string that is not applied is a field like any other, which NULL is not a number of.
    EXPECT_TRUE(throws<invalid_data_error>(
        errc::invalid_field, [&] { (void)input.read_column<double>(1, 4, {.blank = blank_policy::ignore}); }));
    std::vector<std::uint8_t> mask(3);
    std::vector<float> masked(3);
    (void)input.read_column(1, 4, std::span<float>(masked),
                            {.blank = blank_policy::mask, .mask = std::span<std::uint8_t>(mask)});
    EXPECT_EQ(mask, (std::vector<std::uint8_t>{0, 1, 0}));
    EXPECT_EQ(input.read_column<std::int64_t>(1, 4, {.blank = blank_policy::value, .blank_value = -1}),
              (std::vector<std::int64_t>{778000, -1, 27}));
}

TEST(ascii_table, scales_its_numbers)
{
    const reader input = open(catalogue({card_record("TSCAL2", "2"), card_record("TZERO2", "1000")}));
    EXPECT_EQ(input.read_column<std::int64_t>(1, 1), (std::vector<std::int64_t>{1084, 966, 1000}));
    EXPECT_EQ(input.read_column<std::int64_t>(1, 1, {.scaling = openfits::scaling_policy::raw}),
              (std::vector<std::int64_t>{42, -17, 0}));
    EXPECT_TRUE(
        throws<invalid_data_error>(errc::value_out_of_range, [&] { (void)input.read_column<std::uint8_t>(1, 1); }));
}

TEST(ascii_table, reads_its_fields_as_strings)
{
    const reader input = open(catalogue());
    EXPECT_EQ(input.read_strings(1, 0), (std::vector<std::string>{"M 31    ", "M 33    ", "        "}));
    EXPECT_EQ(input.read_strings(1, 0, {.trim_strings = true}), (std::vector<std::string>{"M 31", "M 33", ""}));
    // The fields of numbers as they are written, and an undefined one empty.
    EXPECT_EQ(input.read_strings(1, 4), (std::vector<std::string>{"   7.7800D+5", "", "      2.73D1"}));
    const reader displayed = open(catalogue({card_record("TDISP2", "'I6.3'")}));
    EXPECT_EQ(displayed.read_strings(1, 1), (std::vector<std::string>{"   042", "  -017", "   000"}));
}

TEST(ascii_table, reads_a_character_field_as_a_number_by_parsing_it)
{
    byte_writer rows;
    rows.text(" 2.5", 4).text("-1  ", 4);
    fixture file;
    file.primary();
    file.header(ascii_table_records(4, 2, {ascii_field{.column = 1, .format = "A4"}})).data(rows.data());
    EXPECT_EQ(open(file).read_column<double>(1, 0), (std::vector<double>{2.5, -1.0}));
}

TEST(ascii_table, refuses_a_field_that_is_no_number_of_its_format)
{
    byte_writer rows;
    rows.text("  4.5", 5);
    fixture file;
    file.primary();
    file.header(ascii_table_records(5, 1, {ascii_field{.column = 1, .format = "I5"}})).data(rows.data());
    EXPECT_TRUE(throws<invalid_data_error>(errc::invalid_field, [&] { (void)open(file).read_column<double>(1, 0); }));
}

} // namespace
