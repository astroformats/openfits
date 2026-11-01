// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Standard §4.1 and Appendix A: keyword records, read with card::parse() and written with the writer of records. One
// test per production of Appendix A, which tests elsewhere complete: the values in value.cpp, the units of comments
// in units.cpp.

#include <openfits/card.h>
#include <openfits/error.h>

#include "header/record.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace {

using openfits::card;
using openfits::card_value;
using openfits::diagnostic;
using openfits::value_kind;
using openfits::detail::parse_record;
using openfits::detail::write_commentary_record;
using openfits::detail::write_value_record;

std::string record_of(std::string_view text)
{
    std::string record(text);
    record.resize(80, ' ');
    return record;
}

std::vector<diagnostic> departures_of(std::string_view text)
{
    std::vector<diagnostic> departures;
    (void)parse_record(record_of(text), departures);
    return departures;
}

// FITS keyword record: 80 characters, the constraint of every production.
TEST(conformance_keyword_record, every_record_has_80_characters)
{
    EXPECT_EQ(card::parse("SIMPLE  =                    T").text().size(), 80U);
    EXPECT_EQ(write_value_record("SIMPLE", true, {}, {}).size(), 80U);
    EXPECT_EQ(write_commentary_record("COMMENT", {}, {}).size(), 80U);
    EXPECT_EQ(write_value_record("OBJECT", std::string(68, 'x'), {}, {}).size(), 80U);
}

// keyword field: [keyword char...] [space...], eight characters, left-justified (standard §4.1.2.1).
TEST(conformance_keyword_record, the_keyword_field_is_the_name_padded_to_eight_characters)
{
    for (const char* name : {"A", "NAXIS1", "DATE-OBS", "TTYPE999", "12345678", "-_09"}) {
        const std::string record = write_value_record(name, 1, {}, {});
        std::string field(name);
        field.resize(8, ' ');
        EXPECT_EQ(record.substr(0, 8), field) << name;
        EXPECT_EQ(card::parse(record).keyword(), name) << name;
        EXPECT_TRUE(departures_of(record).empty()) << name;
    }
}

// value indicator: '=' space, in bytes 9 and 10 (standard §4.1.2.2).
TEST(conformance_keyword_record, a_value_indicator_in_bytes_9_and_10_starts_the_value_field)
{
    const card valued = card::parse("NAXIS   =                    2");
    EXPECT_TRUE(valued.has_value_indicator());
    EXPECT_EQ(valued.value(), card_value(2));
    EXPECT_EQ(write_value_record("NAXIS", 2, {}, {}).substr(8, 2), "= ");
}

// FITS commentary keyword record: keyword field anychar but equal [ascii text char...]. Without the value indicator,
// bytes 9 to 80 are commentary text (standard §4.1.2.3).
TEST(conformance_keyword_record, a_record_without_value_indicator_is_commentary_text)
{
    const card text = card::parse("TITLE   The Whirlpool, in Ha");
    EXPECT_EQ(text.keyword(), "TITLE");
    EXPECT_FALSE(text.has_value_indicator());
    EXPECT_EQ(text.value().kind(), value_kind::none);
    EXPECT_EQ(text.comment(), "The Whirlpool, in Ha");
}

// FITS commentary keyword record: keyword field '=' anychar but space [ascii text char...]. Read as a value with a
// warning, since the meaning is clear: an equals sign in byte 9 without its space.
TEST(conformance_keyword_record, an_equals_sign_without_its_space_is_read_as_a_value_with_a_warning)
{
    const card value = card::parse("OBJECT  ='M 31'");
    EXPECT_TRUE(value.has_value_indicator());
    EXPECT_EQ(value.value(), card_value("M 31"));
    const std::vector<diagnostic> departures = departures_of("OBJECT  ='M 31'");
    ASSERT_EQ(departures.size(), 1U);
    EXPECT_EQ(departures.front().code, openfits::errc::invalid_value_indicator);
}

// COMMENT keyword, HISTORY keyword, BLANKFIELD keyword: no value even with '= ' in bytes 9 and 10 (standard
// §4.4.2.4).
TEST(conformance_keyword_record, commentary_keywords_have_no_value_whatever_bytes_9_and_10_hold)
{
    for (const char* text : {"COMMENT = this is no value", "HISTORY = nor is this", "        = nor this"}) {
        const card commentary = card::parse(text);
        EXPECT_FALSE(commentary.has_value_indicator()) << text;
        EXPECT_EQ(commentary.value().kind(), value_kind::none) << text;
        EXPECT_EQ(commentary.comment(), std::string_view(text).substr(8)) << text;
        EXPECT_TRUE(departures_of(text).empty()) << text;
    }
    EXPECT_EQ(card::parse("COMMENT").keyword(), "COMMENT");
    EXPECT_EQ(card::parse("HISTORY   indented text").comment(), "  indented text");
    EXPECT_EQ(card::parse("").keyword(), "");
    EXPECT_EQ(write_commentary_record("HISTORY", "Dark subtracted", {}), record_of("HISTORY Dark subtracted"));
    EXPECT_EQ(write_commentary_record("", "a break", {}), record_of("        a break"));
}

