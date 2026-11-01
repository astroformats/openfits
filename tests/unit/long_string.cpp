// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "header/long_string.h"

#include <openfits/card.h>
#include <openfits/error.h>

#include "support/throws.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace {

using openfits::card;
using openfits::errc;
using openfits::validation_error;
using openfits::detail::card_records;
using openfits::detail::continuation_length;
using openfits::detail::is_continuation;
using openfits::detail::joined_comment;
using openfits::detail::joined_string;
using openfits::test::throws;

std::vector<card> parsed(std::initializer_list<std::string_view> records)
{
    std::vector<card> cards;
    for (const std::string_view text : records) {
        cards.push_back(card::parse(text));
    }
    return cards;
}

// The records read back: the string they make and its comment.
struct read_back
{
    std::string value;
    std::string comment;
};

read_back reread(const std::vector<std::string>& records)
{
    std::vector<card> cards;
    for (const std::string& record : records) {
        EXPECT_EQ(record.size(), 80U);
        cards.push_back(card::parse(record));
    }
    EXPECT_EQ(continuation_length(cards, 0), records.size() - 1);
    return {.value = joined_string(cards, 0), .comment = joined_comment(cards, 0)};
}

std::vector<std::string> records_of(std::string_view keyword, const std::string& value, std::string_view comment = {})
{
    return card_records(card(keyword, value, comment), {.hierarch = true});
}

TEST(long_string, recognizes_conforming_continue_records)
{
    EXPECT_TRUE(is_continuation(card::parse("CONTINUE  'abc'")));
    EXPECT_TRUE(is_continuation(card::parse("CONTINUE        ''  / comment")));
    EXPECT_FALSE(is_continuation(card::parse("CONTINUE= 'abc'")));
    EXPECT_FALSE(is_continuation(card::parse("CONTINUE  123")));
    EXPECT_FALSE(is_continuation(card::parse("CONTINUE")));
    EXPECT_FALSE(is_continuation(card::parse("COMMENT   'abc'")));
}

TEST(long_string, follows_the_ampersands_of_a_string)
{
    const std::vector<card> cards =
        parsed({"A       = 'one&'", "CONTINUE  'two&  '", "CONTINUE  'three'", "CONTINUE  'four'", "B       = 'x&'",
                "COMMENT   'y'", "C       = 'z'", "CONTINUE  'w'"});
    EXPECT_EQ(continuation_length(cards, 0), 2U);
    EXPECT_EQ(joined_string(cards, 0), "onetwothree");
    EXPECT_EQ(continuation_length(cards, 4), 0U);
    EXPECT_EQ(joined_string(cards, 4), "x&");
    EXPECT_EQ(continuation_length(cards, 6), 0U);
    EXPECT_EQ(continuation_length(cards, 7), 0U);
    EXPECT_EQ(continuation_length(parsed({"A       = 'one&'"}), 0), 0U);
}

TEST(long_string, joins_comments_as_their_records_split_them)
{
    // The example of standard §4.2.1.2: comments split between words, records that end before byte 80.
    const std::vector<card> example =
        parsed({"STRKEY  = 'This keyword value is continued &'", "CONTINUE  ' over multiple keyword records.&'",
                "CONTINUE  '&' / The comment field for this", "CONTINUE  '&' / keyword is also continued",
                "CONTINUE  '' / over multiple records."});
    EXPECT_EQ(joined_string(example, 0), "This keyword value is continued  over multiple keyword records.");
    EXPECT_EQ(joined_comment(example, 0),
              "The comment field for this keyword is also continued over multiple records.");

    // A comment split inside a word fills its record to byte 80.
    const std::vector<card> full =
        parsed({"KEY     = 'a&' / first", "CONTINUE  '&' / " + std::string(64, 'x'), "CONTINUE  '' / yz"});
    EXPECT_EQ(joined_comment(full, 0), "first " + std::string(64, 'x') + "yz");
    EXPECT_EQ(joined_comment(parsed({"KEY     = 'a'   / alone"}), 0), "alone");
}

TEST(long_string, writes_a_card_that_fits_as_one_record)
{
    const std::vector<std::string> records = records_of("OBJECT", "M 31", "galaxy");
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0], card::parse("OBJECT  = 'M 31    '           / galaxy").text());
    const card read = card::parse("OBJECT  = 'x' / read");
    EXPECT_EQ(card_records(read, {}), std::vector<std::string>{std::string(read.text())});
}

TEST(long_string, continues_a_long_string_over_full_records)
{
    std::string value;
    for (int i = 0; i < 200; ++i) {
        value += static_cast<char>('a' + (i % 26));
    }
    const std::vector<std::string> records = records_of("LONG", value);
    ASSERT_EQ(records.size(), 3U);
    EXPECT_EQ(records[0], "LONG    = '" + value.substr(0, 67) + "&'");
    EXPECT_EQ(records[1], "CONTINUE  '" + value.substr(67, 67) + "&'");
    EXPECT_EQ(records[2], "CONTINUE  '" + value.substr(134) + "'  ");
    EXPECT_EQ(reread(records).value, value);
}

