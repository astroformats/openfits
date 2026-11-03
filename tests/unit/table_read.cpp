// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/table.h>

#include "io/thread_safe_source.h"
#include "support/counting_source.h"
#include "support/fixture_builder.h"
#include "support/table_fixtures.h"
#include "support/throws.h"
#include "table/column_read.h"

#include <gtest/gtest.h>

#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using openfits::blank_policy;
using openfits::column_destination;
using openfits::column_format;
using openfits::column_info;
using openfits::column_read_options;
using openfits::column_storage;
using openfits::column_type;
using openfits::errc;
using openfits::hdu_kind;
using openfits::invalid_data_error;
using openfits::numeric_type;
using openfits::overflow_policy;
using openfits::reader;
using openfits::row_range;
using openfits::scaling_policy;
using openfits::table_info;
using openfits::usage_error;
using openfits::test::bintable_records;
using openfits::test::byte_writer;
using openfits::test::card_record;
using openfits::test::counting_source;
using openfits::test::fixture;
using openfits::test::throws;
using openfits::test::with;

reader open(const fixture& file)
{
    return reader(std::make_unique<openfits::memory_source>(std::vector<std::byte>(file.bytes())));
}

// A file of an empty primary HDU and a binary table of the records and the data given.
fixture bintable(const std::vector<std::string>& records, const byte_writer& data)
{
    fixture file;
    file.primary();
    file.header(records).data(data.data());
    return file;
}

// A table of one column of 16-bit integers, -3 to 3 in seven rows, with the records given after the mandatory ones.
fixture int16_table(const std::vector<std::string>& more = {})
{
    byte_writer rows;
    rows.values<std::int16_t>({-3, -2, -1, 0, 1, 2, 3});
    return bintable(with(bintable_records(2, 7, {"I"}), more), rows);
}

TEST(table_read, describes_a_binary_table)
{
    byte_writer rows;
    rows.value<std::int32_t>(1).value<double>(2.5).text("ab", 3).value<std::int32_t>(2).value<double>(3.5).text("cd",
                                                                                                                3);
    const fixture file =
        bintable(with(bintable_records(15, 2, {"J", "D", "3A"}),
                      {card_record("TTYPE1", "'COUNT'"), card_record("TTYPE2", "'FLUX'"), card_record("TUNIT2", "'Jy'"),
                       card_record("TSCAL1", "2"), card_record("TZERO1", "10"), card_record("TNULL1", "-1"),
                       card_record("TDISP2", "'F8.3'"), card_record("TTYPE3", "'NAME    '"),
                       card_record("EXTNAME", "'CATALOG'")}),
                 rows);
    const reader input = open(file);
    const table_info table = input.table(1);
    EXPECT_EQ(table.kind, hdu_kind::bintable);
    EXPECT_EQ(table.rows, 2U);
    EXPECT_EQ(table.row_size, 15U);
    EXPECT_EQ(table.heap_offset, 30U);
    EXPECT_EQ(table.heap_size, 0U);
    EXPECT_EQ(table.header, input.hdu(1).header());
    ASSERT_EQ(table.columns.size(), 3U);
    const column_info& count = table.columns[0];
    EXPECT_EQ(count.name, "COUNT");
    EXPECT_EQ(count.format, (column_format{.type = column_type::int32}));
    EXPECT_EQ(count.scaling, (openfits::scaling{.scale = 2.0, .zero = 10.0}));
    EXPECT_EQ(count.null, -1);
    EXPECT_EQ(count.offset, 0U);
    EXPECT_EQ(count.value_type(), numeric_type::int64);
    const column_info& flux = table.columns[1];
    EXPECT_EQ(flux.unit, "Jy");
    EXPECT_EQ(flux.display, openfits::display_format::parse("F8.3"));
    EXPECT_EQ(flux.offset, 4U);
    EXPECT_EQ(flux.value_type(), numeric_type::float64);
    const column_info& name = table.columns[2];
    EXPECT_EQ(name.name, "NAME");
    EXPECT_EQ(name.offset, 12U);
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { (void)name.value_type(); }));
    EXPECT_EQ(input.first_table(), 1U);
}

