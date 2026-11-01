// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Standard §4.2.7, §4.4.2.1 and §9.1.1: datetime strings, the values of DATE, DATE-OBS and the other date keywords.

#include <openfits/card.h>
#include <openfits/error.h>

#include "support/throws.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace {

using openfits::date_time;
using openfits::errc;
using openfits::format_date;
using openfits::parse_date;
using openfits::test::throws;

testing::AssertionResult invalid(const std::string& text)
{
    return throws<openfits::invalid_data_error>(errc::invalid_date, [&text] { (void)parse_date(text); });
}

// CCYY-MM-DD[Thh:mm:ss[.s...]]: the date alone, or with a time of two-digit fields.
TEST(conformance_datetime, a_date_has_a_four_digit_year_and_an_optional_time)
{
    EXPECT_EQ(parse_date("2026-10-26"), (date_time{.year = 2026, .month = 10, .day = 26}));
    EXPECT_EQ(
        parse_date("2026-10-26T21:07:05"),
        (date_time{.year = 2026, .month = 10, .day = 26, .has_time = true, .hour = 21, .minute = 7, .second = 5}));
    EXPECT_EQ(format_date(date_time{.year = 2026, .month = 10, .day = 26}), "2026-10-26");
    EXPECT_EQ(format_date(
                  date_time{.year = 1999, .month = 1, .day = 2, .has_time = true, .hour = 3, .minute = 4, .second = 5}),
              "1999-01-02T03:04:05");
}

// The decimal part of the seconds is optional and may be arbitrarily long (standard §4.4.2.1); nanoseconds are kept.
TEST(conformance_datetime, the_fraction_of_the_second_has_any_number_of_digits)
{
    EXPECT_EQ(parse_date("2026-10-26T21:07:05.5").nanosecond, 500'000'000U);
    EXPECT_EQ(parse_date("2026-10-26T21:07:05.123456789").nanosecond, 123'456'789U);
    EXPECT_EQ(parse_date("2026-10-26T21:07:05.1234567891234567890123").nanosecond, 123'456'789U);
    EXPECT_EQ(parse_date("2026-10-26T21:07:05.000").nanosecond, 0U);

    date_time value{.year = 2026, .month = 10, .day = 26, .has_time = true, .hour = 21, .minute = 7, .second = 5};
    value.nanosecond = 500'000'000;
    EXPECT_EQ(format_date(value), "2026-10-26T21:07:05.5");
    value.nanosecond = 1;
    EXPECT_EQ(format_date(value), "2026-10-26T21:07:05.000000001");
    value.nanosecond = 0;
    EXPECT_EQ(format_date(value), "2026-10-26T21:07:05");
}

