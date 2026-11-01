// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Standard §3 and §4.4.1: the blocks of a file, its header and data units, their mandatory keywords and the sizes of
// their data (Eq. 1, 2 and 4), on constructed fixtures. The departures that the reader tolerates are the rows of
// tolerance.cpp.

#include <openfits/error.h>
#include <openfits/io.h>
#include <openfits/reader.h>

#include "support/fixture_builder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using openfits::diagnostic;
using openfits::errc;
using openfits::hdu_info;
using openfits::hdu_kind;
using openfits::reader;
using openfits::severity;
using openfits::test::card_record;
using openfits::test::extension_records;
using openfits::test::fixture;
using openfits::test::primary_records;
using openfits::test::record;
using openfits::test::with;

// The source owns a copy of the bytes, so that a temporary fixture can be opened.
reader open(const fixture& file)
{
    return reader(std::make_unique<openfits::memory_source>(std::vector<std::byte>(file.bytes())));
}

bool has(const std::vector<diagnostic>& diagnostics, errc code, severity level)
{
    return std::ranges::any_of(diagnostics,
                               [&](const diagnostic& found) { return found.code == code && found.severity == level; });
}

// §3.3.1, §4.4.1.1: a primary header without data, and its END in the first block.
TEST(conformance_file_structure, a_primary_header_alone_is_a_file)
{
    const reader input = open(fixture().primary());
    ASSERT_EQ(input.hdu_count(), 1U);
    const hdu_info& primary = input.hdu(0);
    EXPECT_EQ(primary.kind(), hdu_kind::primary);
    EXPECT_EQ(primary.header_size(), 2880U);
    EXPECT_EQ(primary.data_size(), 0U);
    EXPECT_EQ(primary.header().size(), 3U);
    EXPECT_TRUE(primary.diagnostics().empty());
    EXPECT_TRUE(input.diagnostics().empty());
}

// §3.3.2, Eq. 1: the primary array of |BITPIX| x NAXIS1 x ... x NAXISn bits, in blocks.
TEST(conformance_file_structure, the_primary_array_has_the_size_of_its_axes)
{
    for (const int bitpix : {8, 16, 32, 64, -32, -64}) {
        const std::uint64_t size = static_cast<std::uint64_t>(bitpix < 0 ? -bitpix : bitpix) / 8 * 3 * 5 * 7;
        const reader input = open(fixture().header(primary_records(bitpix, {3, 5, 7})).data(size).image({1}));
        EXPECT_EQ(input.hdu(0).data_size(), size) << bitpix;
        EXPECT_EQ(input.hdu(1).header_offset(), 2880U + ((size + 2879) / 2880 * 2880)) << bitpix;
    }
}

// §4.4.1.1: NAXIS = 0 means no data, whatever else the header says.
TEST(conformance_file_structure, naxis_0_has_no_data)
{
    const reader input = open(fixture().header(primary_records(16)).image({4}));
    EXPECT_EQ(input.hdu(0).data_size(), 0U);
    EXPECT_EQ(input.hdu(1).header_offset(), 2880U);
}

// §4.4.1.1: an axis of length 0 means no data either.
TEST(conformance_file_structure, an_axis_of_length_0_has_no_data)
{
    const reader input = open(fixture().header(primary_records(16, {100, 0})).image({4}));
    EXPECT_EQ(input.hdu(0).data_size(), 0U);
    EXPECT_TRUE(input.hdu(0).diagnostics().empty());
    EXPECT_EQ(input.hdu(1).kind(), hdu_kind::image);
}

// §3.4.1, §4.4.1.2, Eq. 2: an extension of |BITPIX| x GCOUNT x (PCOUNT + NAXIS1 x ... x NAXISn) bits.
TEST(conformance_file_structure, an_extension_has_the_size_of_its_axes_and_counts)
{
    fixture file;
    file.primary();
    file.header(extension_records("BINTABLE", 8, {12, 10}, 100, 1)).data(220);
    file.header(extension_records("FOREIGN", 16, {6}, 4, 3)).data(60);
    file.header(extension_records("IMAGE", -64, {2, 2})).data(32);
    const reader input = open(file);
    ASSERT_EQ(input.hdu_count(), 4U);
    EXPECT_EQ(input.hdu(1).data_size(), 220U);
    EXPECT_EQ(input.hdu(2).data_size(), 60U);
    EXPECT_EQ(input.hdu(3).data_size(), 32U);
    EXPECT_EQ(input.hdu(3).header_offset(), 5U * 2880U);
}