TEST(table_read, finds_columns_by_name)
{
    byte_writer rows;
    rows.values<std::int16_t>({1, 2, 3, 4});
    const reader input = open(bintable(with(bintable_records(8, 1, {"I", "I", "I", "I"}),
                                            {card_record("TTYPE1", "'Time'"), card_record("TTYPE2", "'RATE_1'"),
                                             card_record("TTYPE3", "'RATE_22'"), card_record("TTYPE4", "'TIME'")}),
                                       rows));
    const table_info table = input.table(1);
    // Case-insensitive by default (standard §7.3.2), the first column of the name found.
    EXPECT_EQ(table.find_column("TIME"), 0U);
    EXPECT_EQ(table.find_column("time"), 0U);
    EXPECT_EQ(table.find_column("TIME", true), 3U);
    EXPECT_EQ(table.find_column("Time  "), 0U);
    EXPECT_EQ(table.find_column("RATE"), std::nullopt);
    EXPECT_EQ(table.matching_columns("RATE_#"), (std::vector<std::size_t>{1, 2}));
    EXPECT_EQ(table.matching_columns("rate_?"), (std::vector<std::size_t>{1}));
    EXPECT_EQ(table.matching_columns("*"), (std::vector<std::size_t>{0, 1, 2, 3}));
    EXPECT_EQ(table.matching_columns("T*", true), (std::vector<std::size_t>{0, 3}));
    EXPECT_EQ(table.matching_columns("Ti*", true), (std::vector<std::size_t>{0}));
    EXPECT_EQ(input.read_column<std::int16_t>(1, "rate_22"), (std::vector<std::int16_t>{3}));
    EXPECT_TRUE(throws<usage_error>(errc::column_not_found, [&] { (void)input.read_column<double>(1, "FLUX"); }));
    EXPECT_TRUE(throws<usage_error>(errc::column_not_found, [&] { (void)input.read_column<double>(1, 4); }));
}

TEST(table_read, units_of_other_kinds_hold_no_table)
{
    fixture file;
    file.header(openfits::test::primary_records(8, {4})).data(4);
    file.image({2});
    file.header(with(bintable_records(1, 1, {"B"}), {card_record("ZTABLE", "T")})).data(1);
    file.header(with(bintable_records(1, 1, {"B"}), {card_record("ZIMAGE", "T")})).data(1);
    const reader input = open(file);
    EXPECT_TRUE(throws<usage_error>(errc::not_a_table, [&] { (void)input.table(0); }));
    EXPECT_TRUE(throws<usage_error>(errc::not_a_table, [&] { (void)input.table(1); }));
    // A compressed table is the table it holds, and this one, without the keywords of standard §10.3.1, holds none.
    EXPECT_TRUE(throws<openfits::invalid_data_error>(errc::invalid_compression, [&] { (void)input.table(2); }));
    // A compressed image is the binary table it is.
    EXPECT_EQ(input.table(3).columns.size(), 1U);
    EXPECT_TRUE(throws<usage_error>(errc::not_a_table, [&] { (void)input.read_column<double>(0, 0); }));
}

TEST(table_read, a_table_whose_header_describes_no_columns_refuses_its_table)
{
    byte_writer rows;
    rows.values<std::int16_t>({1});
    const fixture bad_format = bintable(bintable_records(2, 1, {"Z"}), rows);
    EXPECT_TRUE(throws<invalid_data_error>(errc::invalid_column_format, [&] { (void)open(bad_format).table(1); }));
    std::vector<std::string> records = bintable_records(2, 1, {"I"});
    records.erase(records.begin() + 7);
    const fixture no_tfields = bintable(records, rows);
    const reader input = open(no_tfields);
    EXPECT_TRUE(throws<invalid_data_error>(errc::missing_keyword, [&] { (void)input.table(1); }));
    // The walk sized the unit from its mandatory keywords, and goes on.
    EXPECT_EQ(input.hdu(1).data_size(), 2U);
}

