// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/card.h>
#include <openfits/error.h>

#include "support/throws.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using openfits::card;
using openfits::card_value;
using openfits::errc;
using openfits::value_kind;
using openfits::test::throws;

TEST(card, a_default_card_is_a_blank_card)
{
    const card blank;
    EXPECT_EQ(blank.keyword(), "");
    EXPECT_FALSE(blank.has_value_indicator());
    EXPECT_EQ(blank.value().kind(), value_kind::none);
    EXPECT_EQ(blank.comment(), "");
    EXPECT_EQ(blank.text(), "");
}

// Lookups use the canonical name, so a card built with another spelling is found by it.
TEST(card, a_built_card_keeps_its_keyword_in_canonical_form)
{
    EXPECT_EQ(card("exptime", 300.0).keyword(), "EXPTIME");
    EXPECT_EQ(card(" Date-Obs ", "2026-10-26").keyword(), "DATE-OBS");
    EXPECT_EQ(card("HIERARCH ESO  DET DIT", 1.5).keyword(), "ESO DET DIT");
    EXPECT_EQ(card("ESO DET DIT", 1.5).keyword(), "ESO DET DIT");
}

TEST(card, a_built_card_has_a_value_indicator_its_value_and_its_comment_and_no_record)
{
    const card exposure("EXPTIME", 300.0, "[s] exposure time");
    EXPECT_TRUE(exposure.has_value_indicator());
    EXPECT_EQ(exposure.value(), card_value(300.0));
    EXPECT_EQ(exposure.comment(), "[s] exposure time");
    EXPECT_EQ(exposure.text(), "");
    EXPECT_EQ(exposure.value_text(), "");
}

TEST(card, a_commentary_card_has_its_text_and_no_value)
{
    const card history = card::commentary("history", "Calibrated with a master dark");
    EXPECT_EQ(history.keyword(), "HISTORY");
    EXPECT_FALSE(history.has_value_indicator());
    EXPECT_EQ(history.value().kind(), value_kind::none);
    EXPECT_EQ(history.comment(), "Calibrated with a master dark");
}

TEST(card, parse_keeps_the_record_padded_to_80_characters)
{
    std::string record = "EXPTIME = 300.0 / [s] exposure time";
    const card exposure = card::parse(record);
    record.resize(80, ' ');
    EXPECT_EQ(exposure.text(), record);
    EXPECT_EQ(exposure.value_text(), "300.0");
    EXPECT_EQ(exposure.value(), card_value(300.0));
}

// A card of a value text keeps the text, which the writer writes as it is, and has no record of its own: the save makes
// it under its options (record_writer.a_value_text_is_written_as_it_is).
TEST(card, a_card_of_a_value_text_keeps_the_text)
{
    const card exposure = card::with_value_text("exposure", " 1.50E+03 ", "[s] time");
    EXPECT_EQ(exposure.keyword(), "EXPOSURE");
    EXPECT_TRUE(exposure.text().empty());
    EXPECT_TRUE(exposure.has_value_indicator());
    EXPECT_EQ(exposure.value_text(), "1.50E+03");
    EXPECT_EQ(exposure.value(), card_value(1500.0));
    EXPECT_EQ(exposure.comment(), "[s] time");
    EXPECT_EQ(exposure.unit(), "s");
    EXPECT_EQ(card::with_value_text("OBJECT", "'M 31'").value(), card_value("M 31"));
    EXPECT_EQ(card::with_value_text("BLANKED", "").value(), card_value(openfits::undefined));
    // A name of the HIERARCH convention, a comment of any length and bytes outside printable ASCII are the save's to
    // write or refuse, as its options say.
    const card hierarch = card::with_value_text("HIERARCH ESO DET DIT", "1.5", std::string(90, 'c') + "\xE9");
    EXPECT_EQ(hierarch.keyword(), "ESO DET DIT");
    EXPECT_EQ(hierarch.comment().size(), 91U);
    // A text of none of the forms, or of more than one value, has no value; the writer refuses it.
    for (const char* text : {"1,5", "1 / 2", "1 2"}) {
        const card wrong = card::with_value_text("GAIN", text);
        EXPECT_EQ(wrong.value_text(), text);
        EXPECT_EQ(wrong.value().kind(), value_kind::none) << text;
    }
    // A value text longer than a record is kept whole.
    const std::string long_text = "'" + std::string(300, 'x') + "'";
    EXPECT_EQ(card::with_value_text("NOTE", long_text).value_text(), long_text);

    for (const char* keyword : {"COMMENT", "HISTORY", "", "CONTINUE", "END"}) {
        EXPECT_TRUE(throws<openfits::usage_error>(errc::invalid_keyword, [&] {
            (void)card::with_value_text(keyword, "1");
        })) << keyword;
    }
}

TEST(card, parse_refuses_a_record_longer_than_80_characters)
{
    EXPECT_TRUE(throws<openfits::usage_error>(errc::invalid_argument, [] { (void)card::parse(std::string(81, ' ')); }));
    EXPECT_NO_THROW((void)card::parse(std::string(80, ' ')));
    EXPECT_NO_THROW((void)card::parse(""));
}

TEST(card, cards_compare_by_every_member_the_record_included)
{
    EXPECT_EQ(card("A", 1, "c"), card("a", 1, "c"));
    EXPECT_NE(card("A", 1, "c"), card("A", 2, "c"));
    EXPECT_NE(card("A", 1, "c"), card("A", 1, "d"));
    EXPECT_NE(card::parse("A       =                    1 / c"), card("A", 1, "c"));
    EXPECT_EQ(card::parse("A       =                    1 / c"), card::parse("A       =                    1 / c"));
}

} // namespace
