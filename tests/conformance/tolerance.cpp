// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The tolerance table: each departure from the standard that the reader reads as intended, as a constructed fixture
// with invented values, the diagnostic it gives and how it is read. The rows of the departures of compressed data and
// checksums come with those features.

#include <openfits/error.h>
#include <openfits/io.h>
#include <openfits/reader.h>

#include "support/fixture_builder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <vector>

namespace {

using openfits::diagnostic;
using openfits::errc;
using openfits::hdu_kind;
using openfits::reader;
using openfits::severity;
using openfits::value_kind;
using openfits::test::card_record;
using openfits::test::extension_records;
using openfits::test::fixture;
using openfits::test::primary_records;
using openfits::test::record;
using openfits::test::with;

struct tolerance_row
{
    std::string name{};
    std::vector<std::byte> file{};
    // The unit whose diagnostics hold that of the row, nothing for those of the file.
    std::optional<std::size_t> hdu{};
    // The code of the diagnostic, nothing for a departure that takes none.
    std::optional<errc> code{};
    severity level = severity::warning;
    // How the file is read.
    std::function<void(const reader&)> reading{};
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
                              std::optional<errc> code, severity level, std::function<void(const reader&)> reading) {
        tolerance_row row;
        row.name = std::move(name);
        row.file = file.bytes();
        row.hdu = hdu;
        row.code = code;
        row.level = level;
        row.reading = std::move(reading);
        table.push_back(std::move(row));
    };
    const auto primary_with = [](const std::vector<std::string>& more) {
        return fixture().header(with(primary_records(), more));
    };

    // The ends of headers and files.
    // The blank records that follow the last card are the fill of the header.
    add("missing_end", fixture().header(primary_records(), false), 0, errc::missing_end, severity::warning,
        [](const reader& input) {
            EXPECT_EQ(input.hdu(0).header().size(), 36U);
            EXPECT_EQ(input.hdu(0).header().free_cards(), 33U);
            EXPECT_EQ(input.hdu(0).header_size(), 2880U);
        });
    add("short_last_block", fixture().primary().truncate(400), 0, errc::truncated_file, severity::warning,
        [](const reader& input) {
            EXPECT_EQ(input.hdu(0).header().size(), 3U);
            EXPECT_EQ(input.hdu(0).header_size(), 400U);
        });
    add("bytes_after_end", fixture().header(with(primary_records(), {record("END"), record("garbage")}), false), 0,
        errc::header_fill, severity::warning, [](const reader& input) { EXPECT_EQ(input.hdu(0).header().size(), 3U); });
    add("end_with_bytes_after_it", fixture().header(with(primary_records(), {record("END     garbage")}), false), 0,
        errc::header_fill, severity::warning, [](const reader& input) { EXPECT_EQ(input.hdu(0).header().size(), 3U); });
    add("truncated_last_data_unit", fixture().primary().image({3000}).truncate(3 * 2880 + 100), std::nullopt,
        errc::truncated_file, severity::warning, [](const reader& input) {
            EXPECT_EQ(input.hdu_count(), 2U);
            EXPECT_EQ(input.hdu(1).data_size(), 3000U);
            std::vector<std::byte> present(2880 + 100);
            input.read_raw(1, 0, present);
            std::vector<std::byte> missing(1);
            EXPECT_THROW(input.read_raw(1, 2880 + 100, missing), openfits::io_error);
        });
    add("special_records", fixture().primary().append(std::string(2880, 'x')), std::nullopt, errc::special_records,
        severity::warning, [](const reader& input) { EXPECT_EQ(input.hdu_count(), 1U); });