TEST(table_read, reads_a_column_as_every_numeric_type)
{
    const reader input = open(int16_table());
    EXPECT_EQ(input.read_column<std::int16_t>(1, 0), (std::vector<std::int16_t>{-3, -2, -1, 0, 1, 2, 3}));
    EXPECT_EQ(input.read_column<std::int64_t>(1, 0), (std::vector<std::int64_t>{-3, -2, -1, 0, 1, 2, 3}));
    EXPECT_EQ(input.read_column<double>(1, 0), (std::vector<double>{-3, -2, -1, 0, 1, 2, 3}));
    EXPECT_EQ(input.read_column<float>(1, 0), (std::vector<float>{-3, -2, -1, 0, 1, 2, 3}));
    EXPECT_TRUE(
        throws<invalid_data_error>(errc::value_out_of_range, [&] { (void)input.read_column<std::uint8_t>(1, 0); }));
    std::vector<std::uint8_t> clamped(7);
    EXPECT_EQ(input.read_column(1, 0, std::span<std::uint8_t>(clamped), {.overflow = overflow_policy::clamp}).clamped,
              3U);
    EXPECT_EQ(clamped, (std::vector<std::uint8_t>{0, 0, 0, 0, 1, 2, 3}));
}

TEST(table_read, applies_tscal_and_tzero)
{
    // Standard §7.3.2, Eq. 7: physical = TZEROn + TSCALn x field.
    const reader scaled = open(int16_table({card_record("TSCAL1", "0.5"), card_record("TZERO1", "100.0")}));
    EXPECT_EQ(scaled.read_column<double>(1, 0), (std::vector<double>{98.5, 99, 99.5, 100, 100.5, 101, 101.5}));
    EXPECT_EQ(scaled.read_column<std::int32_t>(1, 0), (std::vector<std::int32_t>{98, 99, 99, 100, 100, 101, 101}));
    EXPECT_EQ(scaled.read_column<std::int32_t>(1, 0, {.rounding = openfits::rounding::nearest}),
              (std::vector<std::int32_t>{99, 99, 100, 100, 101, 101, 102}));
    EXPECT_EQ(scaled.read_column<std::int16_t>(1, 0, {.scaling = scaling_policy::raw}),
              (std::vector<std::int16_t>{-3, -2, -1, 0, 1, 2, 3}));
    EXPECT_EQ(scaled.read_column<double>(
                  1, 0, {.scaling = scaling_policy::override, .override_scaling = {.scale = 10.0, .zero = 1.0}}),
              (std::vector<double>{-29, -19, -9, 1, 11, 21, 31}));
    const reader zero = open(int16_table({card_record("TSCAL1", "0")}));
    EXPECT_TRUE(throws<invalid_data_error>(errc::reserved_keyword, [&] { (void)zero.read_column<double>(1, 0); }));
    EXPECT_EQ(zero.read_column<std::int16_t>(1, 0, {.scaling = scaling_policy::raw}).size(), 7U);
}

TEST(table_read, reads_the_unsigned_conventions_of_columns)
{
    // Standard §5.2.5, Table 19: B with TZEROn -128, I with 32768, J with 2^31 and K with 2^63.
    byte_writer rows;
    rows.value<std::uint8_t>(0).value<std::int16_t>(-32768).value<std::int32_t>(
        std::numeric_limits<std::int32_t>::min());
    rows.value<std::int64_t>(std::numeric_limits<std::int64_t>::max());
    rows.value<std::uint8_t>(255).value<std::int16_t>(32767).value<std::int32_t>(-1).value<std::int64_t>(-1);
    const fixture file =
        bintable(with(bintable_records(15, 2, {"B", "I", "J", "K"}),
                      {card_record("TZERO1", "-128"), card_record("TZERO2", "32768"),
                       card_record("TZERO3", "2147483648"), card_record("TZERO4", "9223372036854775808")}),
                 rows);
    const reader input = open(file);
    const table_info table = input.table(1);
    EXPECT_EQ(table.columns[0].value_type(), numeric_type::int8);
    EXPECT_EQ(table.columns[1].value_type(), numeric_type::uint16);
    EXPECT_EQ(table.columns[2].value_type(), numeric_type::uint32);
    EXPECT_EQ(table.columns[3].value_type(), numeric_type::uint64);
    EXPECT_EQ(input.read_column<std::int8_t>(1, 0), (std::vector<std::int8_t>{-128, 127}));
    EXPECT_EQ(input.read_column<std::uint16_t>(1, 1), (std::vector<std::uint16_t>{0, 65535}));
    EXPECT_EQ(input.read_column<std::uint32_t>(1, 2), (std::vector<std::uint32_t>{0, 2147483647}));
    EXPECT_EQ(input.read_column<std::uint64_t>(1, 3),
              (std::vector<std::uint64_t>{18446744073709551615ULL, 9223372036854775807ULL}));
}

