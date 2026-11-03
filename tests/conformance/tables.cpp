// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Standard §6, §7.2 and §7.3: random groups, ASCII tables and binary tables, each rule with a constructed fixture.

#include <openfits/error.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/table.h>

#include "support/fixture_builder.h"
#include "support/table_fixtures.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

using openfits::diagnostic;
using openfits::errc;
using openfits::reader;
using openfits::severity;
using openfits::test::ascii_field;
using openfits::test::ascii_table_records;
using openfits::test::bintable_records;
using openfits::test::byte_writer;
using openfits::test::card_record;
using openfits::test::extension_records;
using openfits::test::fixture;
using openfits::test::throws;
using openfits::test::with;

reader open(const fixture& file)
{
    return reader(std::make_unique<openfits::memory_source>(std::vector<std::byte>(file.bytes())));
}

bool has(const std::vector<diagnostic>& found, errc code, severity level)
{
    return std::ranges::any_of(found,
                               [&](const diagnostic& entry) { return entry.code == code && entry.severity == level; });
}

fixture with_table(const std::vector<std::string>& records, const byte_writer& data)
{
    fixture file;
    file.primary();
    file.header(records).data(data.data());
    return file;
}

// §7.2.1, Table 14: TFIELDS from 0 to 999, and TBCOLn and TFORMn for every field.
TEST(conformance_tables, ascii_tables_have_the_mandatory_keywords_of_table_14)
{
    byte_writer row;
    row.text("12 34", 5);
    const reader valid = open(with_table(
        ascii_table_records(5, 1, {ascii_field{.column = 1, .format = "I2"}, ascii_field{.column = 4, .format = "I2"}}),
        row));
    EXPECT_TRUE(valid.hdu(1).diagnostics().empty());
    EXPECT_EQ(valid.read_column<int>(1, 1), (std::vector<int>{34}));

    std::vector<std::string> without_tbcol = ascii_table_records(5, 1, {ascii_field{.column = 1, .format = "I2"}});
    without_tbcol.erase(without_tbcol.begin() + 8);
    const reader missing = open(with_table(without_tbcol, row));
    EXPECT_TRUE(has(missing.hdu(1).diagnostics(), errc::missing_keyword, severity::error));
    EXPECT_TRUE(throws<openfits::invalid_data_error>(errc::missing_keyword, [&] { (void)missing.table(1); }));

    std::vector<std::string> too_many = extension_records("TABLE", 8, {5, 1});
    too_many.push_back(card_record("TFIELDS", "1000"));
    EXPECT_TRUE(has(open(with_table(too_many, row)).hdu(1).diagnostics(), errc::invalid_structure, severity::error));
}

// §7.2.1: a field lies within the characters of a row.
TEST(conformance_tables, an_ascii_field_beyond_its_row_is_refused)
{
    byte_writer row;
    row.text("12345", 5);
    const reader input = open(with_table(ascii_table_records(5, 1, {ascii_field{.column = 4, .format = "I3"}}), row));
    EXPECT_TRUE(has(input.hdu(1).diagnostics(), errc::invalid_structure, severity::error));
    EXPECT_TRUE(throws<openfits::invalid_data_error>(errc::invalid_structure, [&] { (void)input.table(1); }));
}

// §7.2.3: the rows follow each other independently of the blocks; §7.2.4: fields may leave characters between them,
// overlap, and come in any order.
TEST(conformance_tables, ascii_rows_cross_blocks_and_fields_overlap)
{
    byte_writer rows;
    for (int i = 0; i < 400; ++i) {
        std::string text = std::to_string(1000 + i);
        rows.text("[" + text + "]", 9);
    }
    const reader input = open(with_table(
        ascii_table_records(9, 400,
                            {ascii_field{.column = 2, .format = "I4"}, ascii_field{.column = 3, .format = "I2"},
                             ascii_field{.column = 1, .format = "A6"}}),
        rows));
    const std::vector<int> values = input.read_column<int>(1, 0);
    ASSERT_EQ(values.size(), 400U);
    for (int i = 0; i < 400; ++i) {
        EXPECT_EQ(values[static_cast<std::size_t>(i)], 1000 + i);
    }
    EXPECT_EQ(input.read_column<int>(1, 1).back(), 39);
    EXPECT_EQ(input.read_strings(1, 2).front(), "[1000]");
}

