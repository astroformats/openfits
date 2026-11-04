// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The conformance report of a file: the diagnostics of the walk with their sections, and the checks that reads do not
// make, each with the section of the standard that it departs from. The tolerance table finds each of its rows in the
// report (conformance/tolerance.cpp).

#include <openfits/error.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/verify.h>
#include <openfits/writer.h>

#include "support/bytes.h"
#include "support/checksums.h"
#include "support/compressed_fixtures.h"
#include "support/files.h"
#include "support/fixture_builder.h"
#include "support/table_fixtures.h"
#include "support/temp_directory.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using openfits::diagnostic;
using openfits::errc;
using openfits::severity;
using openfits::test::ascii_field;
using openfits::test::ascii_table_records;
using openfits::test::bintable_records;
using openfits::test::byte_writer;
using openfits::test::card_record;
using openfits::test::fixture;
using openfits::test::primary_records;
using openfits::test::with;

std::vector<diagnostic> verify_bytes(const std::vector<std::byte>& file)
{
    const openfits::reader input(std::make_unique<openfits::memory_source>(std::vector<std::byte>(file)));
    return openfits::verify(input);
}

std::string listed(const std::vector<diagnostic>& findings)
{
    std::string text;
    for (const diagnostic& finding : findings) {
        text += '[' + finding.section + "] " + finding.message + '\n';
    }
    return text;
}

// The findings of a code.
std::vector<diagnostic> of_code(const std::vector<diagnostic>& findings, errc code)
{
    std::vector<diagnostic> found;
    std::ranges::copy_if(findings, std::back_inserter(found),
                         [code](const diagnostic& entry) { return entry.code == code; });
    return found;
}

// The data of an ASCII table: its rows, then spaces to the end of the block (standard §7.2.3).
std::vector<std::byte> ascii_rows(std::string_view rows)
{
    std::vector<std::byte> data;
    for (const char c : rows) {
        data.push_back(static_cast<std::byte>(c));
    }
    while (data.size() % openfits::test::block_bytes != 0) {
        data.push_back(std::byte{' '});
    }
    return data;
}

TEST(verify, a_file_of_every_kind_of_unit_that_the_writer_writes_has_no_findings)
{
    std::vector<std::int16_t> pixels(std::size_t{37} * 23);
    std::vector<float> reals(pixels.size());
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<std::int16_t>((i * 31) % 2000);
        reals[i] = static_cast<float>(i) / 7.0F;
    }
    const std::vector<std::int32_t> ids{1, -2, 3, -4};
    const std::vector<std::string> names{"a", "bb", "ccc", ""};
    const std::vector<double> elements{1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
    const std::vector<std::uint64_t> offsets{0, 1, 3, 3, 6};
    const std::vector<std::uint8_t> flags{1, 0, 1, 0};

    openfits::writer output;
    openfits::image_info image;
    image.geometry = {.axes = {37, 23}};
    image.header.set("OBJECT", "the pattern");
    (void)output.add_image(image, std::span(pixels));
    image.header.set("EXTNAME", "REALS");
    (void)output.add_image(image, std::span(reals));
    (void)output.add_image(
        image, std::span(reals),
        {.compression = openfits::compression_options{.type = openfits::compression::rice, .quantize_level = 4.0}});
    const openfits::table_info binary{
        .kind = openfits::hdu_kind::bintable,
        .rows = 4,
        .columns = {{.name = "ID", .format = openfits::column_format::parse_binary("1J")},
                    {.name = "NAME", .format = openfits::column_format::parse_binary("3A")},
                    {.name = "FLAG", .format = openfits::column_format::parse_binary("1L")},
                    {.name = "SPECTRUM", .format = openfits::column_format::parse_binary("1PD")}}};
    const std::vector<openfits::column_values> binary_values{
        std::span(ids), std::span(names), openfits::column_values::logicals(std::span(flags)),
        openfits::column_values::arrays(std::span(elements), offsets)};
    (void)output.add_table(binary, binary_values);
    (void)output.add_table(binary, binary_values,
                           {.compression = openfits::compression_options{.type = openfits::compression::gzip_2}});
    const openfits::table_info ascii{
        .kind = openfits::hdu_kind::table,
        .rows = 4,
        .columns = {{.name = "NAME", .format = openfits::column_format::parse_ascii("A4")},
                    {.name = "ID", .format = openfits::column_format::parse_ascii("I6")},
                    {.name = "VALUE", .format = openfits::column_format::parse_ascii("E14.6")}}};
    const std::vector<double> values{0.5, -1.25e-30, 3e10, 0.0};
    (void)output.add_table(ascii,
                           std::vector<openfits::column_values>{std::span(names), std::span(ids), std::span(values)});

    for (const bool checksums : {false, true}) {
        openfits::memory_sink sink;
        output.save(sink, {.checksums = checksums});
        const std::vector<diagnostic> findings = verify_bytes(sink.release());
        EXPECT_TRUE(findings.empty()) << listed(findings);
    }
}