TEST(table_read, tnull_marks_undefined_integers)
{
    // Standard §7.3.2: TNULLn is the stored value, before scaling.
    const reader input = open(int16_table({card_record("TNULL1", "0"), card_record("TZERO1", "10")}));
    std::vector<double> values = input.read_column<double>(1, 0);
    EXPECT_TRUE(std::isnan(values[3]));
    EXPECT_EQ(values[4], 11.0);
    // An integer destination without a policy gets TNULLn converted as any stored value, as BLANK of images.
    EXPECT_EQ(input.read_column<std::int16_t>(1, 0)[3], 10);
    EXPECT_EQ(input.read_column<std::int16_t>(1, 0, {.blank = blank_policy::value, .blank_value = -99})[3], -99);
    std::vector<std::uint8_t> mask(7);
    std::vector<std::int32_t> masked(7);
    (void)input.read_column(1, 0, std::span<std::int32_t>(masked),
                            {.blank = blank_policy::mask, .mask = std::span<std::uint8_t>(mask)});
    EXPECT_EQ(mask, (std::vector<std::uint8_t>{0, 0, 0, 1, 0, 0, 0}));
    EXPECT_EQ(masked[3], 0);
    EXPECT_EQ(masked[0], 7);
}

TEST(table_read, nan_is_the_undefined_value_of_floating_point_columns)
{
    byte_writer rows;
    rows.values<float>({1.5F, std::numeric_limits<float>::quiet_NaN(), -0.0F});
    const reader input = open(bintable(bintable_records(4, 3, {"E"}), rows));
    const std::vector<float> values = input.read_column<float>(1, 0);
    EXPECT_EQ(values[0], 1.5F);
    EXPECT_TRUE(std::isnan(values[1]));
    EXPECT_TRUE(std::signbit(values[2]));
    EXPECT_TRUE(
        throws<invalid_data_error>(errc::undefined_value, [&] { (void)input.read_column<std::int32_t>(1, 0); }));
    EXPECT_EQ(input.read_column<std::int32_t>(1, 0, {.blank = blank_policy::value, .blank_value = 7}),
              (std::vector<std::int32_t>{1, 7, 0}));
}

TEST(table_read, reads_fields_of_several_elements_and_empty_fields)
{
    byte_writer rows;
    rows.values<std::int32_t>({1, 2, 3}).values<std::int32_t>({4, 5, 6});
    const reader input = open(bintable(bintable_records(12, 2, {"3J", "0E"}), rows));
    EXPECT_EQ(input.read_column<std::int64_t>(1, 0), (std::vector<std::int64_t>{1, 2, 3, 4, 5, 6}));
    // A repeat count of 0 is an empty field (standard §7.3.1).
    EXPECT_TRUE(input.read_column<float>(1, 1).empty());
    EXPECT_EQ(input.table(1).columns[1].offset, 12U);
}

TEST(table_read, reads_row_ranges)
{
    const reader input = open(int16_table());
    EXPECT_EQ(input.read_column<std::int16_t>(1, 0, {.rows = row_range{.first = 2, .count = 3}}),
              (std::vector<std::int16_t>{-1, 0, 1}));
    EXPECT_EQ(input.read_column<std::int16_t>(1, 0, {.rows = row_range{.first = 5}}),
              (std::vector<std::int16_t>{2, 3}));
    EXPECT_TRUE(input.read_column<std::int16_t>(1, 0, {.rows = row_range{.first = 7}}).empty());
    EXPECT_TRUE(input.read_column<std::int16_t>(1, 0, {.rows = row_range{.first = 3, .count = 0}}).empty());
    EXPECT_TRUE(throws<usage_error>(
        errc::invalid_argument, [&] { (void)input.read_column<std::int16_t>(1, 0, {.rows = row_range{.first = 8}}); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] {
        (void)input.read_column<std::int16_t>(1, 0, {.rows = row_range{.first = 6, .count = 2}});
    }));
}

