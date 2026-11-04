// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The tolerance table: each departure from the standard that the reader reads as intended, as a constructed fixture
// with invented values, the diagnostic it gives with the section of the standard, how it is read, and what the
// conformance report of verify() finds of it: the same diagnostic, and for the departures that reads do not see, the
// finding of verify() alone.

#include <openfits/checksum.h>
#include <openfits/error.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/verify.h>

#include "support/checksums.h"
#include "support/compressed_fixtures.h"
#include "support/fixture_builder.h"
#include "support/table_fixtures.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using openfits::diagnostic;
using openfits::errc;
using openfits::hdu_kind;
using openfits::reader;
using openfits::severity;
using openfits::value_kind;
using openfits::test::bintable_records;
using openfits::test::card_record;
using openfits::test::extension_records;
using openfits::test::field_of;
using openfits::test::fixture;
using openfits::test::primary_records;
using openfits::test::record;
using openfits::test::row_of;
using openfits::test::with;

struct tolerance_row
{
    std::string name{};
    std::vector<std::byte> file{};
    // The unit whose diagnostics hold that of the row, nothing for those of the file.
    std::optional<std::size_t> hdu{};
    // The code of the diagnostic, nothing for a departure that takes none, and the section of the standard it departs
    // from.
    std::optional<errc> code{};
    severity level = severity::warning;
    std::string section{};
    // How the file is read.
    std::function<void(const reader&)> reading{};
    // The finding that verify() alone makes of a departure that reads do not see, in the unit hdu.
    std::optional<errc> verify_code{};
    severity verify_level = severity::info;
    std::string verify_section{};
};

// GoogleTest prints the parameter in messages, and CTest names the test after it. The name is fixed by GoogleTest.
// NOLINTNEXTLINE(readability-identifier-naming)
void PrintTo(const tolerance_row& row, std::ostream* out)
{
    *out << row.name;
}