TEST(verify, reports_the_diagnostics_of_the_walk_unit_by_unit_with_their_sections)
{
    const fixture file = fixture()
                             .header(with(primary_records(), {card_record("exptime", "1.0")}))
                             .image({2}, {card_record("FLAG", "t")})
                             .append(std::string(2880, 'x'));
    const std::vector<diagnostic> findings = verify_bytes(file.bytes());
    ASSERT_EQ(findings.size(), 3U) << listed(findings);
    EXPECT_EQ(findings[0].code, errc::invalid_keyword);
    EXPECT_EQ(findings[0].section, "4.1.2.1");
    EXPECT_EQ(findings[0].context.hdu, 0U);
    EXPECT_EQ(findings[1].code, errc::invalid_logical);
    EXPECT_EQ(findings[1].section, "4.2.2");
    EXPECT_EQ(findings[1].context.hdu, 1U);
    EXPECT_EQ(findings[2].code, errc::special_records);
    EXPECT_EQ(findings[2].section, "3.5");
    EXPECT_EQ(findings[2].context.hdu, std::nullopt);
}

TEST(verify, reports_a_fill_of_data_that_is_not_zeros_or_spaces_in_an_ascii_table)
{
    // Three 16-bit values, then the fill, whose 101st byte is not a zero.
    std::vector<std::byte> block(openfits::test::block_bytes);
    block[100] = std::byte{'x'};
    const fixture image = fixture().header(primary_records(16, {3})).data(block);
    std::vector<diagnostic> findings = verify_bytes(image.bytes());
    ASSERT_EQ(findings.size(), 1U) << listed(findings);
    EXPECT_EQ(findings[0].severity, severity::info);
    EXPECT_EQ(findings[0].code, errc::data_fill);
    EXPECT_EQ(findings[0].section, "3.3.2");
    EXPECT_EQ(findings[0].context.offset, 2880U + 100U);

    // The zeros of a fixture are not the spaces of an ASCII table.
    fixture table;
    table.primary().header(ascii_table_records(4, 1, {{.column = 1, .format = "I4"}})).data(4);
    findings = verify_bytes(table.bytes());
    ASSERT_EQ(of_code(findings, errc::data_fill).size(), 1U) << listed(findings);
    EXPECT_EQ(of_code(findings, errc::data_fill)[0].section, "7.2.3");
}