TEST(table_read, refuses_destinations_that_do_not_fit)
{
    const reader input = open(int16_table());
    std::vector<std::int16_t> short_destination(6);
    EXPECT_TRUE(throws<usage_error>(
        errc::invalid_argument, [&] { (void)input.read_column(1, 0, std::span<std::int16_t>(short_destination)); }));
    std::vector<std::int16_t> values(7);
    std::vector<std::uint8_t> mask(6);
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] {
        (void)input.read_column(1, 0, std::span<std::int16_t>(values),
                                {.blank = blank_policy::mask, .mask = std::span<std::uint8_t>(mask)});
    }));
    EXPECT_TRUE(throws<usage_error>(errc::column_type, [&] { (void)input.read_column<std::complex<float>>(1, 0); }));
}

TEST(table_read, reads_logicals_as_1_and_0)
{
    // Standard §7.3.3.1: T, F, and a 0 byte for an undefined value.
    byte_writer rows;
    rows.bytes({'T', 'F', 0, 'T'});
    const reader input = open(bintable(bintable_records(2, 2, {"2L"}), rows));
    EXPECT_EQ(input.table(1).columns[0].value_type(), numeric_type::uint8);
    std::vector<float> values = input.read_column<float>(1, 0);
    EXPECT_EQ(values[0], 1.0F);
    EXPECT_EQ(values[1], 0.0F);
    EXPECT_TRUE(std::isnan(values[2]));
    // An undefined logical has no stored value for an integer destination.
    EXPECT_TRUE(
        throws<invalid_data_error>(errc::undefined_value, [&] { (void)input.read_column<std::uint8_t>(1, 0); }));
    std::vector<std::uint8_t> mask(4);
    std::vector<std::uint8_t> flags(4);
    (void)input.read_column(1, 0, std::span<std::uint8_t>(flags),
                            {.blank = blank_policy::mask, .mask = std::span<std::uint8_t>(mask)});
    EXPECT_EQ(flags, (std::vector<std::uint8_t>{1, 0, 0, 1}));
    EXPECT_EQ(mask, (std::vector<std::uint8_t>{0, 0, 1, 0}));
    // It stays undefined when no stored value is.
    EXPECT_TRUE(std::isnan(input.read_column<float>(1, 0, {.blank = blank_policy::ignore})[2]));
    EXPECT_TRUE(throws<invalid_data_error>(
        errc::undefined_value, [&] { (void)input.read_column<std::uint8_t>(1, 0, {.blank = blank_policy::ignore}); }));
    byte_writer bad;
    bad.bytes({'T', 't'});
    const reader lowercase = open(bintable(bintable_records(2, 1, {"2L"}), bad));
    EXPECT_TRUE(throws<invalid_data_error>(errc::invalid_field, [&] { (void)lowercase.read_column<float>(1, 0); }));
}

TEST(table_read, reads_bits_most_significant_first)
{
    // Standard §7.3.3.1: the bits from the most significant, and an integral number of bytes.
    for (std::uint64_t bits = 1; bits <= 17; ++bits) {
        SCOPED_TRACE(bits);
        const std::uint64_t bytes = (bits + 7) / 8;
        byte_writer rows;
        std::vector<std::uint8_t> raw(bytes);
        for (std::uint64_t i = 0; i < bytes; ++i) {
            raw[i] = static_cast<std::uint8_t>(0xA5U ^ i);
        }
        rows.bytes(raw);
        const reader input = open(bintable(bintable_records(bytes, 1, {std::to_string(bits) + "X"}), rows));
        const std::vector<std::uint8_t> values = input.read_column<std::uint8_t>(1, 0);
        ASSERT_EQ(values.size(), bits);
        for (std::uint64_t i = 0; i < bits; ++i) {
            const unsigned byte = raw[i / 8];
            EXPECT_EQ(values[i], (byte >> (7 - (i % 8))) & 1U) << i;
        }
    }
}