std::vector<tolerance_row> rows()
{
    std::vector<tolerance_row> table;
    const auto add = [&table](std::string name, const fixture& file, std::optional<std::size_t> hdu,
                              std::optional<errc> code, severity level, std::string section,
                              std::function<void(const reader&)> reading) {
        tolerance_row row;
        row.name = std::move(name);
        row.file = file.bytes();
        row.hdu = hdu;
        row.code = code;
        row.level = level;
        row.section = std::move(section);
        row.reading = std::move(reading);
        table.push_back(std::move(row));
    };
    // The finding of verify() alone of the last row added.
    const auto verified = [&table](errc code, severity level, std::string section) {
        table.back().verify_code = code;
        table.back().verify_level = level;
        table.back().verify_section = std::move(section);
    };
    const auto primary_with = [](const std::vector<std::string>& more) {
        return fixture().header(with(primary_records(), more));
    };

    // The ends of headers and files.
    // The blank records that follow the last card are the fill of the header.
    add("missing_end", fixture().header(primary_records(), false), 0, errc::missing_end, severity::warning, "3.3.1",
        [](const reader& input) {
            EXPECT_EQ(input.hdu(0).header().size(), 36U);
            EXPECT_EQ(input.hdu(0).header().free_cards(), 33U);
            EXPECT_EQ(input.hdu(0).header_size(), 2880U);
        });
    add("short_last_block", fixture().primary().truncate(400), 0, errc::truncated_file, severity::warning, "3.1",
        [](const reader& input) {
            EXPECT_EQ(input.hdu(0).header().size(), 3U);
            EXPECT_EQ(input.hdu(0).header_size(), 400U);
        });
    add("bytes_after_end", fixture().header(with(primary_records(), {record("END"), record("garbage")}), false), 0,
        errc::header_fill, severity::warning, "3.3.1",
        [](const reader& input) { EXPECT_EQ(input.hdu(0).header().size(), 3U); });
    add("end_with_bytes_after_it", fixture().header(with(primary_records(), {record("END     garbage")}), false), 0,
        errc::header_fill, severity::warning, "4.4.1.1",
        [](const reader& input) { EXPECT_EQ(input.hdu(0).header().size(), 3U); });
    add("truncated_last_data_unit", fixture().primary().image({3000}).truncate(3 * 2880 + 100), std::nullopt,
        errc::truncated_file, severity::warning, "3.1", [](const reader& input) {
            EXPECT_EQ(input.hdu_count(), 2U);
            EXPECT_EQ(input.hdu(1).data_size(), 3000U);
            std::vector<std::byte> present(2880 + 100);
            input.read_raw(1, 0, present);
            std::vector<std::byte> missing(1);
            EXPECT_THROW(input.read_raw(1, 2880 + 100, missing), openfits::io_error);
        });
    add("special_records", fixture().primary().append(std::string(2880, 'x')), std::nullopt, errc::special_records,
        severity::warning, "3.5", [](const reader& input) { EXPECT_EQ(input.hdu_count(), 1U); });

    // Records.
    add("value_indicator_without_space", primary_with({record("KEY     =5"), record("OTHER = 7")}), 0,
        errc::invalid_value_indicator, severity::warning, "4.1.2.2", [](const reader& input) {
            EXPECT_EQ(input.hdu(0).header().value<int>("KEY"), 5);
            EXPECT_EQ(input.hdu(0).header().value<int>("OTHER"), 7);
        });
    add("lowercase_keyword", primary_with({record("exptime =                300.0")}), 0, errc::invalid_keyword,
        severity::warning, "4.1.2.1",
        [](const reader& input) { EXPECT_EQ(input.hdu(0).header().value<double>("EXPTIME"), 300.0); });
    add("keyword_with_spaces", primary_with({record("DATE OBS= '2020-01-01'")}), 0, errc::invalid_keyword,
        severity::warning, "4.1.2.1",
        [](const reader& input) { EXPECT_EQ(input.hdu(0).header().value<std::string>("DATE OBS"), "2020-01-01"); });
    add("unmatched_quote", primary_with({record("OBJECT  = 'M 31")}), 0, errc::invalid_string, severity::warning,
        "4.2.1.1", [](const reader& input) { EXPECT_EQ(input.hdu(0).header().value<std::string>("OBJECT"), "M 31"); });
    add("mandatory_keyword_in_free_format",
        fixture()
            .header(
                {card_record("SIMPLE", "T"), record("BITPIX  = 8"), card_record("NAXIS", "1"), record("NAXIS1  = 10")})
            .data(10),
        0, errc::free_format, severity::info, "4.4.1",
        [](const reader& input) { EXPECT_EQ(input.hdu(0).data_size(), 10U); });
    add("lowercase_logical", primary_with({card_record("FLAG", "t")}), 0, errc::invalid_logical, severity::warning,
        "4.2.2", [](const reader& input) { EXPECT_TRUE(input.hdu(0).header().value<bool>("FLAG")); });
    add("logical_as_a_string",
        fixture().header({card_record("SIMPLE", "'T'"), card_record("BITPIX", "8"), card_record("NAXIS", "0")}), 0,
        errc::keyword_type, severity::warning, "4.4.1.1",
        [](const reader& input) { EXPECT_EQ(input.hdu(0).kind(), hdu_kind::primary); });
    add("number_as_a_string", primary_with({card_record("EXPTIME", "'300.0'")}), std::nullopt, std::nullopt,
        severity::info, "", [](const reader& input) {
            EXPECT_EQ(input.hdu(0).header()[3].value().kind(), value_kind::string);
            EXPECT_EQ(input.hdu(0).header().value<double>("EXPTIME"), 300.0);
        });
    add("value_that_does_not_parse", primary_with({card_record("GAIN", "1,5")}), 0, errc::invalid_value,
        severity::warning, "4.2", [](const reader& input) {
            EXPECT_EQ(input.hdu(0).header()[3].value().kind(), value_kind::none);
            EXPECT_EQ(input.hdu(0).header().value<std::string>("GAIN"), "1,5");
        });
    // Rows that real files show: a string written without its quotes, and a real with a lowercase exponent letter, as
    // a capture program writes them.
    add("string_without_quotes", primary_with({record("INSTRUME=     Invented Cam 2600 /")}), 0, errc::invalid_value,
        severity::warning, "4.2", [](const reader& input) {
            EXPECT_EQ(input.hdu(0).header().value<std::string>("INSTRUME"), "Invented Cam 2600");
            EXPECT_EQ(input.hdu(0).header().comment("INSTRUME"), "");
        });
    add("lowercase_exponent", primary_with({record("A_0_2   =   -1.01234567890e-07 /")}), 0, errc::invalid_real,
        severity::warning, "4.2.4",
        [](const reader& input) { EXPECT_EQ(input.hdu(0).header().value<double>("A_0_2"), -1.01234567890e-07); });
    add("integer_beyond_64_bits", primary_with({card_record("BIG", "99999999999999999999")}), 0,
        errc::value_out_of_range, severity::warning, "", [](const reader& input) {
            EXPECT_EQ(input.hdu(0).header().value<std::string>("BIG"), "99999999999999999999");
        });
    std::vector<std::string> unparsed = primary_records(8, {1});
    unparsed[3] = card_record("NAXIS1", "1,5");
    add("mandatory_value_that_does_not_parse", fixture().header(unparsed).data(1), 0, errc::keyword_type,
        severity::error, "4.4.1.1", [](const reader& input) { EXPECT_EQ(input.hdu(0).data_size(), std::nullopt); });
    add("non_ascii_bytes",
        primary_with({record("OBJECT  = 'Nebulosa del \xD1"
                             "and\xFA' / 5\xB0 field")}),
        0, errc::invalid_character, severity::warning, "4.1.2.3", [](const reader& input) {
            EXPECT_EQ(input.hdu(0).header().value<std::string>("OBJECT"), "Nebulosa del \xD1"
                                                                          "and\xFA");
            EXPECT_EQ(input.hdu(0).header().comment("OBJECT"), "5\xB0 field");
        });

    // Mandatory and reserved keywords.
    add("naxis_0_with_naxisn", primary_with({card_record("NAXIS1", "10")}), 0, errc::invalid_structure,
        severity::warning, "4.4.1.1", [](const reader& input) { EXPECT_EQ(input.hdu(0).data_size(), 0U); });
    add("naxisn_0", fixture().header(primary_records(16, {0, 20})), std::nullopt, std::nullopt, severity::info, "",
        [](const reader& input) { EXPECT_EQ(input.hdu(0).data_size(), 0U); });
    add("extend_false_with_extensions", primary_with({card_record("EXTEND", "F")}).image({1}), std::nullopt,
        errc::reserved_keyword, severity::warning, "4.4.2.1",
        [](const reader& input) { EXPECT_EQ(input.hdu_count(), 2U); });
    add("simple_false",
        fixture().header({card_record("SIMPLE", "F"), card_record("BITPIX", "8"), card_record("NAXIS", "0")}), 0,
        errc::invalid_structure, severity::warning, "4.4.1.1",
        [](const reader& input) { EXPECT_EQ(input.hdu(0).kind(), hdu_kind::primary); });
    add("counts_in_a_primary_array",
        fixture()
            .header(with(primary_records(8, {10}), {card_record("PCOUNT", "5"), card_record("GCOUNT", "2")}))
            .data(10)
            .image({1}),
        0, errc::reserved_keyword, severity::warning, "4.4.1.1", [](const reader& input) {
            EXPECT_EQ(input.hdu(0).data_size(), 10U);
            EXPECT_EQ(input.hdu_count(), 2U);
        });
    add("inherit_in_the_primary_header", primary_with({card_record("INHERIT", "T")}), 0, errc::reserved_keyword,
        severity::warning, "Appendix K",
        [](const reader& input) { EXPECT_TRUE(input.hdu(0).header().value<bool>("INHERIT")); });
    add("extend_in_an_extension", fixture().primary().image({2}, {card_record("EXTEND", "T")}), 1, errc::keyword_order,
        severity::warning, "4.4.2.1", [](const reader& input) { EXPECT_EQ(input.hdu(1).data_size(), 2U); });
    add("image_without_pcount_and_gcount",
        fixture()
            .primary()
            .header({card_record("XTENSION", "'IMAGE   '"), card_record("BITPIX", "16"), card_record("NAXIS", "1"),
                     card_record("NAXIS1", "5")})
            .data(10)
            .image({1}),
        1, errc::missing_keyword, severity::warning, "7.1.1", [](const reader& input) {
            EXPECT_EQ(input.hdu(1).data_size(), 10U);
            EXPECT_EQ(input.hdu_count(), 3U);
        });
    add("blank_of_floating_point_data",
        fixture().header(with(primary_records(-32, {2}), {card_record("BLANK", "-32768")})).data(8), 0,
        errc::reserved_keyword, severity::warning, "4.4.2.5",
        [](const reader& input) { EXPECT_EQ(input.hdu(0).data_size(), 8U); });
    add("bscale_0", fixture().header(with(primary_records(16, {2}), {card_record("BSCALE", "0.0")})).data(4).image({1}),
        0, errc::reserved_keyword, severity::error, "4.4.2.5", [](const reader& input) {
            EXPECT_EQ(input.hdu(0).data_size(), 4U);
            EXPECT_EQ(input.hdu_count(), 2U);
        });
    add("repeated_keyword", primary_with({card_record("EXPTIME", "1.0"), card_record("EXPTIME", "2.0")}), 0,
        errc::repeated_keyword, severity::warning, "4.1.2.3", [](const reader& input) {
            EXPECT_EQ(input.hdu(0).header().value<double>("EXPTIME"), 1.0);
            EXPECT_EQ(input.hdu(0).header().count("EXPTIME"), 2U);
        });
    add("date_in_the_old_form", primary_with({card_record("DATE-OBS", "'21/03/98'")}), 0, errc::invalid_date,
        severity::warning, "4.4.2.1", [](const reader& input) {
            EXPECT_EQ(openfits::parse_date(input.hdu(0).header().value<std::string>("DATE-OBS")),
                      (openfits::date_time{.year = 1998, .month = 3, .day = 21}));
        });
    add("xtension_in_lowercase", fixture().primary().header(extension_records("image")), 1, errc::invalid_structure,
        severity::warning, "4.4.1.2", [](const reader& input) { EXPECT_EQ(input.hdu(1).kind(), hdu_kind::image); });
    std::vector<std::string> unpadded = extension_records();
    unpadded[0] = card_record("XTENSION", "'IMAGE'");
    add("xtension_without_its_spaces", fixture().primary().header(unpadded), 1, errc::invalid_structure,
        severity::warning, "4.2.1.1", [](const reader& input) { EXPECT_EQ(input.hdu(1).kind(), hdu_kind::image); });

    // Long strings (standard §4.2.1.2).
    add("orphaned_continue", primary_with({card_record("OBJECT", "'M 31'"), record("CONTINUE  'more'")}), 0,
        errc::orphaned_continue, severity::info, "4.2.1.2", [](const reader& input) {
            const openfits::card& orphan = input.hdu(0).header()[4];
            EXPECT_EQ(orphan.keyword(), "CONTINUE");
            EXPECT_EQ(orphan.value().kind(), value_kind::none);
            EXPECT_EQ(orphan.comment(), "  'more'");
            EXPECT_EQ(input.hdu(0).header().value<std::string>("OBJECT"), "M 31");
        });
    add("ampersand_without_continue", primary_with({card_record("TITLE", "'Rock&'"), card_record("NEXT", "1")}),
        std::nullopt, std::nullopt, severity::info, "",
        [](const reader& input) { EXPECT_EQ(input.hdu(0).header().value<std::string>("TITLE"), "Rock&"); });
    add("continue_of_a_reserved_keyword",
        primary_with({card_record("OBJECT", "'Andromeda &'"), record("CONTINUE  'galaxy'")}), 0,
        errc::continued_reserved_keyword, severity::warning, "4.2.1.2",
        [](const reader& input) { EXPECT_EQ(input.hdu(0).header().value<std::string>("OBJECT"), "Andromeda galaxy"); });

    // Tables (standard §7.2, §7.3): what the keywords of a column give that it must not have, or cannot be, is ignored.
    const auto bintable_with = [](std::uint64_t row_size, const std::vector<std::string>& formats,
                                  const std::vector<std::string>& more, std::uint64_t pcount = 0) {
        fixture file;
        file.primary();
        file.header(with(bintable_records(row_size, 1, formats, pcount), more)).data(row_size + pcount);
        return file;
    };
    add("tform_in_lowercase", bintable_with(4, {"1j"}, {}), 1, errc::invalid_column_format, severity::warning, "7.3.1",
        [](const reader& input) {
            EXPECT_EQ(input.table(1).columns[0].format.value_or(openfits::column_format{}).type,
                      openfits::column_type::int32);
        });
    add("tnull_of_a_floating_point_column", bintable_with(4, {"E"}, {card_record("TNULL1", "0")}), 1,
        errc::reserved_keyword, severity::warning, "7.3.2",
        [](const reader& input) { EXPECT_EQ(input.table(1).columns[0].null, std::nullopt); });
    add("tscal_of_characters", bintable_with(4, {"4A"}, {card_record("TSCAL1", "2.0")}), 1, errc::reserved_keyword,
        severity::warning, "7.3.2",
        [](const reader& input) { EXPECT_EQ(input.table(1).columns[0].scaling, openfits::scaling{}); });
    add("fields_short_of_naxis1", bintable_with(6, {"J"}, {}), 1, errc::invalid_column_format, severity::warning,
        "7.3.1", [](const reader& input) { EXPECT_EQ(input.read_column<std::int32_t>(1, 0).size(), 1U); });
    add("tform_beyond_tfields", bintable_with(4, {"J"}, {card_record("TFORM2", "'E'")}), 1, errc::invalid_structure,
        severity::warning, "7.3.1", [](const reader& input) { EXPECT_EQ(input.table(1).columns.size(), 1U); });
    add("theap_with_pcount_0", bintable_with(4, {"J"}, {card_record("THEAP", "4")}), 1, errc::reserved_keyword,
        severity::warning, "7.3.2", [](const reader& input) { EXPECT_EQ(input.table(1).heap_size, 0U); });
    add("tdim_larger_than_its_field", bintable_with(16, {"4J"}, {card_record("TDIM1", "'(3,3)'")}), 1,
        errc::invalid_dimensions, severity::warning, "7.3.2",
        [](const reader& input) { EXPECT_TRUE(input.table(1).columns[0].dimensions.empty()); });
    add("tdisp_that_cannot_display_its_column", bintable_with(4, {"J"}, {card_record("TDISP1", "'A8'")}), 1,
        errc::invalid_display_format, severity::warning, "7.3.4",
        [](const reader& input) { EXPECT_EQ(input.table(1).columns[0].display, std::nullopt); });
    fixture numeric_null;
    numeric_null.primary();
    numeric_null
        .header(with(openfits::test::ascii_table_records(4, 1, {{.column = 1, .format = "I4"}}),
                     {card_record("TNULL1", "-99")}))
        .data(4);
    add("tnull_of_an_ascii_table_as_a_number", numeric_null, 1, errc::reserved_keyword, severity::warning, "7.2.2",
        [](const reader& input) { EXPECT_EQ(input.table(1).columns[0].null_string, "-99"); });
    std::vector<std::string> without_tfields = bintable_records(4, 1, {"J"});
    without_tfields.erase(without_tfields.begin() + 7);
    add("tfields_missing", fixture().primary().header(without_tfields).data(4), 1, errc::missing_keyword,
        severity::error, "7.3.1", [](const reader& input) {
            EXPECT_THROW((void)input.table(1), openfits::invalid_data_error);
            EXPECT_EQ(input.hdu(1).data_size(), 4U);
        });
    add("tscal_of_0", bintable_with(4, {"J"}, {card_record("TSCAL1", "0")}), 1, errc::reserved_keyword, severity::error,
        "7.3.2", [](const reader& input) {
            EXPECT_THROW((void)input.read_column<double>(1, 0), openfits::invalid_data_error);
            EXPECT_EQ(input.read_column<std::int32_t>(1, 0, {.scaling = openfits::scaling_policy::raw}).size(), 1U);
        });
    add("pscal_of_0",
        fixture()
            .header({card_record("SIMPLE", "T"), card_record("BITPIX", "16"), card_record("NAXIS", "2"),
                     card_record("NAXIS1", "0"), card_record("NAXIS2", "1"), card_record("GROUPS", "T"),
                     card_record("PCOUNT", "1"), card_record("GCOUNT", "1"), card_record("PSCAL1", "0")})
            .data(4),
        0, errc::reserved_keyword, severity::error, "6.1.2", [](const reader& input) {
            EXPECT_THROW((void)input.read_parameter<double>(0, 0), openfits::invalid_data_error);
            EXPECT_EQ(input.read_group<std::int16_t>(0, 0).size(), 1U);
        });

    // Checksums (standard §4.4.2.7), the sums computed by the test.
    const std::vector<std::byte> pixels = openfits::test::big_endian(std::vector<std::int16_t>{1, 2, 3});
    std::vector<std::byte> records = pixels;
    records.resize(2880);
    const std::string data_sum = std::to_string(openfits::test::reference_sum(records));
    const auto image_with = [&pixels](const std::vector<std::string>& more) {
        return fixture().header(with(primary_records(16, {3}), more)).data(pixels);
    };
    add("blank_checksums",
        image_with({card_record("CHECKSUM", "'                '"), card_record("DATASUM", "'          '")}), 0,
        std::nullopt, severity::warning, "", [](const reader& input) {
            const openfits::checksum_report report = input.verify(0);
            EXPECT_EQ(report.datasum, openfits::checksum_state::absent);
            EXPECT_EQ(report.checksum, openfits::checksum_state::absent);
            EXPECT_EQ(input.read_pixels<std::int16_t>(0), (std::vector<std::int16_t>{1, 2, 3}));
        });
    add("datasum_without_quotes", image_with({card_record("DATASUM", data_sum)}), 0, errc::reserved_keyword,
        severity::warning, "4.4.2.7", [](const reader& input) {
            EXPECT_EQ(input.verify(0).datasum, openfits::checksum_state::valid);
            EXPECT_EQ(input.read_pixels<std::int16_t>(0), (std::vector<std::int16_t>{1, 2, 3}));
        });
    add("datasum_of_no_sum", image_with({card_record("DATASUM", "'twelve'")}), 0, errc::reserved_keyword,
        severity::warning, "4.4.2.7", [](const reader& input) {
            EXPECT_EQ(input.verify(0).datasum, openfits::checksum_state::invalid);
            EXPECT_THROW((void)input.read_pixels<std::int16_t>(0), openfits::integrity_error);
        });
    add("checksum_without_quotes", image_with({card_record("CHECKSUM", "12")}), 0, errc::reserved_keyword,
        severity::warning, "4.4.2.7",
        [](const reader& input) { EXPECT_EQ(input.verify(0).checksum, openfits::checksum_state::invalid); });

    // Old compressed images, read as CFITSIO reads them (standard §10.1): an image of 4 x 2 values in tiles of a row,
    // each tile compressed alone in the heap.
    const auto compressed_image = [](int bitpix, const std::vector<std::string>& more,
                                     const std::vector<std::vector<std::byte>>& tiles,
                                     const std::vector<std::pair<std::string, std::string>>& extra_columns = {},
                                     const std::vector<std::vector<std::byte>>& extra_fields = {}) {
        std::vector<std::pair<std::string, std::string>> columns{{"COMPRESSED_DATA", "1PB"}};
        columns.insert(columns.end(), extra_columns.begin(), extra_columns.end());
        std::vector<std::vector<std::byte>> rows;
        std::vector<std::byte> heap;
        for (std::size_t t = 0; t < tiles.size(); ++t) {
            std::vector<std::byte> row = openfits::test::p_descriptor(static_cast<std::uint32_t>(tiles[t].size()),
                                                                      static_cast<std::uint32_t>(heap.size()));
            if (t < extra_fields.size()) {
                row.insert(row.end(), extra_fields[t].begin(), extra_fields[t].end());
            }
            rows.push_back(std::move(row));
            heap.insert(heap.end(), tiles[t].begin(), tiles[t].end());
        }
        fixture file;
        file.primary();
        std::vector<std::string> keywords = openfits::test::image_keywords(bitpix, {4, 2}, {4, 1}, "RICE_1");
        // ZCMPTYPE and the parameters of the algorithm come from more.
        keywords.pop_back();
        keywords.insert(keywords.end(), more.begin(), more.end());
        openfits::test::binary_table_unit(file, columns, rows, heap, keywords);
        return file;
    };
    const std::vector<std::int16_t> shorts{1, 2, 3, 4, -5, 6, -7, 8};
    const std::vector<std::int16_t> first_shorts(shorts.begin(), shorts.begin() + 4);
    const std::vector<std::int16_t> second_shorts(shorts.begin() + 4, shorts.end());
    const std::vector<std::vector<std::byte>> short_tiles{openfits::test::rice16(first_shorts),
                                                          openfits::test::rice16(second_shorts)};
    const auto reads_shorts = [shorts](const reader& input) { EXPECT_EQ(input.read_pixels<std::int16_t>(1), shorts); };
    add("rice_one",
        compressed_image(16,
                         {card_record("ZCMPTYPE", "'RICE_ONE'"), card_record("ZNAME1", "'BLOCKSIZE'"),
                          card_record("ZVAL1", "32"), card_record("ZNAME2", "'BYTEPIX'"), card_record("ZVAL2", "2")},
                         short_tiles),
        1, errc::invalid_compression, severity::info, "10.1.1", reads_shorts);
    add("rice_parameters_reversed",
        compressed_image(16,
                         {card_record("ZCMPTYPE", "'RICE_1'"), card_record("ZNAME1", "'BLOCKSIZE'"),
                          card_record("ZVAL1", "2"), card_record("ZNAME2", "'BYTEPIX'"), card_record("ZVAL2", "32")},
                         short_tiles),
        1, errc::invalid_compression, severity::warning, "10.4.1", reads_shorts);
    const std::vector<std::int32_t> ints{100, -200, 300, -400, 500, -600, 700, -800};
    // The readings that hold a vector are given as lvalues, and copied: a closure holds a captured const vector as
    // const, so that its move would be a copy, which may throw.
    const auto reads_ints = [ints](const reader& input) { EXPECT_EQ(input.read_pixels<std::int32_t>(1), ints); };
    add("rice_noisebit",
        compressed_image(
            32,
            {card_record("ZCMPTYPE", "'RICE_1'"), card_record("ZNAME1", "'BLOCKSIZE'"), card_record("ZVAL1", "32"),
             card_record("ZNAME2", "'NOISEBIT'"), card_record("ZVAL2", "2")},
            {openfits::test::rice32({100, -200, 300, -400}), openfits::test::rice32({500, -600, 700, -800})}),
        1, errc::invalid_compression, severity::warning, "10.4.1", reads_ints);
    // Quantized codes 0 to 7, restored as code x ZSCALE + ZZERO without dithering.
    const std::vector<std::vector<std::byte>> code_tiles{openfits::test::rice32({0, 1, 2, 3}),
                                                         openfits::test::rice32({4, 5, 6, 7})};
    const std::vector<std::string> rice_codes{card_record("ZCMPTYPE", "'RICE_1'"), card_record("ZNAME1", "'BLOCKSIZE'"),
                                              card_record("ZVAL1", "32"), card_record("ZNAME2", "'BYTEPIX'"),
                                              card_record("ZVAL2", "4")};
    add("zscale_and_zzero_keywords",
        compressed_image(-32, with(rice_codes, {card_record("ZSCALE", "0.5"), card_record("ZZERO", "10.0")}),
                         code_tiles),
        1, errc::invalid_compression, severity::info, "10.1.3", [](const reader& input) {
            EXPECT_EQ(input.read_pixels<float>(1),
                      (std::vector<float>{10.0F, 10.5F, 11.0F, 11.5F, 12.0F, 12.5F, 13.0F, 13.5F}));
        });
    add("quantized_without_zquantiz",
        compressed_image(-32, rice_codes, code_tiles, {{"ZSCALE", "1D"}, {"ZZERO", "1D"}},
                         {row_of({field_of(0.25), field_of(-1.0)}), row_of({field_of(2.0), field_of(0.0)})}),
        std::nullopt, std::nullopt, severity::warning, "", [](const reader& input) {
            // NO_DITHER (standard §10.2.1), each tile with its own ZSCALE and ZZERO.
            EXPECT_EQ(input.read_pixels<float>(1),
                      (std::vector<float>{-1.0F, -0.75F, -0.5F, -0.25F, 8.0F, 10.0F, 12.0F, 14.0F}));
        });
    const std::vector<float> reals{0.5F, -1.5F, 2.25F, 1e30F, -0.0F, 3.0F, 4.0F, 5.0F};
    const std::vector<std::byte> real_bytes = openfits::test::big_endian(reals);
    const auto reads_reals = [reals](const reader& input) { EXPECT_EQ(input.read_pixels<float>(1), reals); };
    add("zquantiz_none",
        compressed_image(-32, {card_record("ZCMPTYPE", "'GZIP_1'"), card_record("ZQUANTIZ", "'NONE'")},
                         {openfits::test::gzip(std::span(real_bytes).first(16)),
                          openfits::test::gzip(std::span(real_bytes).subspan(16))}),
        std::nullopt, std::nullopt, severity::warning, "", reads_reals);
    verified(errc::invalid_compression, severity::info, "10.2.1");

    // The data that reads take as they are and verify() checks: the fill of the last block of a data unit, zeros, or
    // spaces in an ASCII table (standard §3.3.2, §7.2.3), and numbers of an ASCII table in forms that standard §7.2.5
    // forbids.
    std::vector<std::byte> filled(openfits::test::block_bytes);
    filled[100] = std::byte{'x'};
    add("data_fill_not_zeros", fixture().header(primary_records(16, {3})).data(filled), std::nullopt, std::nullopt,
        severity::info, "", [](const reader& input) {
            EXPECT_EQ(input.read_pixels<std::int16_t>(0), (std::vector<std::int16_t>{0, 0, 0}));
        });
    verified(errc::data_fill, severity::info, "3.3.2");
    const auto ascii_table_of = [](std::string_view format, std::string_view row, char fill) {
        std::vector<std::byte> data;
        for (const char c : row) {
            data.push_back(static_cast<std::byte>(c));
        }
        data.resize(openfits::test::block_bytes, static_cast<std::byte>(fill));
        fixture file;
        file.primary()
            .header(openfits::test::ascii_table_records(row.size(), 1, {{.column = 1, .format = std::string(format)}}))
            .data(data);
        return file;
    };
    add("ascii_fill_not_spaces", ascii_table_of("I4", "  12", '\0'), std::nullopt, std::nullopt, severity::info, "",
        [](const reader& input) { EXPECT_EQ(input.read_column<std::int64_t>(1, 0), (std::vector<std::int64_t>{12})); });
    verified(errc::data_fill, severity::info, "7.2.3");
    add("spaces_inside_an_ascii_number", ascii_table_of("I6", "  1 23", ' '), std::nullopt, std::nullopt,
        severity::info, "", [](const reader& input) {
            EXPECT_EQ(input.read_column<std::int64_t>(1, 0), (std::vector<std::int64_t>{123}));
        });
    verified(errc::invalid_field, severity::warning, "7.2.5");
    add("lowercase_exponent_in_an_ascii_number", ascii_table_of("E10.2", "  1.50e+02", ' '), std::nullopt, std::nullopt,
        severity::info, "",
        [](const reader& input) { EXPECT_EQ(input.read_column<double>(1, 0), (std::vector<double>{150.0})); });
    verified(errc::invalid_field, severity::warning, "7.2.5");

    // Units whose headers give no size stop the walk.
    add("bitpix_outside_its_values", fixture().header(primary_records(24)).image({1}), 0, errc::invalid_structure,
        severity::error, "4.4.1.1", [](const reader& input) {
            EXPECT_EQ(input.hdu(0).data_size(), std::nullopt);
            EXPECT_EQ(input.hdu_count(), 1U);
        });
    add("negative_naxisn",
        fixture().primary().header({card_record("XTENSION", "'IMAGE   '"), card_record("BITPIX", "8"),
                                    card_record("NAXIS", "1"), card_record("NAXIS1", "-1"), card_record("PCOUNT", "0"),
                                    card_record("GCOUNT", "1")}),
        1, errc::invalid_structure, severity::error, "4.4.1.1", [](const reader& input) {
            EXPECT_EQ(input.hdu(1).data_size(), std::nullopt);
            EXPECT_THROW((void)input.hdu(2), openfits::invalid_data_error);
        });
    add("size_beyond_64_bits", fixture().header(primary_records(-64, {1ULL << 40, 1ULL << 40})), 0,
        errc::invalid_structure, severity::error, "",
        [](const reader& input) { EXPECT_EQ(input.hdu(0).data_size(), std::nullopt); });
    return table;
}

