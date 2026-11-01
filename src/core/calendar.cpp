// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "core/calendar.h"

namespace openfits::detail {

namespace {

// The calendar repeats every 400 years, which have 146097 days. Counting years from March 1 puts the leap day at the
// end of a year, so that the month lengths before it follow a fixed pattern.
constexpr std::int64_t days_per_era = 146097;
// Days from 0000-03-01 to 1970-01-01.
constexpr std::int64_t epoch_offset = 719468;

// The number of days from March 1 to the first day of a month, counting months from March (0) to February (11).
std::int64_t days_before_month(std::int64_t month_from_march) noexcept
{
    return ((153 * month_from_march) + 2) / 5;
}

} // namespace

bool is_leap_year(std::int64_t year) noexcept
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

unsigned days_in_month(std::int64_t year, unsigned month) noexcept
{
    if (month == 2) {
        return is_leap_year(year) ? 29 : 28;
    }
    return month == 4 || month == 6 || month == 9 || month == 11 ? 30 : 31;
}

civil_date civil_from_days(std::int64_t days) noexcept
{
    const std::int64_t shifted = days + epoch_offset;
    const std::int64_t era = (shifted >= 0 ? shifted : shifted - (days_per_era - 1)) / days_per_era;
    const std::int64_t day_of_era = shifted - (era * days_per_era);
    // Removes the leap days before day_of_era: one every 4 years (1460 days), none every 100 years (36524 days), and
    // one every 400 years (the last day of the era).
    const std::int64_t year_of_era =
        (day_of_era - (day_of_era / 1460) + (day_of_era / 36524) - (day_of_era / (days_per_era - 1))) / 365;
    const std::int64_t day_of_year = day_of_era - ((year_of_era * 365) + (year_of_era / 4) - (year_of_era / 100));
    const std::int64_t month_from_march = ((5 * day_of_year) + 2) / 153;
    const std::int64_t month = month_from_march < 10 ? month_from_march + 3 : month_from_march - 9;
    return {.year = year_of_era + (era * 400) + (month <= 2 ? 1 : 0),
            .month = static_cast<unsigned>(month),
            .day = static_cast<unsigned>(day_of_year - days_before_month(month_from_march) + 1)};
}

} // namespace openfits::detail
