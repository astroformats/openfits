// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/table.h>
#include <openfits/writer.h>

#include "support/throws.h"
#include "support/written_file.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using openfits::column_arrays;
using openfits::column_format;
using openfits::column_info;
using openfits::column_storage;
using openfits::column_type;
using openfits::column_values;
using openfits::errc;
using openfits::hdu_kind;
using openfits::reader;
using openfits::table_info;
using openfits::usage_error;
using openfits::validation_error;
using openfits::writer;
using openfits::test::throws;

// The file that output writes, in memory.
std::vector<std::byte> saved(const writer& output, const openfits::write_options& options = {})
{
    openfits::memory_sink sink;
    output.save(sink, options);
    return sink.release();
}

reader read_back(const writer& output, std::string_view name = {})
{
    std::vector<std::byte> bytes = saved(output);
    if (!name.empty()) {
        openfits::test::record_file(bytes);
    }
    return reader(std::make_unique<openfits::memory_source>(std::move(bytes)), {.strict = true});
}

table_info binary_table(std::uint64_t rows, std::vector<column_info> columns)
{
    return table_info{.kind = hdu_kind::bintable, .rows = rows, .columns = std::move(columns)};
}

TEST(table_write, writes_every_numeric_type_in_its_own_type)
{
    const std::vector<std::int8_t> s8{-128, 0, 127};
    const std::vector<std::uint8_t> u8{0, 7, 255};
    const std::vector<std::int16_t> s16{-32768, 1, 32767};
    const std::vector<std::uint16_t> u16{0, 1, 65535};
    const std::vector<std::int32_t> s32{std::numeric_limits<std::int32_t>::min(), 2, 3};
    const std::vector<std::uint32_t> u32{0, 4, std::numeric_limits<std::uint32_t>::max()};
    const std::vector<std::int64_t> s64{std::numeric_limits<std::int64_t>::min(), 5, 6};
    const std::vector<std::uint64_t> u64{0, 7, std::numeric_limits<std::uint64_t>::max()};
    const std::vector<float> f32{1.5F, -0.0F, std::numeric_limits<float>::infinity()};
    const std::vector<double> f64{0.1, std::numeric_limits<double>::denorm_min(), -1e300};
    std::vector<column_info> columns;
    for (const char* name : {"S8", "U8", "S16", "U16", "S32", "U32", "S64", "U64", "F32", "F64"}) {
        columns.push_back({.name = name});
    }
    const std::vector<column_values> values{std::span(s8),  std::span(u8),  std::span(s16), std::span(u16),
                                            std::span(s32), std::span(u32), std::span(s64), std::span(u64),
                                            std::span(f32), std::span(f64)};
    writer output;
    EXPECT_EQ(output.add_table(binary_table(3, columns), values), 1U);
    EXPECT_EQ(output.hdu_count(), 2U);
    const reader input = read_back(output, "table-every-type.fits");
    const table_info table = input.table(1);
    // The conventions of standard §5.2.5 for the types that FITS stores with an offset (Table 19).
    EXPECT_EQ(table.columns[0].format.value_or(column_format{}).type, column_type::uint8);
    EXPECT_EQ(table.columns[0].scaling.zero, -128.0);
    EXPECT_EQ(table.columns[3].format.value_or(column_format{}).type, column_type::int16);
    EXPECT_EQ(table.columns[3].scaling.zero, 32768.0);
    EXPECT_EQ(table.columns[7].scaling.zero, 9223372036854775808.0);
    EXPECT_EQ(table.row_size, 1U + 1 + 2 + 2 + 4 + 4 + 8 + 8 + 4 + 8);
    EXPECT_EQ(input.read_column<std::int8_t>(1, "S8"), s8);
    EXPECT_EQ(input.read_column<std::uint8_t>(1, "U8"), u8);
    EXPECT_EQ(input.read_column<std::int16_t>(1, "S16"), s16);
    EXPECT_EQ(input.read_column<std::uint16_t>(1, "U16"), u16);
    EXPECT_EQ(input.read_column<std::int32_t>(1, "S32"), s32);
    EXPECT_EQ(input.read_column<std::uint32_t>(1, "U32"), u32);
    EXPECT_EQ(input.read_column<std::int64_t>(1, "S64"), s64);
    EXPECT_EQ(input.read_column<std::uint64_t>(1, "U64"), u64);
    const std::vector<float> floats = input.read_column<float>(1, "F32");
    EXPECT_EQ(floats[0], 1.5F);
    EXPECT_TRUE(std::signbit(floats[1]));
    EXPECT_EQ(floats[2], std::numeric_limits<float>::infinity());
    EXPECT_EQ(input.read_column<double>(1, "F64"), f64);
    for (std::size_t i = 0; i < table.columns.size(); ++i) {
        EXPECT_EQ(table.columns[i].value_type(), static_cast<openfits::numeric_type>(i)) << i;
    }
}