class conformance_tolerance : public testing::TestWithParam<tolerance_row>
{};

TEST_P(conformance_tolerance, reads_the_departure_as_intended)
{
    const tolerance_row& row = GetParam();
    const reader input(std::make_unique<openfits::memory_source>(std::span<const std::byte>(row.file)));
    std::vector<diagnostic> all = input.diagnostics();
    for (const openfits::hdu_info& unit : input.hdus()) {
        all.insert(all.end(), unit.diagnostics().begin(), unit.diagnostics().end());
    }
    if (row.code) {
        const std::vector<diagnostic> found = row.hdu ? input.hdu(*row.hdu).diagnostics() : input.diagnostics();
        EXPECT_TRUE(std::ranges::any_of(found,
                                        [&row](const diagnostic& entry) {
                                            return entry.code == *row.code && entry.severity == row.level &&
                                                   entry.section == row.section;
                                        }))
            << "diagnostics: " << testing::PrintToString(all.size());
        for (const diagnostic& entry : all) {
            EXPECT_TRUE(entry.severity != severity::error || (row.level == severity::error && entry.code == *row.code))
                << entry.message;
        }
    } else {
        for (const diagnostic& entry : all) {
            ADD_FAILURE() << "unexpected diagnostic: " << entry.message;
        }
    }
    row.reading(input);
}

