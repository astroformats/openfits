// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <cstdint>

// Dates of the Gregorian calendar, proleptic before 1582, with the year 0 of ISO 8601 (standard §9.1.1). The arithmetic
// is written out, so that no non-reentrant C function, no time zone database and no locale is involved.

namespace openfits::detail {

/// A date of the proleptic Gregorian calendar. Year 0 is 1 BCE.
struct civil_date
{
    std::int64_t year = 1970;
    unsigned month = 1; ///< 1 to 12
    unsigned day = 1;   ///< 1 to the length of the month

    friend bool operator==(const civil_date&, const civil_date&) = default;
};

[[nodiscard]] bool is_leap_year(std::int64_t year) noexcept;

/// The number of days of a month, 1 to 12, in a year.
[[nodiscard]] unsigned days_in_month(std::int64_t year, unsigned month) noexcept;

/// The date that is the given number of days after 1970-01-01, before it when negative. Exact for years of up to nine
/// digits.
[[nodiscard]] civil_date civil_from_days(std::int64_t days) noexcept;

} // namespace openfits::detail