TEST(table_write, writes_the_structural_keywords_of_a_binary_table)
{
    // Standard §7.3.1, Table 17, then the keywords of each column.
    const std::vector<std::int32_t> counts{1, 2};
    const std::vector<double> fluxes{0.5, 1.5};
    writer output;
    table_info info = binary_table(2, {{.name = "COUNT", .unit = "ct"},
                                       {.name = "FLUX",
                                        .format = column_format::parse_binary("1E"),
                                        .scaling = {.scale = 2.0},
                                        .display = openfits::display_format::parse("F8.3")}});
    info.header.set("EXTNAME", "CATALOG");
    (void)output.add_table(std::move(info), std::vector<column_values>{std::span(counts), std::span(fluxes)});
    const std::vector<std::byte> bytes = saved(output);
    const std::string text(reinterpret_cast<const char*>(bytes.data()) + 2880, 2880);
    const std::vector<std::string> expected{"XTENSION= 'BINTABLE'           / binary table extension",
                                            "BITPIX  =                    8 / 8-bit bytes",
                                            "NAXIS   =                    2 / 2-dimensional table",
                                            "NAXIS1  =                    8 / bytes in a row",
                                            "NAXIS2  =                    2 / rows",
                                            "PCOUNT  =                    0 / bytes of the heap",
                                            "GCOUNT  =                    1 / one table",
                                            "TFIELDS =                    2 / columns",
                                            "TTYPE1  = 'COUNT   '           / name of column 1",
                                            "TFORM1  = '1J      '           / format of column 1",
                                            "TUNIT1  = 'ct      '           / unit of column 1",
                                            "TTYPE2  = 'FLUX    '           / name of column 2",
                                            "TFORM2  = '1E      '           / format of column 2",
                                            "TSCAL2  =                    2 / physical = TZERO + TSCAL x stored",
                                            "TDISP2  = 'F8.3    '           / display format of column 2",
                                            "EXTNAME = 'CATALOG '"};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        std::string record = expected[i];
        record.resize(80, ' ');
        EXPECT_EQ(text.substr(i * 80, 80), record) << i;
    }
    // The values written stored = value / TSCAL, read back as written.
    const reader input(std::make_unique<openfits::memory_source>(std::vector<std::byte>(bytes)));
    EXPECT_EQ(input.read_column<double>(1, "FLUX"), fluxes);
    EXPECT_EQ(input.read_column<float>(1, "FLUX", {.scaling = openfits::scaling_policy::raw}),
              (std::vector<float>{0.25F, 0.75F}));
}

TEST(table_write, writes_complex_numbers_logicals_and_bits)
{
    // Not const, as column_values takes complex numbers that are not const too.
    std::vector<std::complex<float>> c{{1.0F, 2.0F}, {-3.0F, 0.5F}};
    std::vector<std::complex<double>> m{{1e100, -1.0}, {0.0, 1e-300}};
    const std::vector<std::uint8_t> flags{1, 0, 5, 0};
    const std::vector<std::uint8_t> undefined{0, 0, 0, 1};
    std::vector<std::uint8_t> bits(22);
    for (std::size_t i = 0; i < bits.size(); ++i) {
        bits[i] = static_cast<std::uint8_t>((i * 7) % 3 == 0 ? 1 : 0);
    }
    writer output;
    (void)output.add_table(
        binary_table(
            2,
            {{.name = "C"}, {.name = "M"}, {.name = "FLAGS", .dimensions = {2}}, {.name = "BITS", .dimensions = {11}}}),
        std::vector<column_values>{std::span(c), std::span(m), column_values::logicals(flags, undefined),
                                   column_values::bits(bits)});
    const reader input = read_back(output, "table-complex-logical-bits.fits");
    const table_info table = input.table(1);
    EXPECT_EQ(table.columns[2].format, (column_format{.type = column_type::logical, .repeat = 2}));
    EXPECT_EQ(table.columns[3].format, (column_format{.type = column_type::bit, .repeat = 11}));
    EXPECT_EQ(table.columns[3].format.value_or(column_format{}).field_size(), 2U);
    EXPECT_EQ(input.read_column<std::complex<float>>(1, 0), c);
    EXPECT_EQ(input.read_column<std::complex<double>>(1, 1), m);
    std::vector<std::uint8_t> mask(4);
    std::vector<std::uint8_t> read_flags(4);
    (void)input.read_column(1, 2, std::span<std::uint8_t>(read_flags),
                            {.blank = openfits::blank_policy::mask, .mask = std::span<std::uint8_t>(mask)});
    EXPECT_EQ(read_flags, (std::vector<std::uint8_t>{1, 0, 1, 0}));
    EXPECT_EQ(mask, undefined);
    EXPECT_EQ(input.read_column<std::uint8_t>(1, 3), bits);
}