TEST(long_string, never_splits_a_doubled_quote)
{
    for (std::size_t at = 60; at < 72; ++at) {
        std::string value(100, 'q');
        value[at] = '\'';
        const std::vector<std::string> records = records_of("QUOTES", value);
        for (const std::string& record : records) {
            EXPECT_LE(record.find_last_not_of(' '), 79U);
        }
        EXPECT_EQ(reread(records).value, value) << at;
    }
    const std::string quotes(150, '\'');
    EXPECT_EQ(reread(records_of("QUOTES", quotes)).value, quotes);
}

TEST(long_string, puts_the_comment_on_the_last_record_where_it_fits)
{
    const std::string value(100, 'v');
    const std::vector<std::string> records = records_of("KEY", value, "short");
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(records[1].substr(0, 11 + 33 + 1 + 8), "CONTINUE  '" + std::string(33, 'v') + "' / short");
    const read_back read = reread(records);
    EXPECT_EQ(read.value, value);
    EXPECT_EQ(read.comment, "short");
}

TEST(long_string, continues_a_long_comment_on_records_of_null_strings)
{
    const std::string value(100, 'v');
    std::string comment;
    for (int i = 0; i < 150; ++i) {
        comment += static_cast<char>('A' + (i % 26));
    }
    const std::vector<std::string> records = records_of("KEY", value, comment);
    ASSERT_EQ(records.size(), 5U);
    EXPECT_EQ(records[1], "CONTINUE  '" + std::string(33, 'v') + "&'" + std::string(34, ' '));
    EXPECT_EQ(records[2], "CONTINUE  '&' / " + comment.substr(0, 64));
    EXPECT_EQ(records[3], "CONTINUE  '&' / " + comment.substr(64, 64));
    EXPECT_EQ(records[4].substr(0, 15 + 22), "CONTINUE  '' / " + comment.substr(128));
    const read_back read = reread(records);
    EXPECT_EQ(read.value, value);
    EXPECT_EQ(read.comment, comment);
}

TEST(long_string, gives_the_last_piece_its_ampersand_when_the_comment_follows_apart)
{
    // The last piece fills its record: it moves on, so that its ampersand fits.
    const std::string value(67 + 68, 'w');
    const std::vector<std::string> records = records_of("KEY", value, std::string(70, 'c'));
    const read_back read = reread(records);
    EXPECT_EQ(read.value, value);
    EXPECT_EQ(read.comment, std::string(70, 'c'));
}

TEST(long_string, continues_the_string_of_a_hierarch_keyword)
{
    const std::string value(120, 'h');
    const std::vector<std::string> records = records_of("ESO INS PATH", value, "path");
    ASSERT_GE(records.size(), 2U);
    EXPECT_TRUE(records[0].starts_with("HIERARCH ESO INS PATH = '"));
    EXPECT_EQ(records[0].size(), 80U);
    EXPECT_EQ(records[0].back(), '\'');
    const read_back read = reread(records);
    EXPECT_EQ(read.value, value);
    EXPECT_EQ(read.comment, "path");
}

TEST(long_string, refuses_a_long_string_of_a_reserved_keyword)
{
    for (const char* keyword : {"OBJECT", "EXTNAME", "XTENSION", "CTYPE1", "CTYPE1A", "TTYPE3", "DATE-OBS", "ORIGIN"}) {
        EXPECT_TRUE(throws<validation_error>(errc::continued_reserved_keyword, [&] {
            (void)records_of(keyword, std::string(100, 'x'));
        })) << keyword;
    }
}

TEST(long_string, refuses_what_no_record_can_hold)
{
    EXPECT_TRUE(throws<validation_error>(errc::invalid_character,
                                         [] { (void)records_of("LONG", std::string(100, 'x') + '\n'); }));
    EXPECT_TRUE(throws<validation_error>(errc::invalid_character,
                                         [] { (void)records_of("LONG", std::string(100, 'x'), "tab\there"); }));
    EXPECT_TRUE(throws<validation_error>(errc::value_too_long,
                                         [] { (void)records_of(std::string(62, 'N'), std::string(100, 'x')); }));
    EXPECT_TRUE(throws<validation_error>(errc::invalid_keyword, [] {
        (void)card_records(card("LONG NAME", std::string(100, 'x')), {.hierarch = false});
    }));
    EXPECT_TRUE(
        throws<validation_error>(errc::comment_too_long, [] { (void)records_of("SHORT", "x", std::string(80, 'c')); }));
}

} // namespace