// §7.2.5: a blank field is zero, and TNULLn, filled with spaces to the width, marks an undefined one.
TEST(conformance_tables, a_blank_ascii_field_is_zero_unless_tnull_says_otherwise)
{
    byte_writer rows;
    rows.text("    ", 4).text(" 7  ", 4);
    const fixture plain = with_table(ascii_table_records(4, 2, {ascii_field{.column = 1, .format = "F4.1"}}), rows);
    EXPECT_EQ(open(plain).read_column<double>(1, 0), (std::vector<double>{0.0, 0.7}));
    const fixture blank_null = with_table(
        with(ascii_table_records(4, 2, {ascii_field{.column = 1, .format = "F4.1"}}), {card_record("TNULL1", "' '")}),
        rows);
    std::vector<std::uint8_t> mask(2);
    std::vector<double> values(2);
    (void)open(blank_null)
        .read_column(1, 0, std::span<double>(values),
                     {.blank = openfits::blank_policy::mask, .mask = std::span<std::uint8_t>(mask)});
    EXPECT_EQ(mask, (std::vector<std::uint8_t>{1, 0}));
}

// §7.3.1, Table 17: the mandatory keywords in their order; TFIELDS elsewhere is read with a warning.
TEST(conformance_tables, binary_tables_have_the_mandatory_keywords_of_table_17)
{
    byte_writer row;
    row.value<std::int32_t>(5);
    const reader valid = open(with_table(bintable_records(4, 1, {"J"}), row));
    EXPECT_TRUE(valid.hdu(1).diagnostics().empty());

    std::vector<std::string> moved = bintable_records(4, 1, {"J"});
    std::rotate(moved.begin() + 7, moved.begin() + 8, moved.end());
    const reader late = open(with_table(moved, row));
    EXPECT_TRUE(has(late.hdu(1).diagnostics(), errc::keyword_order, severity::warning));
    EXPECT_EQ(late.read_column<int>(1, 0), (std::vector<int>{5}));

    std::vector<std::string> wide = bintable_records(4, 1, {"J"});
    wide[1] = card_record("BITPIX", "16");
    EXPECT_TRUE(has(open(with_table(wide, row)).hdu(1).diagnostics(), errc::invalid_structure, severity::error));
    std::vector<std::string> no_form = bintable_records(4, 1, {"J"});
    no_form.pop_back();
    EXPECT_TRUE(has(open(with_table(no_form, row)).hdu(1).diagnostics(), errc::missing_keyword, severity::error));
}

// §7.3.1, Eq. 8: the fields of a row take NAXIS1 bytes; more is refused.
TEST(conformance_tables, the_fields_of_a_row_take_naxis1_bytes)
{
    byte_writer row;
    row.value<std::int32_t>(5);
    const reader input = open(with_table(bintable_records(4, 1, {"J", "B"}), row));
    EXPECT_TRUE(has(input.hdu(1).diagnostics(), errc::invalid_column_format, severity::error));
    EXPECT_TRUE(throws<openfits::invalid_data_error>(errc::invalid_column_format, [&] { (void)input.table(1); }));
}

// §7.3.1, Table 18: the bytes of each type, the descriptors of P and Q among them.
TEST(conformance_tables, every_type_of_table_18_takes_its_bytes)
{
    byte_writer row;
    row.bytes({'T', 0x80, 7}).value<std::int16_t>(-2).value<std::int32_t>(-3).value<std::int64_t>(-4).text("a", 1);
    row.value<float>(1.5F).value<double>(2.5).values<float>({1.0F, 2.0F}).values<double>({3.0, 4.0});
    row.values<std::int32_t>({0, 0}).values<std::int64_t>({0, 0});
    const reader input = open(
        with_table(bintable_records(84, 1, {"L", "X", "B", "I", "J", "K", "A", "E", "D", "C", "M", "PJ", "QJ"}), row));
    const openfits::table_info table = input.table(1);
    const std::vector<std::uint64_t> offsets{0, 1, 2, 3, 5, 9, 17, 18, 22, 30, 38, 54, 62};
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        EXPECT_EQ(table.columns[i].offset, offsets[i]) << i;
    }
    EXPECT_EQ(input.read_column<int>(1, 0), (std::vector<int>{1}));
    EXPECT_EQ(input.read_column<int>(1, 1), (std::vector<int>{1}));
    EXPECT_EQ(input.read_column<int>(1, 5), (std::vector<int>{-4}));
    EXPECT_EQ(input.read_column<std::complex<double>>(1, 10), (std::vector<std::complex<double>>{{3.0, 4.0}}));
}

