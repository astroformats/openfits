// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>
#include <openfits/header.h>
#include <openfits/io.h>
#include <openfits/reader.h>

#include "header/emission.h"
#include "support/bytes.h"
#include "support/fixture_builder.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using openfits::card;
using openfits::errc;
using openfits::header;
using openfits::validation_error;
using openfits::detail::emission_options;
using openfits::detail::header_blocks;
using openfits::test::card_record;
using openfits::test::record;
using openfits::test::throws;

std::vector<card> structural_cards()
{
    return {card("SIMPLE", true), card("BITPIX", 16), card("NAXIS", 0)};
}

// The records of a block of a header, without trailing spaces.
std::vector<std::string> records_of(const std::string& blocks)
{
    std::vector<std::string> result;
    for (std::size_t i = 0; i < blocks.size(); i += 80) {
        const std::string text = blocks.substr(i, 80);
        result.push_back(text.substr(0, text.find_last_not_of(' ') + 1));
    }
    return result;
}

TEST(header_emission, writes_records_then_end_and_spaces_to_the_end_of_the_block)
{
    header caller;
    caller.set("EXPTIME", 300.0, "[s] exposure time");
    caller.set("OBJECT", "M 31");
    caller.add_history("made by a test");
    const std::string blocks = header_blocks(structural_cards(), caller, {});
    ASSERT_EQ(blocks.size(), 2880U);
    const std::vector<std::string> records = records_of(blocks);
    // The fixed format of standard §4.2: a value right-justified to byte 30, a string from byte 11, the comment
    // after byte 30.
    EXPECT_EQ(records[0], "SIMPLE  =                    T");
    EXPECT_EQ(records[1], "BITPIX  =                   16");
    EXPECT_EQ(records[2], "NAXIS   =                    0");
    EXPECT_EQ(records[3], "EXPTIME =                 300. / [s] exposure time");
    EXPECT_EQ(records[4], "OBJECT  = 'M 31    '");
    EXPECT_EQ(records[5], "HISTORY made by a test");
    EXPECT_EQ(records[6], "END");
    for (std::size_t i = 7; i < records.size(); ++i) {
        EXPECT_EQ(records[i], "") << i;
    }
}

TEST(header_emission, fills_the_last_block_exactly)
{
    header caller;
    for (int i = 0; i < 32; ++i) {
        caller.add_comment("card " + std::to_string(i));
    }
    // 3 + 32 records and END fill one block of 36; one more record takes a second block.
    EXPECT_EQ(header_blocks(structural_cards(), caller, {}).size(), 2880U);
    caller.add_comment("one more");
    EXPECT_EQ(header_blocks(structural_cards(), caller, {}).size(), 5760U);
}

TEST(header_emission, writes_reserved_cards_before_end)
{
    header caller;
    caller.set("OBJECT", "x");
    const std::string blocks = header_blocks(structural_cards(), caller, {.reserved_cards = 40});
    ASSERT_EQ(blocks.size(), 5760U);
    const std::vector<std::string> records = records_of(blocks);
    for (std::size_t i = 4; i < 44; ++i) {
        EXPECT_EQ(records[i], "") << i;
    }
    EXPECT_EQ(records[44], "END");
}

TEST(header_emission, leaves_out_the_fill_space_of_the_header)
{
    header caller;
    caller.set("OBJECT", "x");
    caller.append(card());
    caller.append(card());
    const std::vector<std::string> records = records_of(header_blocks(structural_cards(), caller, {}));
    EXPECT_EQ(records[4], "END");
}

TEST(header_emission, refuses_structural_keywords_in_the_header_of_the_caller)
{
    for (const char* keyword : {"SIMPLE", "BITPIX", "NAXIS2", "BZERO", "BLANK", "TFORM1", "ZCMPTYPE", "CHECKSUM"}) {
        header caller;
        caller.set(keyword, 1);
        EXPECT_TRUE(throws<validation_error>(errc::structural_keyword, [&] {
            (void)header_blocks(structural_cards(), caller, {});
        })) << keyword;
    }
}