// FITS value keyword record: keyword field, value indicator [space...] [value] [space...] [comment].
TEST(conformance_keyword_record, a_value_record_has_a_value_and_an_optional_comment)
{
    const card with_comment = card::parse("EXPTIME =                300.0 / exposure time");
    EXPECT_EQ(with_comment.value(), card_value(300.0));
    EXPECT_EQ(with_comment.comment(), "exposure time");

    const card without_comment = card::parse("EXPTIME =                300.0");
    EXPECT_EQ(without_comment.comment(), "");

    // The value may be preceded by any number of spaces, and the comment follows any number of them.
    const card spaced = card::parse("EXPTIME =  300.0         /exposure time");
    EXPECT_EQ(spaced.value(), card_value(300.0));
    EXPECT_EQ(spaced.comment(), "exposure time");
}

// FITS value keyword record without a value: "If the value field is not present, the value of the FITS keyword is not
// defined" (standard §4.1.2.3).
TEST(conformance_keyword_record, a_value_field_of_spaces_is_the_undefined_value)
{
    for (const char* text : {"KEYWORD3=", "KEYWORD3= / undefined keyword", "KEYWORD3=                      "}) {
        const card undefined = card::parse(text);
        EXPECT_TRUE(undefined.has_value_indicator()) << text;
        EXPECT_EQ(undefined.value().kind(), value_kind::undefined) << text;
    }
    EXPECT_EQ(card::parse("KEYWORD3= / undefined keyword").comment(), "undefined keyword");
    EXPECT_EQ(write_value_record("KEYWORD3", openfits::undefined, {}, {}), record_of("KEYWORD3="));
    EXPECT_EQ(write_value_record("KEYWORD3", openfits::undefined, "undefined keyword", {}),
              record_of("KEYWORD3=                      / undefined keyword"));
}

// comment: '/' [ascii text char...]; a space between the value and the slash is strongly recommended (standard
// §4.1.2.3), and the writer puts one on each side.
TEST(conformance_keyword_record, a_comment_follows_a_slash)
{
    EXPECT_EQ(card::parse("NAXIS   =                    2 /number of axes").comment(), "number of axes");
    EXPECT_EQ(card::parse("NAXIS   =                    2 /  two spaces kept").comment(), " two spaces kept");
    EXPECT_EQ(card::parse("NAXIS   =                    2 /").comment(), "");
    EXPECT_EQ(write_value_record("NAXIS", 2, "number of axes", {}),
              record_of("NAXIS   =                    2 / number of axes"));
}

// ascii text char: space to '~'. The control characters and DEL must not appear in a record (standard §4.1.2.3): a
// reader keeps them with a warning, and the writer refuses them.
TEST(conformance_keyword_record, records_hold_the_printable_ascii_characters)
{
    std::string every;
    for (char c = ' '; c <= '~'; ++c) {
        if (c != '\'') {
            every += c;
        }
    }
    const std::string first = every.substr(0, 68);
    EXPECT_TRUE(departures_of(write_value_record("TEXT", first, {}, {})).empty());
    EXPECT_EQ(card::parse(write_value_record("TEXT", first, {}, {})).value(), card_value(first));
    EXPECT_FALSE(departures_of("COMMENT \x7F").empty());
    EXPECT_FALSE(departures_of("COMMENT \x1F").empty());
}

// CONTINUE keyword, continuation kwd record: CONTINUE, spaces in bytes 9 and 10, then a string anywhere in bytes 11 to
// 80 (standard §4.2.1.2).
TEST(conformance_keyword_record, a_continuation_record_holds_a_partial_string)
{
    const card continued = card::parse("CONTINUE  'ollowed by cloudy skies overnight.&'");
    EXPECT_EQ(continued.keyword(), "CONTINUE");
    EXPECT_FALSE(continued.has_value_indicator());
    EXPECT_EQ(continued.value(), card_value("ollowed by cloudy skies overnight.&"));

    // last continuation record: a character string value, here the null string of a continued comment.
    const card last = card::parse("CONTINUE  '' / over multiple records.");
    EXPECT_EQ(last.value(), card_value(""));
    EXPECT_EQ(last.comment(), "over multiple records.");

    // The substring may be preceded by spaces.
    EXPECT_EQ(card::parse("CONTINUE        ' Low 21C.'").value(), card_value(" Low 21C."));
}

} // namespace