// §7.3.5: THEAP gives the heap, from the end of the rows to the end of the data.
TEST(conformance_tables, theap_lies_between_the_rows_and_the_end_of_the_data)
{
    byte_writer data;
    data.value<std::int32_t>(1).value<std::int32_t>(0).text("", 8, '\0');
    for (const auto& [theap, level] : std::vector<std::pair<std::string, std::optional<severity>>>{
             {"8", std::nullopt}, {"16", std::nullopt}, {"4", severity::error}, {"17", severity::error}}) {
        SCOPED_TRACE(theap);
        const reader input =
            open(with_table(with(bintable_records(8, 1, {"PB"}, 8), {card_record("THEAP", theap)}), data));
        if (level) {
            EXPECT_TRUE(has(input.hdu(1).diagnostics(), errc::invalid_structure, *level));
        } else {
            EXPECT_TRUE(input.hdu(1).diagnostics().empty());
        }
    }
}

// §7.3.2: TDIMn makes the field an array, the first dimension varying fastest, in fixed and variable fields.
TEST(conformance_tables, tdim_gives_the_dimensions_of_a_field)
{
    byte_writer row;
    row.values<std::int16_t>({1, 2, 3, 4, 5, 6});
    const reader input =
        open(with_table(with(bintable_records(12, 1, {"6I"}), {card_record("TDIM1", "'( 3, 2 )'")}), row));
    EXPECT_EQ(input.table(1).columns[0].dimensions, (std::vector<std::uint64_t>{3, 2}));
    EXPECT_TRUE(input.hdu(1).diagnostics().empty());
}

// Appendix L, the Green Bank convention: keywords expanded into columns, read as the binary table it is.
TEST(conformance_tables, a_table_of_the_green_bank_convention_is_a_binary_table)
{
    byte_writer rows;
    rows.text("2026-10-30T21:00:00", 19).value<double>(55.5);
    rows.text("2026-10-30T22:00:00", 19).value<double>(54.0);
    const reader input = open(with_table(with(bintable_records(27, 2, {"19A", "D"}),
                                              {card_record("TTYPE1", "'DATE-OBS'"), card_record("TTYPE2", "'HUMIDITY'"),
                                               card_record("TELESCOP", "'GBT'")}),
                                         rows));
    EXPECT_EQ(input.read_strings(1, "DATE-OBS"),
              (std::vector<std::string>{"2026-10-30T21:00:00", "2026-10-30T22:00:00"}));
    EXPECT_EQ(input.read_column<double>(1, "HUMIDITY"), (std::vector<double>{55.5, 54.0}));
    EXPECT_TRUE(input.hdu(1).diagnostics().empty());
}

// §6.1.1, Eq. 4: random groups are GCOUNT groups of PCOUNT parameters and an array; §6.1.2: PTYPEn, PSCALn, PZEROn.
TEST(conformance_tables, random_groups_are_groups_of_parameters_and_arrays)
{
    byte_writer data;
    data.values<std::int32_t>({1, 2, 10, 20, 30, 3, 4, 40, 50, 60});
    const reader input =
        open(fixture()
                 .header({card_record("SIMPLE", "T"), card_record("BITPIX", "32"), card_record("NAXIS", "2"),
                          card_record("NAXIS1", "0"), card_record("NAXIS2", "3"), card_record("GROUPS", "T"),
                          card_record("PCOUNT", "2"), card_record("GCOUNT", "2"), card_record("PTYPE1", "'A'"),
                          card_record("PTYPE2", "'A'"), card_record("PZERO2", "0.5")})
                 .data(data.data()));
    EXPECT_TRUE(input.hdu(0).diagnostics().empty());
    EXPECT_EQ(input.read_parameter<double>(0, "A"), (std::vector<double>{3.5, 7.5}));
    EXPECT_EQ(input.read_group<int>(0, 1), (std::vector<int>{40, 50, 60}));
}

} // namespace