TEST_P(conformance_tolerance, is_found_by_verify_with_its_section)
{
    const tolerance_row& row = GetParam();
    const reader input(std::make_unique<openfits::memory_source>(std::span<const std::byte>(row.file)));
    const std::vector<diagnostic> report = openfits::verify(input);
    std::string listing;
    for (const diagnostic& entry : report) {
        listing += '[' + entry.section + "] " + entry.message + '\n';
    }
    const auto has = [&report, &row](errc code, severity level, const std::string& section) {
        return std::ranges::any_of(report, [&](const diagnostic& entry) {
            return entry.code == code && entry.severity == level && entry.section == section &&
                   (!row.hdu || entry.context.hdu == row.hdu);
        });
    };
    if (row.code) {
        EXPECT_TRUE(has(*row.code, row.level, row.section)) << listing;
    }
    if (row.verify_code) {
        EXPECT_TRUE(has(*row.verify_code, row.verify_level, row.verify_section)) << listing;
    }
    if (!row.code && !row.verify_code) {
        EXPECT_TRUE(report.empty()) << listing;
    }
}

INSTANTIATE_TEST_SUITE_P(rows, conformance_tolerance, testing::ValuesIn(rows()),
                         [](const testing::TestParamInfo<tolerance_row>& instance) { return instance.param.name; });

} // namespace