TEST(table_write, writes_strings_padded_with_spaces)
{
    // Not const, as column_values takes strings that are not const too.
    std::vector<std::string> names{"M 31", "", "NGC 6992"};
    writer output;
    (void)output.add_table(binary_table(3, {{.name = "NAME"}}), std::vector<column_values>{std::span(names)});
    const reader input = read_back(output, "table-strings.fits");
    EXPECT_EQ(input.table(1).columns[0].format, (column_format{.type = column_type::character, .repeat = 8}));
    EXPECT_EQ(input.read_strings(1, 0), (std::vector<std::string>{"M 31    ", "        ", "NGC 6992"}));
    EXPECT_EQ(input.read_strings(1, 0, {.trim_strings = true}), names);
}

TEST(table_write, writes_substrings_with_tdim_or_the_raw_convention)
{
    const std::vector<std::string> words{"one", "two", "three", "four", "five", "six"};
    writer output;
    (void)output.add_table(binary_table(2, {{.name = "WORDS", .dimensions = {5, 3}}}),
                           std::vector<column_values>{std::span(words)});
    (void)output.add_table(
        binary_table(2, {{.name = "WORDS",
                          .format = column_format{.type = column_type::character, .repeat = 15, .substring_width = 5},
                          .dimensions = {5, 3}}}),
        std::vector<column_values>{std::span(words)});
    const reader input = read_back(output, "table-substrings.fits");
    EXPECT_EQ(input.hdu(1).header().value<std::string>("TFORM1"), "15A");
    EXPECT_EQ(input.hdu(1).header().value<std::string>("TDIM1"), "(5,3)");
    EXPECT_EQ(input.hdu(2).header().value<std::string>("TFORM1"), "15A5");
    EXPECT_FALSE(input.hdu(2).header().contains("TDIM1"));
    for (const std::size_t unit : {1U, 2U}) {
        EXPECT_EQ(input.read_strings(unit, 0, {.trim_strings = true}), words) << unit;
    }
}

TEST(table_write, writes_undefined_values_as_tnull)
{
    const std::vector<double> values{1.0, std::numeric_limits<double>::quiet_NaN(), 3.0};
    writer output;
    (void)output.add_table(binary_table(3, {{.name = "N", .format = column_format::parse_binary("1J"), .null = -999}}),
                           std::vector<column_values>{std::span(values)});
    const reader input = read_back(output);
    EXPECT_EQ(input.read_column<std::int32_t>(1, 0, {.scaling = openfits::scaling_policy::raw}),
              (std::vector<std::int32_t>{1, -999, 3}));
    const std::vector<double> back = input.read_column<double>(1, 0);
    EXPECT_TRUE(std::isnan(back[1]));
    // A NaN into an integer column without TNULLn is refused when the file is saved.
    writer refused;
    (void)refused.add_table(binary_table(3, {{.name = "N", .format = column_format::parse_binary("1J")}}),
                            std::vector<column_values>{std::span(values)});
    EXPECT_TRUE(throws<openfits::invalid_data_error>(errc::undefined_value, [&] { (void)saved(refused); }));
}