TEST(verify, parses_every_field_of_an_ascii_table_by_its_format)
{
    // Rows of an I6 field, an F8.2 field and an A3 field, with TNULL1 = '*'.
    const std::string rows = "    12    1.50abc" // conforming
                             "  1 23    2.50abc" // spaces inside an integer
                             "   -45  1.5e+2abc" // a lowercase exponent
                             "    67     150abc" // an implied decimal point
                             "   abc    1.00abc" // not a number
                             "*         1.00a\x01"
                             "c"                  // a null field, and a byte outside ASCII text
                             "          1.00   "; // blank fields
    fixture file;
    file.primary()
        .header(
            with(ascii_table_records(
                     17, 7,
                     {{.column = 1, .format = "I6"}, {.column = 7, .format = "F8.2"}, {.column = 15, .format = "A3"}}),
                 {card_record("TNULL1", "'*'")}))
        .data(ascii_rows(rows));
    const std::vector<diagnostic> findings = verify_bytes(file.bytes());
    ASSERT_EQ(findings.size(), 5U) << listed(findings);
    for (const diagnostic& finding : findings) {
        EXPECT_EQ(finding.section, "7.2.5");
        EXPECT_EQ(finding.context.hdu, 1U);
    }
    EXPECT_EQ(findings[0].code, errc::invalid_field);
    EXPECT_EQ(findings[0].severity, severity::warning);
    EXPECT_EQ(findings[0].context.column, 1U);
    EXPECT_EQ(findings[0].context.offset, 2 * 2880U + 17U);
    EXPECT_EQ(findings[1].severity, severity::warning);
    EXPECT_EQ(findings[1].context.column, 2U);
    EXPECT_EQ(findings[2].severity, severity::info);
    EXPECT_EQ(findings[3].severity, severity::error);
    EXPECT_EQ(findings[3].context.column, 1U);
    EXPECT_EQ(findings[4].code, errc::invalid_character);
    EXPECT_EQ(findings[4].context.column, 3U);
}

TEST(verify, lists_ten_findings_of_a_problem_and_counts_the_others_in_one)
{
    std::string rows;
    for (int row = 0; row < 25; ++row) {
        rows += "  x";
    }
    fixture file;
    file.primary().header(ascii_table_records(3, 25, {{.column = 1, .format = "I3"}})).data(ascii_rows(rows));
    const std::vector<diagnostic> findings = verify_bytes(file.bytes());
    ASSERT_EQ(findings.size(), 11U) << listed(findings);
    EXPECT_EQ(findings[9].context.offset, 2 * 2880U + (9 * 3U));
    EXPECT_EQ(findings[10].severity, severity::error);
    EXPECT_EQ(findings[10].message, "15 more fields of the column that are not numbers, not listed");
    EXPECT_EQ(findings[10].context.column, 1U);
    EXPECT_EQ(findings[10].context.offset, std::nullopt);
}

TEST(verify, checks_the_logicals_and_the_characters_of_binary_tables)
{
    const std::vector<std::byte> row_data =
        byte_writer().text("TF", 2).text("ab", 3, '\0').bytes({0, 'X'}).bytes({'a', 0x80, 0}).data();
    fixture file;
    file.primary().header(bintable_records(5, 2, {"2L", "3A"})).data(row_data);
    const std::vector<diagnostic> findings = verify_bytes(file.bytes());
    ASSERT_EQ(findings.size(), 2U) << listed(findings);
    EXPECT_EQ(findings[0].code, errc::invalid_field);
    EXPECT_EQ(findings[0].severity, severity::error);
    EXPECT_EQ(findings[0].section, "7.3.3.1");
    EXPECT_EQ(findings[0].context.column, 1U);
    EXPECT_EQ(findings[0].context.offset, 2 * 2880U + 5U + 1U);
    EXPECT_EQ(findings[1].code, errc::invalid_character);
    EXPECT_EQ(findings[1].severity, severity::warning);
    EXPECT_EQ(findings[1].context.column, 2U);
    EXPECT_EQ(findings[1].context.offset, 2 * 2880U + 5U + 2U + 1U);
}