TEST(table_read, reads_complex_numbers)
{
    byte_writer rows;
    rows.values<float>({1.5F, -2.0F}).values<double>({3.0, 4.0});
    rows.values<float>({std::numeric_limits<float>::quiet_NaN(), 1.0F}).values<double>({-1.0, 0.5});
    const reader input = open(bintable(
        with(bintable_records(24, 2, {"C", "M"}), {card_record("TSCAL2", "2"), card_record("TZERO2", "1")}), rows));
    const std::vector<std::complex<float>> c = input.read_column<std::complex<float>>(1, 0);
    EXPECT_EQ(c[0], std::complex<float>(1.5F, -2.0F));
    EXPECT_TRUE(std::isnan(c[1].real()));
    // TZEROn is added to the real part alone (standard §7.3.2).
    EXPECT_EQ(input.read_column<std::complex<double>>(1, 1),
              (std::vector<std::complex<double>>{{7.0, 8.0}, {-1.0, 1.0}}));
    EXPECT_EQ(input.read_column<std::complex<double>>(1, 0)[0], std::complex<double>(1.5, -2.0));
    // A NaN in either part makes the value undefined (standard §7.3.3.1).
    std::vector<std::complex<double>> values(2);
    std::vector<std::uint8_t> mask(2);
    (void)input.read_column(1, 0, std::span<std::complex<double>>(values),
                            {.blank = blank_policy::mask, .mask = std::span<std::uint8_t>(mask)});
    EXPECT_EQ(mask, (std::vector<std::uint8_t>{0, 1}));
    EXPECT_EQ(input.read_column<std::complex<double>>(1, 0, {.blank = blank_policy::value, .blank_value = 9.0})[1],
              std::complex<double>(9.0, 0.0));
    EXPECT_TRUE(throws<usage_error>(errc::column_type, [&] { (void)input.read_column<double>(1, 0); }));
}

TEST(table_read, reads_characters_as_strings)
{
    // Standard §7.3.3.1: a string may end before the field with a NUL; trailing blanks are kept, or trimmed.
    byte_writer rows;
    rows.text("M 31", 6).text("ab", 6, '\0').text("full!!", 6);
    const reader input = open(bintable(bintable_records(6, 3, {"6A"}), rows));
    EXPECT_EQ(input.read_strings(1, 0), (std::vector<std::string>{"M 31  ", "ab", "full!!"}));
    EXPECT_EQ(input.read_strings(1, 0, {.trim_strings = true}), (std::vector<std::string>{"M 31", "ab", "full!!"}));
    EXPECT_TRUE(throws<usage_error>(errc::column_type, [&] { (void)input.read_column<double>(1, 0); }));
}

TEST(table_read, reads_the_substrings_of_tdim_and_of_the_raw_convention)
{
    byte_writer rows;
    rows.text("abcdefghijkl", 12);
    // Standard §7.3.2: TFORMn = '60A' and TDIMn = '(5,4,3)' hold a 4 x 3 array of strings of 5 characters.
    const reader tdim = open(bintable(with(bintable_records(12, 1, {"12A"}), {card_record("TDIM1", "'(4,3)'")}), rows));
    EXPECT_EQ(tdim.table(1).columns[0].dimensions, (std::vector<std::uint64_t>{4, 3}));
    EXPECT_EQ(tdim.read_strings(1, 0), (std::vector<std::string>{"abcd", "efgh", "ijkl"}));
    const reader raw = open(bintable(bintable_records(12, 1, {"12A3"}), rows));
    EXPECT_EQ(raw.table(1).columns[0].dimensions, (std::vector<std::uint64_t>{3, 4}));
    EXPECT_EQ(raw.read_strings(1, 0), (std::vector<std::string>{"abc", "def", "ghi", "jkl"}));
    const reader empty = open(bintable(bintable_records(0, 2, {"0A"}), byte_writer()));
    EXPECT_EQ(empty.read_strings(1, 0), (std::vector<std::string>{"", ""}));
}