    // Records.
    add("value_indicator_without_space", primary_with({record("KEY     =5"), record("OTHER = 7")}), 0,
        errc::invalid_value_indicator, severity::warning, [](const reader& input) {
            EXPECT_EQ(input.hdu(0).header().value<int>("KEY"), 5);
            EXPECT_EQ(input.hdu(0).header().value<int>("OTHER"), 7);
        });
    add("lowercase_keyword", primary_with({record("exptime =                300.0")}), 0, errc::invalid_keyword,
        severity::warning,
        [](const reader& input) { EXPECT_EQ(input.hdu(0).header().value<double>("EXPTIME"), 300.0); });
    add("keyword_with_spaces", primary_with({record("DATE OBS= '2020-01-01'")}), 0, errc::invalid_keyword,
        severity::warning,
        [](const reader& input) { EXPECT_EQ(input.hdu(0).header().value<std::string>("DATE OBS"), "2020-01-01"); });
    add("unmatched_quote", primary_with({record("OBJECT  = 'M 31")}), 0, errc::invalid_string, severity::warning,
        [](const reader& input) { EXPECT_EQ(input.hdu(0).header().value<std::string>("OBJECT"), "M 31"); });
    add("mandatory_keyword_in_free_format",
        fixture()
            .header(
                {card_record("SIMPLE", "T"), record("BITPIX  = 8"), card_record("NAXIS", "1"), record("NAXIS1  = 10")})
            .data(10),
        0, errc::free_format, severity::info, [](const reader& input) { EXPECT_EQ(input.hdu(0).data_size(), 10U); });
    add("lowercase_logical", primary_with({card_record("FLAG", "t")}), 0, errc::invalid_logical, severity::warning,
        [](const reader& input) { EXPECT_TRUE(input.hdu(0).header().value<bool>("FLAG")); });
    add("logical_as_a_string",
        fixture().header({card_record("SIMPLE", "'T'"), card_record("BITPIX", "8"), card_record("NAXIS", "0")}), 0,
        errc::keyword_type, severity::warning,
        [](const reader& input) { EXPECT_EQ(input.hdu(0).kind(), hdu_kind::primary); });
    add("number_as_a_string", primary_with({card_record("EXPTIME", "'300.0'")}), std::nullopt, std::nullopt,
        severity::info, [](const reader& input) {
            EXPECT_EQ(input.hdu(0).header()[3].value().kind(), value_kind::string);
            EXPECT_EQ(input.hdu(0).header().value<double>("EXPTIME"), 300.0);
        });
    add("value_that_does_not_parse", primary_with({card_record("GAIN", "1,5")}), 0, errc::invalid_value,
        severity::warning, [](const reader& input) {
            EXPECT_EQ(input.hdu(0).header()[3].value().kind(), value_kind::none);
            EXPECT_EQ(input.hdu(0).header().value<std::string>("GAIN"), "1,5");
        });
    // Rows that real files show: a string written without its quotes, and a real with a lowercase exponent letter, as
    // a capture program writes them.
    add("string_without_quotes", primary_with({record("INSTRUME=     Invented Cam 2600 /")}), 0, errc::invalid_value,
        severity::warning, [](const reader& input) {
            EXPECT_EQ(input.hdu(0).header().value<std::string>("INSTRUME"), "Invented Cam 2600");
            EXPECT_EQ(input.hdu(0).header().comment("INSTRUME"), "");
        });
    add("lowercase_exponent", primary_with({record("A_0_2   =   -1.01234567890e-07 /")}), 0, errc::invalid_real,
        severity::warning,
        [](const reader& input) { EXPECT_EQ(input.hdu(0).header().value<double>("A_0_2"), -1.01234567890e-07); });
    add("integer_beyond_64_bits", primary_with({card_record("BIG", "99999999999999999999")}), 0,
        errc::value_out_of_range, severity::warning, [](const reader& input) {
            EXPECT_EQ(input.hdu(0).header().value<std::string>("BIG"), "99999999999999999999");
        });
    std::vector<std::string> unparsed = primary_records(8, {1});
    unparsed[3] = card_record("NAXIS1", "1,5");
    add("mandatory_value_that_does_not_parse", fixture().header(unparsed).data(1), 0, errc::keyword_type,
        severity::error, [](const reader& input) { EXPECT_EQ(input.hdu(0).data_size(), std::nullopt); });
    add("non_ascii_bytes",
        primary_with({record("OBJECT  = 'Nebulosa del \xD1"
                             "and\xFA' / 5\xB0 field")}),
        0, errc::invalid_character, severity::warning, [](const reader& input) {
            EXPECT_EQ(input.hdu(0).header().value<std::string>("OBJECT"), "Nebulosa del \xD1"
                                                                          "and\xFA");
            EXPECT_EQ(input.hdu(0).header().comment("OBJECT"), "5\xB0 field");
        });

