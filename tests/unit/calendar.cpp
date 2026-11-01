// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "core/calendar.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

using openfits::detail::civil_date;
using openfits::detail::civil_from_days;
using openfits::detail::days_in_month;
using openfits::detail::is_leap_year;

TEST(calendar, leap_years_are_those_of_the_gregorian_calendar)
{
    EXPECT_TRUE(is_leap_year(2024));
    EXPECT_TRUE(is_leap_year(2000));
    EXPECT_TRUE(is_leap_year(0));
    EXPECT_TRUE(is_leap_year(-4));
    EXPECT_TRUE(is_leap_year(-400));
    EXPECT_FALSE(is_leap_year(2026));
    EXPECT_FALSE(is_leap_year(1900));
    EXPECT_FALSE(is_leap_year(-100));
    EXPECT_FALSE(is_leap_year(-1));
}

TEST(calendar, months_have_their_lengths)
{
    constexpr std::array<unsigned, 12> lengths{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (unsigned month = 1; month <= 12; ++month) {
        EXPECT_EQ(days_in_month(2026, month), lengths[month - 1]) << month;
    }
    EXPECT_EQ(days_in_month(2024, 2), 29U);
    EXPECT_EQ(days_in_month(1900, 2), 28U);
}

TEST(calendar, days_count_from_1970_in_both_directions)
{
    EXPECT_EQ(civil_from_days(0), (civil_date{.year = 1970, .month = 1, .day = 1}));
    EXPECT_EQ(civil_from_days(-1), (civil_date{.year = 1969, .month = 12, .day = 31}));
    EXPECT_EQ(civil_from_days(59), (civil_date{.year = 1970, .month = 3, .day = 1}));
    EXPECT_EQ(civil_from_days(11'016), (civil_date{.year = 2000, .month = 2, .day = 29}));
    EXPECT_EQ(civil_from_days(20'752), (civil_date{.year = 2026, .month = 10, .day = 26}));
    // The year 0 and the day before it, 1 BCE and 2 BCE.
    EXPECT_EQ(civil_from_days(-719'528), (civil_date{.year = 0, .month = 1, .day = 1}));
    EXPECT_EQ(civil_from_days(-719'529), (civil_date{.year = -1, .month = 12, .day = 31}));
}

// Every day of eight centuries around 1970, two cycles of the calendar, follows the one before it.
TEST(calendar, consecutive_days_give_consecutive_dates)
{
    civil_date previous = civil_from_days(-146'097);
    for (std::int64_t day = -146'096; day <= 146'097; ++day) {
        const civil_date date = civil_from_days(day);
        if (date.day == 1) {
            EXPECT_EQ(previous.day, days_in_month(previous.year, previous.month)) << day;
            EXPECT_EQ(date.month == 1 ? date.year - 1 : date.year, previous.year) << day;
            EXPECT_EQ(date.month, previous.month % 12 + 1) << day;
        } else {
            EXPECT_EQ(date.year, previous.year) << day;
            EXPECT_EQ(date.month, previous.month) << day;
            EXPECT_EQ(date.day, previous.day + 1) << day;
        }
        previous = date;
    }
}

} // namespace