TEST(table_read, renders_numbers_with_their_display_format)
{
    byte_writer rows;
    rows.value<std::int16_t>(42).value<double>(1.25).value<std::uint8_t>('T');
    rows.value<std::int16_t>(-7).value<double>(-1e10).value<std::uint8_t>(0);
    const reader input = open(bintable(
        with(bintable_records(11, 2, {"I", "D", "L"}), {card_record("TDISP1", "'I5'"), card_record("TNULL1", "-7")}),
        rows));
    EXPECT_EQ(input.read_strings(1, 0), (std::vector<std::string>{"   42", ""}));
    // Without TDISPn, G23.15 for doubles.
    const openfits::display_format general = openfits::display_format::parse("G23.15");
    EXPECT_EQ(input.read_strings(1, 1), (std::vector<std::string>{openfits::format_display(general, 1.25),
                                                                  openfits::format_display(general, -1e10)}));
    EXPECT_EQ(input.read_strings(1, 2), (std::vector<std::string>{"T", ""}));
    std::vector<std::uint8_t> mask(2);
    (void)input.read_strings(1, 2, {.blank = blank_policy::mask, .mask = std::span<std::uint8_t>(mask)});
    EXPECT_EQ(mask, (std::vector<std::uint8_t>{0, 1}));
}

TEST(table_read, renders_the_bytes_of_bits_and_complex_values)
{
    byte_writer rows;
    rows.bytes({0xA5, 0x80}).values<float>({1.0F, -2.0F});
    const reader input = open(bintable(bintable_records(10, 1, {"9X", "C"}), rows));
    // Each byte of bits is an unsigned integer for display (standard §7.3.2), B8.8 by default.
    EXPECT_EQ(input.read_strings(1, 0), (std::vector<std::string>{"10100101", "10000000"}));
    EXPECT_EQ(input.read_strings(1, 1).size(), 1U);
    EXPECT_EQ(input.read_strings(1, 1).front().front(), '(');
}

// The reads that a read of the 32-bit column of a table costs: rows of row_size bytes, the field of each at offset 8,
// after a 64-bit one, and characters after it; the values read are checked.
std::vector<openfits::test::served_read> reads_of(std::uint64_t row_size, std::uint64_t rows, std::size_t transfer)
{
    byte_writer data;
    for (std::uint64_t row = 0; row < rows; ++row) {
        data.value<std::int64_t>(-static_cast<std::int64_t>(row)).value<std::int32_t>(static_cast<std::int32_t>(row));
        data.text("", row_size - 12);
    }
    const fixture file =
        bintable(bintable_records(row_size, rows, {"K", "J", std::to_string(row_size - 12) + "A"}), data);
    const reader input = open(file);
    auto counting = std::make_unique<counting_source>(file.bytes());
    const counting_source* counter = counting.get();
    const openfits::detail::thread_safe_source source(std::move(counting));
    std::vector<std::int32_t> values(rows);
    const column_destination target = column_destination::of(1, std::span<std::int32_t>(values));
    (void)openfits::detail::read_columns(source, input.hdu(1), input.table(1),
                                         std::span<const column_destination>(&target, 1), {}, transfer);
    for (std::uint64_t row = 0; row < rows; ++row) {
        EXPECT_EQ(values[row], static_cast<std::int32_t>(row));
    }
    return counter->reads();
}

TEST(table_read, reads_close_rows_in_chunks)
{
    // Rows of 32 bytes: the fields of every row, from the first to the last, in one read of the span.
    const std::vector<openfits::test::served_read> reads = reads_of(32, 100, std::size_t{4} << 20);
    ASSERT_EQ(reads.size(), 1U);
    EXPECT_EQ(reads.front().size, (99 * 32) + 4);
    // A chunk of 10 rows' worth.
    EXPECT_EQ(reads_of(32, 100, 320).size(), 10U);
}

TEST(table_read, reads_fields_far_apart_one_by_one)
{
    // Rows of 256 KiB, whose field of 4 bytes lies far from that of the next row: the fields alone.
    const std::uint64_t row_size = std::uint64_t{256} << 10;
    const std::vector<openfits::test::served_read> reads = reads_of(row_size, 5, std::size_t{4} << 20);
    ASSERT_EQ(reads.size(), 5U);
    for (const openfits::test::served_read& read : reads) {
        EXPECT_EQ(read.size, 4U);
    }
}