    // Mandatory and reserved keywords.
    add("naxis_0_with_naxisn", primary_with({card_record("NAXIS1", "10")}), 0, errc::invalid_structure,
        severity::warning, [](const reader& input) { EXPECT_EQ(input.hdu(0).data_size(), 0U); });
    add("naxisn_0", fixture().header(primary_records(16, {0, 20})), std::nullopt, std::nullopt, severity::info,
        [](const reader& input) { EXPECT_EQ(input.hdu(0).data_size(), 0U); });
    add("extend_false_with_extensions", primary_with({card_record("EXTEND", "F")}).image({1}), std::nullopt,
        errc::reserved_keyword, severity::warning, [](const reader& input) { EXPECT_EQ(input.hdu_count(), 2U); });
    add("simple_false",
        fixture().header({card_record("SIMPLE", "F"), card_record("BITPIX", "8"), card_record("NAXIS", "0")}), 0,
        errc::invalid_structure, severity::warning,
        [](const reader& input) { EXPECT_EQ(input.hdu(0).kind(), hdu_kind::primary); });
    add("image_without_pcount_and_gcount",
        fixture()
            .primary()
            .header({card_record("XTENSION", "'IMAGE   '"), card_record("BITPIX", "16"), card_record("NAXIS", "1"),
                     card_record("NAXIS1", "5")})
            .data(10)
            .image({1}),
        1, errc::missing_keyword, severity::warning, [](const reader& input) {
            EXPECT_EQ(input.hdu(1).data_size(), 10U);
            EXPECT_EQ(input.hdu_count(), 3U);
        });
    add("blank_of_floating_point_data",
        fixture().header(with(primary_records(-32, {2}), {card_record("BLANK", "-32768")})).data(8), 0,
        errc::reserved_keyword, severity::warning,
        [](const reader& input) { EXPECT_EQ(input.hdu(0).data_size(), 8U); });
    add("bscale_0", fixture().header(with(primary_records(16, {2}), {card_record("BSCALE", "0.0")})).data(4).image({1}),
        0, errc::reserved_keyword, severity::error, [](const reader& input) {
            EXPECT_EQ(input.hdu(0).data_size(), 4U);
            EXPECT_EQ(input.hdu_count(), 2U);
        });
    add("repeated_keyword", primary_with({card_record("EXPTIME", "1.0"), card_record("EXPTIME", "2.0")}), 0,
        errc::repeated_keyword, severity::warning, [](const reader& input) {
            EXPECT_EQ(input.hdu(0).header().value<double>("EXPTIME"), 1.0);
            EXPECT_EQ(input.hdu(0).header().count("EXPTIME"), 2U);
        });
    add("date_in_the_old_form", primary_with({card_record("DATE-OBS", "'21/03/98'")}), 0, errc::invalid_date,
        severity::warning, [](const reader& input) {
            EXPECT_EQ(openfits::parse_date(input.hdu(0).header().value<std::string>("DATE-OBS")),
                      (openfits::date_time{.year = 1998, .month = 3, .day = 21}));
        });
    add("xtension_in_lowercase", fixture().primary().header(extension_records("image")), 1, errc::invalid_structure,
        severity::warning, [](const reader& input) { EXPECT_EQ(input.hdu(1).kind(), hdu_kind::image); });
    std::vector<std::string> unpadded = extension_records();
    unpadded[0] = card_record("XTENSION", "'IMAGE'");
    add("xtension_without_its_spaces", fixture().primary().header(unpadded), 1, errc::invalid_structure,
        severity::warning, [](const reader& input) { EXPECT_EQ(input.hdu(1).kind(), hdu_kind::image); });

    // Long strings (standard §4.2.1.2).
    add("orphaned_continue", primary_with({card_record("OBJECT", "'M 31'"), record("CONTINUE  'more'")}), 0,
        errc::orphaned_continue, severity::info, [](const reader& input) {
            const openfits::card& orphan = input.hdu(0).header()[4];
            EXPECT_EQ(orphan.keyword(), "CONTINUE");
            EXPECT_EQ(orphan.value().kind(), value_kind::none);
            EXPECT_EQ(orphan.comment(), "  'more'");
            EXPECT_EQ(input.hdu(0).header().value<std::string>("OBJECT"), "M 31");
        });
    add("ampersand_without_continue", primary_with({card_record("TITLE", "'Rock&'"), card_record("NEXT", "1")}),
        std::nullopt, std::nullopt, severity::info,
        [](const reader& input) { EXPECT_EQ(input.hdu(0).header().value<std::string>("TITLE"), "Rock&"); });
    add("continue_of_a_reserved_keyword",
        primary_with({card_record("OBJECT", "'Andromeda &'"), record("CONTINUE  'galaxy'")}), 0,
        errc::continued_reserved_keyword, severity::warning,
        [](const reader& input) { EXPECT_EQ(input.hdu(0).header().value<std::string>("OBJECT"), "Andromeda galaxy"); });

    // Units whose headers give no size stop the walk.
    add("bitpix_outside_its_values", fixture().header(primary_records(24)).image({1}), 0, errc::invalid_structure,
        severity::error, [](const reader& input) {
            EXPECT_EQ(input.hdu(0).data_size(), std::nullopt);
            EXPECT_EQ(input.hdu_count(), 1U);
        });
    add("negative_naxisn",
        fixture().primary().header({card_record("XTENSION", "'IMAGE   '"), card_record("BITPIX", "8"),
                                    card_record("NAXIS", "1"), card_record("NAXIS1", "-1"), card_record("PCOUNT", "0"),
                                    card_record("GCOUNT", "1")}),
        1, errc::invalid_structure, severity::error, [](const reader& input) {
            EXPECT_EQ(input.hdu(1).data_size(), std::nullopt);
            EXPECT_THROW((void)input.hdu(2), openfits::invalid_data_error);
        });
    add("size_beyond_64_bits", fixture().header(primary_records(-64, {1ULL << 40, 1ULL << 40})), 0,
        errc::invalid_structure, severity::error,
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
        EXPECT_TRUE(std::ranges::any_of(
            found, [&row](const diagnostic& entry) { return entry.code == *row.code && entry.severity == row.level; }))
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

INSTANTIATE_TEST_SUITE_P(rows, conformance_tolerance, testing::ValuesIn(rows()),
                         [](const testing::TestParamInfo<tolerance_row>& instance) { return instance.param.name; });

} // namespace