TEST(table_write, writes_variable_length_arrays_in_a_compact_heap)
{
    // Not const, as arrays() takes them too.
    std::vector<std::int16_t> elements{1, 2, 3, 4, 5, 6};
    const std::vector<std::uint64_t> offsets{0, 3, 3, 6};
    const std::vector<std::string> notes{"first", "", "the third"};
    const std::vector<std::complex<double>> pairs{{1, 2}, {3, 4}};
    const std::vector<std::uint64_t> pair_offsets{0, 1, 1, 2};
    writer output;
    (void)output.add_table(binary_table(3, {{.name = "VALUES"}, {.name = "NOTES"}, {.name = "PAIRS"}}),
                           std::vector<column_values>{column_values::arrays(std::span(elements), offsets),
                                                      column_values::arrays(std::span(notes)),
                                                      column_values::arrays(std::span(pairs), pair_offsets)});
    const reader input = read_back(output, "table-variable-length.fits");
    const table_info table = input.table(1);
    // Standard §7.3.5: P descriptors and emax; the heap after the rows, its size PCOUNT, THEAP not needed.
    EXPECT_EQ(input.hdu(1).header().value<std::string>("TFORM1"), "1PI(3)");
    EXPECT_EQ(input.hdu(1).header().value<std::string>("TFORM2"), "1PA(9)");
    EXPECT_EQ(input.hdu(1).header().value<std::string>("TFORM3"), "1PM(1)");
    EXPECT_FALSE(input.hdu(1).header().contains("THEAP"));
    // The empty note is one NUL byte, as CFITSIO writes it, and reads back empty.
    EXPECT_EQ(table.heap_size, (6 * 2) + 15 + (2 * 16));
    // The empty array of row 1 has the offset 0 that the standard asks for (§7.3.5): its descriptor in bytes 24 to 31.
    std::array<std::byte, 8> descriptor{};
    input.read_raw(1, 24, descriptor);
    EXPECT_EQ(descriptor, (std::array<std::byte, 8>{}));
    EXPECT_EQ(table.row_size, 24U);
    const column_arrays<std::int16_t> arrays = input.read_arrays<std::int16_t>(1, 0);
    EXPECT_EQ(arrays.values, elements);
    EXPECT_EQ(arrays.offsets, offsets);
    EXPECT_EQ(input.read_strings(1, 1), notes);
    const column_arrays<std::complex<double>> complex_arrays = input.read_arrays<std::complex<double>>(1, 2);
    EXPECT_EQ(complex_arrays.values, pairs);
    EXPECT_EQ(complex_arrays.offsets, pair_offsets);
}

TEST(table_write, writes_ascii_tables)
{
    // Standard §7.2: one space between the fields, numbers right-justified in theirs, strings left-justified.
    const std::vector<std::string> names{"M 31", "M 33"};
    const std::vector<std::int32_t> counts{42, -17};
    const std::vector<float> floats{0.1F, -3.5e30F};
    const std::vector<double> doubles{1.0 / 3.0, -1e300};
    const std::vector<double> fixed{12.3456, std::numeric_limits<double>::quiet_NaN()};
    writer output;
    (void)output.add_table(
        table_info{.kind = hdu_kind::table,
                   .rows = 2,
                   .columns = {{.name = "NAME"},
                               {.name = "COUNT"},
                               {.name = "F"},
                               {.name = "D"},
                               {.name = "X", .format = column_format::parse_ascii("F8.2"), .null_string = "NULL"}}},
        std::vector<column_values>{std::span(names), std::span(counts), std::span(floats), std::span(doubles),
                                   std::span(fixed)});
    const std::vector<std::byte> bytes = saved(output);
    openfits::test::record_file(bytes);
    const reader input(std::make_unique<openfits::memory_source>(std::vector<std::byte>(bytes)), {.strict = true});
    const table_info table = input.table(1);
    EXPECT_EQ(table.kind, hdu_kind::table);
    EXPECT_EQ(table.columns[0].format, column_format::parse_ascii("A4"));
    EXPECT_EQ(table.columns[1].format, column_format::parse_ascii("I11"));
    EXPECT_EQ(table.columns[2].format, column_format::parse_ascii("E16.9"));
    EXPECT_EQ(table.columns[3].format, column_format::parse_ascii("D25.17"));
    EXPECT_EQ(table.columns[1].offset, 5U);
    EXPECT_EQ(table.row_size, 4U + 1 + 11 + 1 + 16 + 1 + 25 + 1 + 8);
    EXPECT_EQ(input.read_strings(1, 0), names);
    EXPECT_EQ(input.read_column<std::int32_t>(1, 1), counts);
    EXPECT_EQ(input.read_column<float>(1, 2), floats);
    EXPECT_EQ(input.read_column<double>(1, 3), doubles);
    const std::vector<double> back = input.read_column<double>(1, 4);
    EXPECT_EQ(back[0], 12.35);
    EXPECT_TRUE(std::isnan(back[1]));
    // The fields as written: a three-digit exponent keeps its letter where the field has room.
    // The rows after the two blocks of the headers.
    const std::string row(reinterpret_cast<const char*>(bytes.data()) + std::size_t{5760}, table.row_size * 2);
    EXPECT_EQ(row.substr(0, table.row_size), "M 31          42  0.100000001E+00   0.33333333333333331D+00    12.35");
    EXPECT_EQ(row.substr(table.row_size), "M 33         -17 -0.350000013E+31 -0.10000000000000001D+301 NULL    ");
    // The fill of the last block is spaces (standard §7.2.3).
    EXPECT_EQ(static_cast<char>(bytes.back()), ' ');
}