TEST(table_read, reads_fields_larger_than_a_chunk_in_pieces)
{
    // A field of 3 x 4 bytes, read 4 bytes at a time: three reads for each row.
    byte_writer data;
    data.values<std::int32_t>({1, 2, 3, 4, 5, 6});
    const fixture file = bintable(bintable_records(12, 2, {"3J"}), data);
    const reader input = open(file);
    auto counting = std::make_unique<counting_source>(file.bytes());
    const counting_source* counter = counting.get();
    const openfits::detail::thread_safe_source source(std::move(counting));
    std::vector<double> values(6);
    const column_destination target = column_destination::of(0, std::span<double>(values));
    (void)openfits::detail::read_columns(source, input.hdu(1), input.table(1),
                                         std::span<const column_destination>(&target, 1), {}, 4);
    EXPECT_EQ(values, (std::vector<double>{1, 2, 3, 4, 5, 6}));
    EXPECT_EQ(counter->reads().size(), 6U);
}

TEST(table_read, reads_several_columns_in_one_pass)
{
    byte_writer data;
    for (int row = 0; row < 50; ++row) {
        data.value<std::int16_t>(static_cast<std::int16_t>(row)).value<float>(static_cast<float>(row) / 2);
        data.bytes({static_cast<std::uint8_t>(row % 2 == 0 ? 'T' : 'F')});
    }
    const fixture file = bintable(bintable_records(7, 50, {"I", "E", "L"}), data);
    const reader input = open(file);
    auto counting = std::make_unique<counting_source>(file.bytes());
    const counting_source* counter = counting.get();
    const openfits::detail::thread_safe_source source(std::move(counting));
    std::vector<std::int32_t> first(30);
    std::vector<double> second(30);
    std::vector<std::uint8_t> third(30);
    const std::vector<column_destination> targets{column_destination::of(0, std::span<std::int32_t>(first)),
                                                  column_destination::of(1, std::span<double>(second)),
                                                  column_destination::of(2, std::span<std::uint8_t>(third))};
    (void)openfits::detail::read_columns(source, input.hdu(1), input.table(1), targets,
                                         {.rows = row_range{.first = 10, .count = 30}}, std::size_t{4} << 20);
    EXPECT_EQ(counter->reads().size(), 1U);
    std::vector<std::int32_t> all(30);
    std::vector<double> halves(30);
    std::vector<std::uint8_t> flags(30);
    const std::vector<column_destination> through_reader{column_destination::of(0, std::span<std::int32_t>(all)),
                                                         column_destination::of(1, std::span<double>(halves)),
                                                         column_destination::of(2, std::span<std::uint8_t>(flags))};
    (void)input.read_columns(1, through_reader, {.rows = row_range{.first = 10, .count = 30}});
    for (std::size_t i = 0; i < 30; ++i) {
        EXPECT_EQ(all[i], static_cast<std::int32_t>(i + 10));
        EXPECT_EQ(halves[i], static_cast<double>(i + 10) / 2);
        EXPECT_EQ(flags[i], (i + 10) % 2 == 0 ? 1 : 0);
    }
}

TEST(table_read, reports_progress_and_cancels)
{
    const reader input = open(int16_table());
    std::vector<std::uint64_t> reported;
    const openfits::progress_function record = [&reported](std::uint64_t done, std::uint64_t total) {
        EXPECT_EQ(total, 14U);
        reported.push_back(done);
        return true;
    };
    (void)input.read_column<double>(1, 0, {.progress = record});
    EXPECT_EQ(reported.front(), 0U);
    EXPECT_EQ(reported.back(), 14U);
    EXPECT_TRUE(throws<openfits::cancelled_error>(errc::cancelled, [&] {
        (void)input.read_column<double>(1, 0, {.progress = [](std::uint64_t, std::uint64_t) { return false; }});
    }));
}

TEST(table_read, a_truncated_table_fails_beyond_its_bytes)
{
    fixture file = int16_table();
    file.truncate(file.size() - 2880 + 6);
    const reader input = open(file);
    EXPECT_EQ(input.read_column<std::int16_t>(1, 0, {.rows = row_range{.first = 0, .count = 3}}),
              (std::vector<std::int16_t>{-3, -2, -1}));
    EXPECT_TRUE(throws<openfits::io_error>(errc::end_of_data, [&] { (void)input.read_column<std::int16_t>(1, 0); }));
}

} // namespace