TEST(header_emission, writes_long_strings_and_announces_them_on_request)
{
    header caller;
    caller.set("PATH", std::string(100, 'p'), "a path");
    const std::vector<std::string> plain = records_of(header_blocks(structural_cards(), caller, {}));
    EXPECT_TRUE(plain[3].starts_with("PATH    = '"));
    EXPECT_TRUE(plain[4].starts_with("CONTINUE  '"));
    EXPECT_EQ(plain[5], "END");

    const std::vector<std::string> announced =
        records_of(header_blocks(structural_cards(), caller, {.longstrn = true}));
    EXPECT_TRUE(announced[3].starts_with("LONGSTRN= 'OGIP 1.0'"));
    EXPECT_TRUE(announced[4].starts_with("PATH    = '"));

    header short_strings;
    short_strings.set("OBJECT", "x");
    EXPECT_EQ(records_of(header_blocks(structural_cards(), short_strings, {.longstrn = true}))[3],
              "OBJECT  = 'x       '");

    caller.set("LONGSTRN", "OGIP 1.0");
    const std::vector<std::string> once = records_of(header_blocks(structural_cards(), caller, {.longstrn = true}));
    EXPECT_TRUE(once[3].starts_with("PATH    = '"));
}

TEST(header_emission, refuses_what_the_records_cannot_hold)
{
    header named;
    named.set("A LONG NAME", 1);
    EXPECT_TRUE(
        throws<validation_error>(errc::invalid_keyword, [&] { (void)header_blocks(structural_cards(), named, {}); }));
    EXPECT_EQ(records_of(header_blocks(structural_cards(), named, {.records = {.hierarch = true}}))[3],
              "HIERARCH A LONG NAME = 1");

    header commented;
    commented.set("KEY", 1, std::string(60, 'c'));
    EXPECT_TRUE(throws<validation_error>(errc::comment_too_long,
                                         [&] { (void)header_blocks(structural_cards(), commented, {}); }));
    EXPECT_EQ(records_of(header_blocks(structural_cards(), commented, {.records = {.truncate_comment = true}}))[3],
              card_record("KEY", "1", std::string(47, 'c')).substr(0, 80));

    header reserved;
    reserved.set("OBJECT", std::string(100, 'o'));
    EXPECT_TRUE(throws<validation_error>(errc::continued_reserved_keyword,
                                         [&] { (void)header_blocks(structural_cards(), reserved, {}); }));

    header text;
    text.set("OBJECT", "caf\xC3\xA9");
    EXPECT_TRUE(
        throws<validation_error>(errc::invalid_character, [&] { (void)header_blocks(structural_cards(), text, {}); }));
}

TEST(header_emission, writes_a_header_read_from_a_file_back_byte_for_byte)
{
    using openfits::test::fixture;
    std::vector<std::string> records = openfits::test::primary_records(16, {});
    records.push_back(record("OBJECT  =   'free  format'  /comment without a space"));
    records.push_back(record("lower   =                    t"));
    records.push_back(record("HIERARCH ESO DET DIT=2.5"));
    records.push_back(record("LONG    = 'abc&'"));
    records.push_back(record("CONTINUE  'def' / end"));
    records.push_back(record("CONTINUE  'orphan'"));
    records.push_back(record("HISTORY   history text"));
    records.push_back(record(""));
    records.push_back(record(""));
    const fixture file = fixture().header(records);
    const openfits::reader input(std::make_unique<openfits::memory_source>(file.bytes()));
    const header& cards = input.hdu(0).header();
    ASSERT_EQ(cards.size(), records.size());
    const std::vector<card> structural(cards.begin(), cards.begin() + 3);
    const header caller(std::vector<card>(cards.begin() + 3, cards.end()));
    const std::string blocks = header_blocks(structural, caller, {.reserved_cards = caller.free_cards()});
    EXPECT_EQ(blocks, openfits::test::text(file.bytes()));
}

} // namespace