// §6.1.1, Eq. 4: random groups, GROUPS = T with NAXIS1 = 0: GCOUNT groups of PCOUNT parameters and an array.
TEST(conformance_file_structure, random_groups_have_the_size_of_their_groups)
{
    const reader input =
        open(fixture()
                 .header(with(primary_records(-32, {0, 7, 5}),
                              {card_record("GROUPS", "T"), card_record("PCOUNT", "4"), card_record("GCOUNT", "3")}))
                 .data(std::uint64_t{4} * 3 * (4 + 35)));
    const hdu_info& groups = input.hdu(0);
    EXPECT_EQ(groups.kind(), hdu_kind::random_groups);
    EXPECT_EQ(groups.data_size(), 4U * 3U * (4U + 35U));
    EXPECT_TRUE(groups.diagnostics().empty());

    // Without GROUPS = T, NAXIS1 = 0 is an array without data.
    const reader plain = open(fixture().header(with(primary_records(8, {0, 7}), {card_record("GCOUNT", "3")})));
    EXPECT_EQ(plain.hdu(0).kind(), hdu_kind::primary);
    EXPECT_EQ(plain.hdu(0).data_size(), 0U);

    // A group of parameters alone, NAXIS = 1, holds one element as well, the empty product of its axes.
    const reader parameters =
        open(fixture()
                 .header(with(primary_records(8, {0}),
                              {card_record("GROUPS", "T"), card_record("PCOUNT", "2"), card_record("GCOUNT", "5")}))
                 .data(15));
    EXPECT_EQ(parameters.hdu(0).data_size(), 15U);

    const reader missing = open(fixture().header(with(primary_records(8, {0, 2}), {card_record("GROUPS", "T")})));
    EXPECT_EQ(missing.hdu(0).data_size(), std::nullopt);
    EXPECT_TRUE(has(missing.hdu(0).diagnostics(), errc::missing_keyword, severity::error));
}

// §3.4.1, §7: the kinds of the standard's extensions, and those of tiled compression (§10).
TEST(conformance_file_structure, extensions_have_the_kind_of_their_type)
{
    fixture file;
    file.primary();
    file.header(extension_records("IMAGE"));
    file.header(extension_records("TABLE", 8, {10, 1}, 0, 1)).data(10);
    file.header(extension_records("BINTABLE", 8, {8, 1}, 0, 1)).data(8);
    file.header(with(extension_records("BINTABLE", 8, {8, 1}, 0, 1), {card_record("ZIMAGE", "T")})).data(8);
    file.header(with(extension_records("BINTABLE", 8, {8, 1}, 0, 1), {card_record("ZTABLE", "T")})).data(8);
    file.header(with(extension_records("BINTABLE", 8, {8, 1}, 0, 1), {card_record("ZIMAGE", "F")})).data(8);
    const reader input = open(file);
    const std::vector<hdu_kind> kinds{
        hdu_kind::primary,          hdu_kind::image,   hdu_kind::table, hdu_kind::bintable, hdu_kind::compressed_image,
        hdu_kind::compressed_table, hdu_kind::bintable};
    ASSERT_EQ(input.hdu_count(), kinds.size());
    for (std::size_t i = 0; i < kinds.size(); ++i) {
        EXPECT_EQ(input.hdu(i).kind(), kinds[i]) << i;
        EXPECT_TRUE(input.hdu(i).diagnostics().empty()) << i;
    }
}

// §3.4.1, Appendix F: an extension of another type, the reserved names included, is walked by its size.
TEST(conformance_file_structure, unknown_extensions_are_walked_by_their_size)
{
    fixture file;
    file.primary();
    for (const char* type : {"IUEIMAGE", "A3DTABLE", "FOREIGN", "DUMP", "MYTYPE"}) {
        file.header(extension_records(type, 8, {100}, 0, 1)).data(100);
    }
    file.image({1});
    const reader input = open(file);
    ASSERT_EQ(input.hdu_count(), 7U);
    for (std::size_t i = 1; i <= 5; ++i) {
        EXPECT_EQ(input.hdu(i).kind(), hdu_kind::unknown) << i;
        EXPECT_EQ(input.hdu(i).data_size(), 100U) << i;
    }
    EXPECT_EQ(input.hdu(6).kind(), hdu_kind::image);
    std::vector<std::byte> bytes(100);
    input.read_raw(5, 0, bytes);
    EXPECT_EQ(bytes[99], std::byte{99});
}