TEST(verify, checks_every_descriptor_against_the_heap_and_emax)
{
    // Three rows of a P descriptor of 16-bit integers with emax 2, and a Q descriptor of bits, over a heap of 8 bytes.
    byte_writer rows;
    rows.value<std::uint32_t>(2).value<std::uint32_t>(0).value<std::uint64_t>(9).value<std::uint64_t>(6);
    rows.value<std::uint32_t>(3).value<std::uint32_t>(2).value<std::uint64_t>(17).value<std::uint64_t>(6);
    rows.value<std::uint32_t>(1).value<std::uint32_t>(7).value<std::int64_t>(-1).value<std::uint64_t>(0);
    std::vector<std::byte> data = rows.data();
    data.resize(data.size() + 8);
    fixture file;
    file.primary().header(bintable_records(24, 3, {"1PI(2)", "1QX"}, 8)).data(data);
    const std::vector<diagnostic> findings = verify_bytes(file.bytes());
    ASSERT_EQ(findings.size(), 4U) << listed(findings);
    for (const diagnostic& finding : findings) {
        EXPECT_EQ(finding.code, errc::invalid_descriptor);
        EXPECT_EQ(finding.section, "7.3.5");
    }
    // Row 2: three elements beyond emax 2, and 3 bytes of bits at byte 6 beyond the heap.
    EXPECT_EQ(findings[0].severity, severity::warning);
    EXPECT_EQ(findings[0].context.column, 1U);
    EXPECT_EQ(findings[1].severity, severity::error);
    EXPECT_EQ(findings[1].context.column, 2U);
    // Row 3: 2 bytes at byte 7, and a negative count.
    EXPECT_EQ(findings[2].severity, severity::error);
    EXPECT_EQ(findings[2].context.column, 1U);
    EXPECT_EQ(findings[3].context.column, 2U);
}

TEST(verify, verifies_the_checksums_of_every_unit_that_has_them)
{
    const std::vector<std::byte> pixels = openfits::test::big_endian(std::vector<std::int16_t>{1, 2, 3});
    const fixture file = fixture()
                             .header(with(primary_records(16, {3}), {card_record("DATASUM", "'12'"),
                                                                     card_record("CHECKSUM", "'0000000000000000'")}))
                             .data(pixels);
    // The walk warns of the header part of CHECKSUM, and the verification finds both sums wrong.
    std::vector<diagnostic> findings = of_code(verify_bytes(file.bytes()), errc::checksum_mismatch);
    std::erase_if(findings, [](const diagnostic& entry) { return entry.severity != severity::error; });
    ASSERT_EQ(findings.size(), 2U);
    EXPECT_EQ(findings[0].context.keyword, "DATASUM");
    EXPECT_EQ(findings[0].severity, severity::error);
    EXPECT_EQ(findings[0].section, "4.4.2.7");
    EXPECT_EQ(findings[1].context.keyword, "CHECKSUM");
}

TEST(verify, reports_units_of_one_type_with_the_same_name_version_and_level)
{
    const auto named = [](std::string_view name, int version) {
        return std::vector<std::string>{card_record("EXTNAME", "'" + std::string(name) + "'"),
                                        card_record("EXTVER", std::to_string(version))};
    };
    fixture file;
    file.primary({card_record("EXTNAME", "'SCI'")})
        .image({1}, named("SCI", 1))
        .image({1}, named("sci", 2))
        .image({1}, named("SCI ", 2))
        .header(with(bintable_records(0, 0, {}), named("SCI", 1)));
    const std::vector<diagnostic> findings = verify_bytes(file.bytes());
    ASSERT_EQ(findings.size(), 2U) << listed(findings);
    EXPECT_EQ(findings[0].code, errc::repeated_hdu_name);
    EXPECT_EQ(findings[0].severity, severity::warning);
    EXPECT_EQ(findings[0].section, "4.4.2.6");
    EXPECT_EQ(findings[0].context.hdu, 1U);
    EXPECT_EQ(findings[1].context.hdu, 3U);
}

