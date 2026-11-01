// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Standard §4.3.2: units in comment fields, in square brackets at the start of the comment.

#include <openfits/card.h>

#include <gtest/gtest.h>

#include <optional>
#include <string_view>

namespace {

using openfits::card;

// The example of the standard: EXPTIME = 1200. / [s] exposure time in seconds.
TEST(conformance_units, the_unit_is_in_square_brackets_at_the_start_of_the_comment)
{
    const card exposure = card::parse("EXPTIME =                1200. / [s] exposure time in seconds");
    EXPECT_EQ(exposure.unit(), "s");
    EXPECT_EQ(exposure.comment(), "[s] exposure time in seconds");

    EXPECT_EQ(card::parse("FOCUSPOS=                 1200 /[um] focuser position").unit(), "um");
    EXPECT_EQ(card::parse("SITEELEV=                 2400 / [m]").unit(), "m");
    EXPECT_EQ(card::parse("FLUX    =                  1.5 / [10**(-20) W m**-2 Hz**-1] flux").unit(),
              "10**(-20) W m**-2 Hz**-1");
    EXPECT_EQ(card::parse("RATIO   =                  1.5 / [] no unit").unit(), "");
}

// Software should not depend on units being written this way: any other comment has none.
TEST(conformance_units, a_comment_that_does_not_start_with_a_bracketed_unit_has_none)
{
    EXPECT_EQ(card::parse("EXPTIME =                1200. / exposure time [s]").unit(), std::nullopt);
    EXPECT_EQ(card::parse("EXPTIME =                1200. /  [s] after two spaces").unit(), std::nullopt);
    EXPECT_EQ(card::parse("EXPTIME =                1200. / [s exposure time").unit(), std::nullopt);
    EXPECT_EQ(card::parse("EXPTIME =                1200.").unit(), std::nullopt);
    // Commentary keywords have no value, so no unit.
    EXPECT_EQ(card::parse("COMMENT [s] a note").unit(), std::nullopt);
}

TEST(conformance_units, a_card_built_with_a_bracketed_unit_has_it)
{
    EXPECT_EQ(card("EXPTIME", 300.0, "[s] exposure time").unit(), "s");
    EXPECT_EQ(card("EXPTIME", 300.0, "exposure time").unit(), std::nullopt);
}

} // namespace