// §4.4.1: a mandatory keyword missing leaves the unit without a size, which stops the walk.
TEST(conformance_file_structure, a_missing_mandatory_keyword_stops_the_walk)
{
    const auto without = [](std::vector<std::string> records, std::size_t index) {
        records.erase(records.begin() + static_cast<std::ptrdiff_t>(index));
        return records;
    };
    const std::vector<std::string> primary = primary_records(8, {2, 2});
    for (std::size_t index = 1; index < primary.size(); ++index) {
        const reader input = open(fixture().header(without(primary, index)).data(4));
        EXPECT_EQ(input.hdu(0).data_size(), std::nullopt) << index;
        EXPECT_TRUE(has(input.hdu(0).diagnostics(), errc::missing_keyword, severity::error)) << index;
        EXPECT_EQ(input.hdu_count(), 1U) << index;
    }
    const std::vector<std::string> table = extension_records("BINTABLE", 8, {2, 2}, 0, 1);
    for (std::size_t index = 1; index < table.size(); ++index) {
        const reader input = open(fixture().primary().header(without(table, index)).data(4));
        EXPECT_EQ(input.hdu(1).data_size(), std::nullopt) << index;
        EXPECT_TRUE(has(input.hdu(1).diagnostics(), errc::missing_keyword, severity::error)) << index;
    }
}

// §4.4.1: the mandatory keywords in their order; another order is read with a warning.
TEST(conformance_file_structure, mandatory_keywords_out_of_order_are_read)
{
    std::vector<std::string> records = primary_records(16, {2, 3});
    std::swap(records[3], records[4]);
    records.insert(records.begin() + 2, card_record("OBJECT", "'early'"));
    const reader input = open(fixture().header(records).data(12));
    EXPECT_EQ(input.hdu(0).data_size(), 12U);
    EXPECT_TRUE(has(input.hdu(0).diagnostics(), errc::keyword_order, severity::warning));

    std::vector<std::string> extension = extension_records("IMAGE", 8, {4});
    std::swap(extension[4], extension[5]);
    const reader moved = open(fixture().primary().header(extension).data(4));
    EXPECT_EQ(moved.hdu(1).data_size(), 4U);
    EXPECT_TRUE(has(moved.hdu(1).diagnostics(), errc::keyword_order, severity::warning));
}

// §4.4.1: a mandatory keyword of the wrong type, or of a value outside its range, leaves no size.
TEST(conformance_file_structure, a_mandatory_keyword_of_the_wrong_value_stops_the_walk)
{
    for (const auto& [keyword, value] : std::vector<std::pair<std::string, std::string>>{{"BITPIX", "12"},
                                                                                         {"BITPIX", "T"},
                                                                                         {"BITPIX", "'x'"},
                                                                                         {"NAXIS", "-1"},
                                                                                         {"NAXIS", "1000"},
                                                                                         {"NAXIS1", "-5"},
                                                                                         {"NAXIS1", "2.5"},
                                                                                         {"NAXIS1", "(1, 2)"},
                                                                                         {"NAXIS1", ""}}) {
        std::vector<std::string> records = primary_records(8, {2});
        for (std::string& text : records) {
            if (text.starts_with(keyword + std::string(8 - keyword.size(), ' '))) {
                text = card_record(keyword, value);
            }
        }
        const reader input = open(fixture().header(records).data(2));
        EXPECT_EQ(input.hdu(0).data_size(), std::nullopt) << keyword << " = " << value;
        EXPECT_TRUE(std::ranges::any_of(input.hdu(0).diagnostics(),
                                        [](const diagnostic& found) { return found.severity == severity::error; }))
            << keyword << " = " << value;
    }
    // An integral real or a string that holds an integer is read, with a warning.
    std::vector<std::string> records = primary_records(8, {2});
    records[3] = card_record("NAXIS1", "2.0");
    const reader integral = open(fixture().header(records).data(2));
    EXPECT_EQ(integral.hdu(0).data_size(), 2U);
    EXPECT_TRUE(has(integral.hdu(0).diagnostics(), errc::keyword_type, severity::warning));
}