// The earliest and latest dates of the four-digit form, and of the signed five-digit form; the origin of JD.
TEST(conformance_datetime, the_signed_five_digit_year_extends_the_range)
{
    EXPECT_EQ(parse_date("0000-01-01T00:00:00").year, 0);
    EXPECT_EQ(parse_date("9999-12-31T23:59:59").year, 9999);
    EXPECT_EQ(parse_date("-99999-01-01T00:00:00").year, -99'999);
    EXPECT_EQ(parse_date("+99999-12-31T23:59:59").year, 99'999);
    const date_time jd_origin = parse_date("-04713-11-24T12:00:00");
    EXPECT_EQ(jd_origin.year, -4713);
    EXPECT_EQ(jd_origin.hour, 12U);
    EXPECT_EQ(parse_date("+02026-10-26").year, 2026);

    EXPECT_EQ(format_date(date_time{.year = -4713, .month = 11, .day = 24, .has_time = true, .hour = 12}),
              "-04713-11-24T12:00:00");
    EXPECT_EQ(format_date(date_time{.year = 10'000, .month = 1, .day = 1}), "+10000-01-01");
    EXPECT_EQ(format_date(date_time{.year = 0, .month = 1, .day = 1}), "0000-01-01");
    EXPECT_EQ(format_date(date_time{.year = -1, .month = 12, .day = 31}), "-00001-12-31");
}

// The form of files written before 2000: DD/MM/YY, the year in 1900 to 1999 (standard §4.4.2.1). Read, never written.
TEST(conformance_datetime, the_form_of_files_written_before_2000_is_read)
{
    EXPECT_EQ(parse_date("31/12/99"), (date_time{.year = 1999, .month = 12, .day = 31}));
    EXPECT_EQ(parse_date("01/01/00"), (date_time{.year = 1900, .month = 1, .day = 1}));
    EXPECT_EQ(format_date(parse_date("26/10/96")), "1996-10-26");
    EXPECT_TRUE(invalid("29/02/00"));
    EXPECT_TRUE(invalid("1/1/99"));
    EXPECT_TRUE(invalid("01/01/1999"));
}

// Every field at its boundaries: days of each month and of leap years, 24:00 refused, the leap second of UTC accepted.
TEST(conformance_datetime, every_field_is_checked_at_its_boundaries)
{
    EXPECT_TRUE(invalid("2026-00-01"));
    EXPECT_TRUE(invalid("2026-13-01"));
    EXPECT_TRUE(invalid("2026-01-00"));
    EXPECT_TRUE(invalid("2026-01-32"));
    EXPECT_NO_THROW((void)parse_date("2026-04-30"));
    EXPECT_TRUE(invalid("2026-04-31"));
    EXPECT_NO_THROW((void)parse_date("2024-02-29"));
    EXPECT_NO_THROW((void)parse_date("2000-02-29"));
    EXPECT_TRUE(invalid("1900-02-29"));
    EXPECT_TRUE(invalid("2026-02-29"));
    EXPECT_NO_THROW((void)parse_date("2026-10-26T23:59:59"));
    EXPECT_TRUE(invalid("2026-10-26T24:00:00"));
    EXPECT_TRUE(invalid("2026-10-26T23:60:00"));
    EXPECT_EQ(parse_date("2016-12-31T23:59:60").second, 60U);
    EXPECT_TRUE(invalid("2016-12-31T23:59:61"));
}

// Leading zeros must not be omitted, and time zones are not allowed: no Z, no offset (standard §9.1.1).
TEST(conformance_datetime, other_forms_are_refused)
{
    for (const char* text : {"",
                             "2026",
                             "2026-10",
                             "26-10-26",
                             "2026-1-26",
                             "2026-10-6",
                             "2026-10-26T",
                             "2026-10-26T21:07",
                             "2026-10-26T21:07:5",
                             "2026-10-26T21:07:05.",
                             "2026-10-26T21:07:05Z",
                             "2026-10-26T21:07:05+01:00",
                             "2026-10-26 21:07:05",
                             "2026-10-26t21:07:05",
                             " 2026-10-26",
                             "2026-10-26 ",
                             "12026-10-26",
                             "-2026-10-26",
                             "+2026-10-26",
                             "+100000-01-01",
                             "2026/10/26"}) {
        EXPECT_TRUE(invalid(text)) << text;
    }
}

TEST(conformance_datetime, a_date_time_out_of_its_ranges_is_not_formatted)
{
    const auto refused = [](const date_time& value) {
        return throws<openfits::usage_error>(errc::invalid_date, [&value] { (void)format_date(value); });
    };
    EXPECT_TRUE(refused(date_time{.year = 100'000, .month = 1, .day = 1}));
    EXPECT_TRUE(refused(date_time{.year = 2026, .month = 2, .day = 29}));
    EXPECT_TRUE(refused(date_time{.year = 2026, .month = 1, .day = 1, .has_time = true, .hour = 24}));
    EXPECT_TRUE(refused(date_time{.year = 2026, .month = 1, .day = 1, .has_time = true, .nanosecond = 1'000'000'000}));
    // The members of the time are zero for a date alone.
    EXPECT_TRUE(refused(date_time{.year = 2026, .month = 1, .day = 1, .hour = 5}));
}

// DATE in UTC from a time the caller gives: the library reads no clock.
TEST(conformance_datetime, a_time_point_of_the_system_clock_gives_its_date_in_utc)
{
    using namespace std::chrono_literals;
    const std::chrono::sys_days day = std::chrono::sys_days(std::chrono::days(20'752));
    EXPECT_EQ(format_date(day + 21h + 7min + 5s), "2026-10-26T21:07:05");
    EXPECT_EQ(format_date(day + 21h + 7min + 5s + 250ms), "2026-10-26T21:07:05.25");
    EXPECT_EQ(format_date(std::chrono::floor<std::chrono::seconds>(day + 999'999'999ns)), "2026-10-26T00:00:00");
    EXPECT_EQ(format_date(std::chrono::sys_days(std::chrono::days(0))), "1970-01-01T00:00:00");
    // Before 1970, the second and the day fall below.
    EXPECT_EQ(format_date(std::chrono::sys_days(std::chrono::days(0)) - 1ns), "1969-12-31T23:59:59.999999999");

    const date_time value = openfits::to_date_time(day + 1h);
    EXPECT_TRUE(value.has_time);
    EXPECT_EQ(value.hour, 1U);
    EXPECT_EQ(format_date(value), "2026-10-26T01:00:00");
}

// What format_date() writes, parse_date() reads back as the same value.
TEST(conformance_datetime, a_date_formatted_reads_back_as_itself)
{
    for (const char* text : {"2026-10-26", "2026-10-26T21:07:05", "2026-10-26T21:07:05.000000001", "-99999-01-01",
                             "+99999-12-31T23:59:60.999999999", "0000-02-29", "1582-10-15T00:00:00"}) {
        EXPECT_EQ(format_date(parse_date(text)), text);
        EXPECT_EQ(parse_date(format_date(parse_date(text))), parse_date(text));
    }
}

} // namespace