TEST(verify, reports_the_descriptions_that_reads_refuse)
{
    fixture file;
    file.primary();
    const std::vector<std::string> keywords = openfits::test::image_keywords(16, {4, 2}, {4, 1}, "NOT_A_CODEC");
    openfits::test::binary_table_unit(file, {{"COMPRESSED_DATA", "1PB"}},
                                      {openfits::test::p_descriptor(0, 0), openfits::test::p_descriptor(0, 0)}, {},
                                      keywords);
    const std::vector<diagnostic> findings = verify_bytes(file.bytes());
    ASSERT_EQ(findings.size(), 1U) << listed(findings);
    EXPECT_EQ(findings[0].severity, severity::error);
    EXPECT_EQ(findings[0].code, errc::unsupported_compression);
    EXPECT_EQ(findings[0].section, "10.4");
    EXPECT_EQ(findings[0].context.hdu, 1U);
    EXPECT_EQ(findings[0].message.find(" (HDU"), std::string::npos) << findings[0].message;
}

TEST(verify, of_a_path_reports_a_file_that_cannot_be_read_as_fits_alone)
{
    const openfits::test::temp_directory directory;
    openfits::test::write_file(directory.path() / "text.fits", openfits::test::bytes("not a FITS file at all"));
    std::vector<diagnostic> findings = openfits::verify(directory.file("text.fits"));
    ASSERT_EQ(findings.size(), 1U);
    EXPECT_EQ(findings[0].severity, severity::error);
    EXPECT_EQ(findings[0].code, errc::not_a_fits_file);
    EXPECT_EQ(findings[0].section, "4.4.1.1");

    std::vector<std::byte> stream = openfits::test::gzip(fixture().primary().bytes());
    stream.resize(stream.size() - 1);
    openfits::test::write_file(directory.path() / "cut.fits.gz", stream);
    findings = openfits::verify(directory.file("cut.fits.gz"));
    ASSERT_EQ(findings.size(), 1U);
    EXPECT_EQ(findings[0].code, errc::corrupt_compressed_data);

    openfits::test::write_file(directory.path() / "image.fits", fixture().primary().image({3}).bytes());
    EXPECT_TRUE(openfits::verify(directory.file("image.fits"), {.strict = true}).empty());
    EXPECT_TRUE(openfits::test::throws<openfits::io_error>(
        errc::open_failed, [&] { (void)openfits::verify(directory.file("missing.fits")); }));
    EXPECT_TRUE(openfits::test::throws<openfits::usage_error>(errc::invalid_argument,
                                                              [&] { (void)openfits::verify(std::string_view()); }));
}

TEST(verify, reports_its_progress_over_the_bytes_of_the_file_and_can_be_cancelled)
{
    const std::vector<std::byte> pixels = openfits::test::big_endian(std::vector<std::int16_t>(5000, 7));
    const fixture file =
        fixture().header(with(primary_records(16, {5000}), {card_record("DATASUM", "'0'")})).data(pixels).image({100});
    const openfits::reader input(std::make_unique<openfits::memory_source>(std::vector<std::byte>(file.bytes())));
    std::vector<std::uint64_t> reported;
    (void)openfits::verify(input, {.progress = [&reported](std::uint64_t done, std::uint64_t total) {
                               EXPECT_EQ(total, 7U * 2880U);
                               reported.push_back(done);
                               return true;
                           }});
    ASSERT_GE(reported.size(), 3U);
    EXPECT_EQ(reported.front(), 0U);
    EXPECT_EQ(reported.back(), 7U * 2880U);
    EXPECT_TRUE(std::ranges::is_sorted(reported));
    EXPECT_TRUE(openfits::test::throws<openfits::cancelled_error>(errc::cancelled, [&] {
        (void)openfits::verify(input, {.progress = [](std::uint64_t done, std::uint64_t) { return done == 0; }});
    }));
}

TEST(verify, reads_the_present_part_of_a_truncated_file)
{
    fixture table;
    table.primary().header(ascii_table_records(3, 2000, {{.column = 1, .format = "I3"}}));
    table.append(std::string(std::size_t{3} * 1000, '7'));
    const std::vector<diagnostic> findings = verify_bytes(table.bytes());
    ASSERT_EQ(findings.size(), 1U) << listed(findings);
    EXPECT_EQ(findings[0].code, errc::truncated_file);
}

} // namespace