TEST(table_write, writes_stored_values_as_they_are_under_the_scaling_of_their_columns)
{
    // The stored values of a binary and of an ASCII column, with the TSCALn and TZEROn that a read applies.
    const std::vector<std::int32_t> codes{-3, 0, 7};
    const openfits::scaling scaled{.scale = 0.5, .zero = 10.0};
    writer output;
    (void)output.add_table(
        binary_table(3, {{.name = "J", .format = column_format::parse_binary("1J"), .scaling = scaled}}),
        std::vector<column_values>{std::span(codes)}, {.scaling = openfits::scaling_policy::raw});
    (void)output.add_table(
        table_info{.kind = hdu_kind::table,
                   .rows = 3,
                   .columns = {{.name = "I", .format = column_format::parse_ascii("I4"), .scaling = scaled}}},
        std::vector<column_values>{std::span(codes)}, {.scaling = openfits::scaling_policy::raw});
    const reader input = read_back(output, "table-stored-values.fits");
    for (const std::size_t unit : {1U, 2U}) {
        EXPECT_EQ(input.table(unit).columns[0].scaling, scaled) << unit;
        EXPECT_EQ(input.read_column<std::int32_t>(unit, 0, {.scaling = openfits::scaling_policy::raw}), codes) << unit;
        EXPECT_EQ(input.read_column<double>(unit, 0), (std::vector<double>{8.5, 10.0, 13.5})) << unit;
    }
    // Stored values are written into a format, which a column of numbers must have.
    writer refusing;
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] {
        (void)refusing.add_table(binary_table(3, {{.name = "J"}}), std::vector<column_values>{std::span(codes)},
                                 {.scaling = openfits::scaling_policy::raw});
    }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] {
        (void)refusing.add_table(binary_table(3, {{.name = "J", .format = column_format::parse_binary("1J")}}),
                                 std::vector<column_values>{std::span(codes)},
                                 {.scaling = openfits::scaling_policy::override});
    }));
}

TEST(table_write, refuses_a_value_beyond_its_ascii_field)
{
    const std::vector<std::int32_t> counts{12345};
    writer output;
    (void)output.add_table(
        table_info{.kind = hdu_kind::table, .rows = 1, .columns = {{.format = column_format::parse_ascii("I3")}}},
        std::vector<column_values>{std::span(counts)});
    EXPECT_TRUE(throws<openfits::invalid_data_error>(errc::value_out_of_range, [&] { (void)saved(output); }));
}