// §4.4.1.2: PCOUNT and GCOUNT of an extension other than IMAGE are mandatory, and not negative.
TEST(conformance_file_structure, extension_counts_are_mandatory_and_not_negative)
{
    const reader negative = open(fixture().primary().header(extension_records("BINTABLE", 8, {4, 1}, 0, 1)));
    EXPECT_EQ(negative.hdu(1).data_size(), 4U);
    std::vector<std::string> records = extension_records("BINTABLE", 8, {4, 1}, 0, 1);
    records[5] = card_record("PCOUNT", "-1");
    const reader broken = open(fixture().primary().header(records).data(4));
    EXPECT_EQ(broken.hdu(1).data_size(), std::nullopt);
    EXPECT_TRUE(has(broken.hdu(1).diagnostics(), errc::invalid_structure, severity::error));
}

// §7.1.1, §7.2.1, §7.3.1: the counts that IMAGE, TABLE and BINTABLE must have.
TEST(conformance_file_structure, the_counts_of_the_standard_extensions_have_their_values)
{
    const reader image = open(fixture().primary().header(extension_records("IMAGE", 8, {4}, 2, 2)).data(12));
    EXPECT_EQ(image.hdu(1).data_size(), 12U);
    EXPECT_TRUE(has(image.hdu(1).diagnostics(), errc::invalid_structure, severity::warning));
    const reader table = open(fixture().primary().header(extension_records("BINTABLE", 8, {4, 1}, 6, 2)).data(20));
    EXPECT_EQ(table.hdu(1).data_size(), 20U);
    EXPECT_TRUE(has(table.hdu(1).diagnostics(), errc::invalid_structure, severity::warning));
}

// §4.4.1.1: sizes beyond 64 bits leave no size.
TEST(conformance_file_structure, a_size_beyond_64_bits_stops_the_walk)
{
    const reader input =
        open(fixture().header(primary_records(64, {4294967296, 4294967296}))); // 2^64 elements of 8 bytes.
    EXPECT_EQ(input.hdu(0).data_size(), std::nullopt);
    EXPECT_TRUE(has(input.hdu(0).diagnostics(), errc::invalid_structure, severity::error));
}

// §3.1: a file is a sequence of blocks; a header ends with the block of its END (§4.4.1).
TEST(conformance_file_structure, a_header_ends_with_the_block_of_its_end)
{
    std::vector<std::string> records = primary_records();
    for (int i = 0; i < 32; ++i) {
        records.push_back(card_record("KEY" + std::to_string(i), "1"));
    }
    // 3 + 32 records and END fill the block.
    const reader full = open(fixture().header(records).image({1}));
    EXPECT_EQ(full.hdu(0).header_size(), 2880U);
    EXPECT_EQ(full.hdu(1).header_offset(), 2880U);
    records.push_back(card_record("ONE", "1"));
    const reader longer = open(fixture().header(records).image({1}));
    EXPECT_EQ(longer.hdu(0).header_size(), 5760U);
    EXPECT_EQ(longer.hdu(1).header_offset(), 5760U);
}

// §4.4.1: END is the record of the keyword END and spaces; a record whose keyword merely starts with END is not.
TEST(conformance_file_structure, end_is_a_keyword_of_its_own)
{
    const reader input = open(fixture().header(with(primary_records(), {card_record("ENDTIME", "1")})));
    EXPECT_EQ(input.hdu(0).header().size(), 4U);
    EXPECT_EQ(input.hdu(0).header().value<int>("ENDTIME"), 1);
}

// §3.5: special records after the last unit, which are not a header.
TEST(conformance_file_structure, special_records_end_the_walk)
{
    fixture file = fixture().primary();
    std::string special(2880, 'x');
    file.append(special);
    const reader input = open(file);
    EXPECT_EQ(input.hdu_count(), 1U);
    EXPECT_TRUE(has(input.diagnostics(), errc::special_records, severity::warning));
    EXPECT_TRUE(input.hdu(0).diagnostics().empty());
}

// §4.4.2.6: EXTNAME and EXTVER may name the primary HDU too.
TEST(conformance_file_structure, the_primary_header_may_have_a_name)
{
    const reader input =
        open(fixture()
                 .header(with(primary_records(), {card_record("EXTNAME", "'MAIN'"), card_record("EXTVER", "3")}))
                 .image({1}, {card_record("EXTNAME", "'MAIN'")}));
    EXPECT_EQ(input.hdu(0).name(), "MAIN");
    EXPECT_EQ(input.hdu(0).version(), 3);
    EXPECT_EQ(input.hdu("MAIN").index(), 0U);
    EXPECT_EQ(input.hdu("MAIN", 1).index(), 1U);
}

} // namespace