TEST(table_write, refuses_models_that_the_standard_forbids)
{
    const std::vector<std::string> names{"a"};
    const std::vector<float> floats{1.0F};
    const std::vector<std::int16_t> shorts{1};
    const auto refused = [](table_info info, std::vector<column_values> values, errc code) {
        writer output;
        return throws<validation_error>(code, [&] { (void)output.add_table(std::move(info), values); });
    };
    EXPECT_TRUE(refused(binary_table(1, {{.format = column_format::parse_binary("1A"), .scaling = {.scale = 2.0}}}),
                        {std::span(names)}, errc::reserved_keyword));
    EXPECT_TRUE(refused(binary_table(1, {{.format = column_format::parse_binary("1E"), .null = 0}}),
                        {std::span(floats)}, errc::reserved_keyword));
    EXPECT_TRUE(refused(binary_table(1, {{.format = column_format::parse_binary("1I"), .null = 40000}}),
                        {std::span(shorts)}, errc::reserved_keyword));
    EXPECT_TRUE(refused(binary_table(1, {{.format = column_format::parse_binary("2I"), .dimensions = {3}}}),
                        {std::span(shorts)}, errc::invalid_dimensions));
    EXPECT_TRUE(refused(binary_table(1, {{.format = column_format::parse_binary("1I"),
                                          .display = openfits::display_format::parse("A4")}}),
                        {std::span(shorts)}, errc::invalid_display_format));
    EXPECT_TRUE(refused(binary_table(1, {{.format = column_format::parse_ascii("I4")}}), {std::span(shorts)},
                        errc::invalid_column_format));
    EXPECT_TRUE(refused(binary_table(1, {{.format = column_format::parse_binary("1E"), .scaling = {.scale = 0.0}}}),
                        {std::span(floats)}, errc::reserved_keyword));
    std::vector<column_info> many(1000);
    std::vector<column_values> many_values(1000, column_values(std::span(shorts)));
    EXPECT_TRUE(refused(binary_table(1, many), many_values, errc::invalid_structure));
}

TEST(table_write, refuses_values_that_do_not_fit_the_model)
{
    const std::vector<std::int16_t> shorts{1, 2, 3};
    const std::vector<std::string> names{"too long"};
    const std::vector<std::string> control{"a\tb"};
    writer output;
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] {
        (void)output.add_table(binary_table(2, {{}}), std::vector<column_values>{std::span(shorts)});
    }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] {
        (void)output.add_table(binary_table(1, {{}, {}}), std::vector<column_values>{std::span(shorts)});
    }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] {
        (void)output.add_table(binary_table(1, {{.format = column_format::parse_binary("4A")}}),
                               std::vector<column_values>{std::span(names)});
    }));
    EXPECT_TRUE(throws<validation_error>(errc::invalid_character, [&] {
        (void)output.add_table(binary_table(1, {{}}), std::vector<column_values>{std::span(control)});
    }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] {
        (void)output.add_table(binary_table(3, {{.scaling = {.scale = 2.0}}}),
                               std::vector<column_values>{std::span(shorts)});
    }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] {
        (void)output.add_table(binary_table(3, {{.format = column_format::parse_binary("1PI")}}),
                               std::vector<column_values>{std::span(shorts)});
    }));
    const std::vector<std::uint64_t> bad_offsets{0, 2, 1, 3};
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] {
        (void)output.add_table(binary_table(3, {{}}),
                               std::vector<column_values>{column_values::arrays(std::span(shorts), bad_offsets)});
    }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] {
        (void)output.add_table(table_info{.kind = hdu_kind::image}, std::vector<column_values>{});
    }));
    EXPECT_EQ(output.hdu_count(), 1U);
}

TEST(table_write, the_header_of_a_table_holds_no_keyword_of_its_columns)
{
    const std::vector<std::int16_t> shorts{1};
    table_info info = binary_table(1, {{.name = "A"}});
    info.header.set("TTYPE1", "B");
    writer output;
    (void)output.add_table(std::move(info), std::vector<column_values>{std::span(shorts)});
    EXPECT_TRUE(throws<validation_error>(errc::structural_keyword, [&] { (void)saved(output); }));
}

TEST(table_write, writes_tables_of_no_row_and_of_no_column)
{
    const std::vector<double> none;
    writer output;
    (void)output.add_table(binary_table(0, {{.name = "EMPTY"}}), std::vector<column_values>{std::span(none)});
    (void)output.add_table(binary_table(0, {}), std::vector<column_values>{});
    const reader input = read_back(output);
    EXPECT_EQ(input.table(1).rows, 0U);
    EXPECT_TRUE(input.read_column<double>(1, 0).empty());
    EXPECT_EQ(input.table(2).columns.size(), 0U);
    EXPECT_EQ(input.hdu(2).data_size(), 0U);
}

TEST(table_write, writes_the_same_bytes_for_the_same_model)
{
    const std::vector<std::uint16_t> values{1, 2, 3};
    const std::vector<std::string> names{"a", "b", "c"};
    const auto make = [&] {
        writer output;
        (void)output.add_table(binary_table(3, {{.name = "V"}, {.name = "N"}}),
                               std::vector<column_values>{std::span(values), std::span(names)});
        return saved(output);
    };
    EXPECT_EQ(make(), make());
}

} // namespace
