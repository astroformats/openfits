// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/card.h>
#include <openfits/error.h>

#include "core/ascii.h"
#include "core/calendar.h"
#include "core/quote.h"

#include <cstddef>
#include <optional>
#include <string>

namespace openfits {

namespace {

constexpr int max_year = 99'999;
constexpr std::uint32_t nanoseconds_per_second = 1'000'000'000;

// Reads a datetime string from left to right. Each step returns nothing, or false, when the text does not follow the
// grammar.
class date_scanner
{
public:
    explicit date_scanner(std::string_view text) noexcept : text_(text) {}

    [[nodiscard]] bool at_end() const noexcept
    {
        return position_ == text_.size();
    }

    // True, and past c, when c is next.
    bool skip(char c) noexcept
    {
        if (!at_end() && text_[position_] == c) {
            ++position_;
            return true;
        }
        return false;
    }

    // The value of the next count digits.
    std::optional<unsigned> digits(std::size_t count) noexcept
    {
        unsigned value = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (at_end() || !detail::is_digit(text_[position_])) {
                return std::nullopt;
            }
            value = (value * 10) + static_cast<unsigned>(text_[position_] - '0');
            ++position_;
        }
        return value;
    }

    // The nanoseconds of a decimal fraction of at least one digit, truncated after the ninth.
    std::optional<std::uint32_t> fraction() noexcept
    {
        std::uint32_t nanoseconds = 0;
        std::size_t count = 0;
        while (!at_end() && detail::is_digit(text_[position_])) {
            if (count < 9) {
                nanoseconds = (nanoseconds * 10) + static_cast<std::uint32_t>(text_[position_] - '0');
            }
            ++count;
            ++position_;
        }
        for (std::size_t i = count; i < 9; ++i) {
            nanoseconds *= 10;
        }
        return count == 0 ? std::nullopt : std::optional(nanoseconds);
    }

private:
    std::string_view text_;
    std::size_t position_ = 0;
};

// The form of files written before 2000, DD/MM/YY, for the years 1900 to 1999 (standard §4.4.2.1).
std::optional<date_time> scan_old_date(std::string_view text) noexcept
{
    date_scanner scan(text);
    const std::optional<unsigned> day = scan.digits(2);
    if (!day || !scan.skip('/')) {
        return std::nullopt;
    }
    const std::optional<unsigned> month = scan.digits(2);
    if (!month || !scan.skip('/')) {
        return std::nullopt;
    }
    const std::optional<unsigned> year = scan.digits(2);
    if (!year || !scan.at_end()) {
        return std::nullopt;
    }
    return date_time{.year = 1900 + static_cast<int>(*year), .month = *month, .day = *day};
}

// The datetime form, [+-]YYYYY-MM-DD or YYYY-MM-DD, then optionally Thh:mm:ss and a fraction (standard §9.1.1).
std::optional<date_time> scan_datetime(std::string_view text) noexcept
{
    date_scanner scan(text);
    date_time result;
    const bool negative = scan.skip('-');
    const bool signed_year = negative || scan.skip('+');
    const std::optional<unsigned> year = scan.digits(signed_year ? 5 : 4);
    if (!year || !scan.skip('-')) {
        return std::nullopt;
    }
    const std::optional<unsigned> month = scan.digits(2);
    if (!month || !scan.skip('-')) {
        return std::nullopt;
    }
    const std::optional<unsigned> day = scan.digits(2);
    if (!day) {
        return std::nullopt;
    }
    result.year = negative ? -static_cast<int>(*year) : static_cast<int>(*year);
    result.month = *month;
    result.day = *day;
    if (scan.at_end()) {
        return result;
    }

    if (!scan.skip('T')) {
        return std::nullopt;
    }
    const std::optional<unsigned> hour = scan.digits(2);
    if (!hour || !scan.skip(':')) {
        return std::nullopt;
    }
    const std::optional<unsigned> minute = scan.digits(2);
    if (!minute || !scan.skip(':')) {
        return std::nullopt;
    }
    const std::optional<unsigned> second = scan.digits(2);
    if (!second) {
        return std::nullopt;
    }
    result.has_time = true;
    result.hour = *hour;
    result.minute = *minute;
    result.second = *second;
    if (scan.skip('.')) {
        const std::optional<std::uint32_t> nanoseconds = scan.fraction();
        if (!nanoseconds) {
            return std::nullopt;
        }
        result.nanosecond = *nanoseconds;
    }
    if (!scan.at_end()) {
        return std::nullopt;
    }
    return result;
}

// True when the members of value are in their ranges, and name a date and a time that exist.
bool is_valid(const date_time& value) noexcept
{
    const bool date = value.year >= -max_year && value.year <= max_year && value.month >= 1 && value.month <= 12 &&
                      value.day >= 1 && value.day <= detail::days_in_month(value.year, value.month);
    if (!value.has_time) {
        return date && value.hour == 0 && value.minute == 0 && value.second == 0 && value.nanosecond == 0;
    }
    return date && value.hour <= 23 && value.minute <= 59 && value.second <= 60 &&
           value.nanosecond < nanoseconds_per_second;
}

// Appends value in decimal, with leading zeros up to width digits.
void append_padded(std::string& text, unsigned value, std::size_t width)
{
    const std::string digits = std::to_string(value);
    if (digits.size() < width) {
        text.append(width - digits.size(), '0');
    }
    text += digits;
}

} // namespace

date_time parse_date(std::string_view text)
{
    std::optional<date_time> value = scan_old_date(text);
    if (!value) {
        value = scan_datetime(text);
    }
    if (!value || !is_valid(*value)) {
        throw invalid_data_error(errc::invalid_date, detail::quote(text) + " is not a date of the standard");
    }
    return *value;
}

std::string format_date(const date_time& value)
{
    if (!is_valid(value)) {
        throw usage_error(errc::invalid_date, "the date and time to format are not valid");
    }
    std::string text;
    // The four-digit year from 0 to 9999, and the signed five-digit year otherwise (standard §9.1.1).
    if (value.year >= 0 && value.year <= 9999) {
        append_padded(text, static_cast<unsigned>(value.year), 4);
    } else {
        text += value.year < 0 ? '-' : '+';
        append_padded(text, static_cast<unsigned>(value.year < 0 ? -value.year : value.year), 5);
    }
    text += '-';
    append_padded(text, value.month, 2);
    text += '-';
    append_padded(text, value.day, 2);
    if (!value.has_time) {
        return text;
    }
    text += 'T';
    append_padded(text, value.hour, 2);
    text += ':';
    append_padded(text, value.minute, 2);
    text += ':';
    append_padded(text, value.second, 2);
    if (value.nanosecond != 0) {
        std::string fraction;
        append_padded(fraction, value.nanosecond, 9);
        fraction.erase(fraction.find_last_not_of('0') + 1);
        text += '.';
        text += fraction;
    }
    return text;
}

date_time to_date_time(std::chrono::sys_time<std::chrono::nanoseconds> time) noexcept
{
    constexpr std::int64_t seconds_per_day = 86'400;

    // Floor divisions, so that times before 1970 fall into the right second and day.
    std::int64_t seconds = time.time_since_epoch().count() / nanoseconds_per_second;
    std::int64_t nanoseconds = time.time_since_epoch().count() % nanoseconds_per_second;
    if (nanoseconds < 0) {
        nanoseconds += nanoseconds_per_second;
        --seconds;
    }
    std::int64_t days = seconds / seconds_per_day;
    std::int64_t second_of_day = seconds % seconds_per_day;
    if (second_of_day < 0) {
        second_of_day += seconds_per_day;
        --days;
    }
    const detail::civil_date date = detail::civil_from_days(days);
    return {.year = static_cast<int>(date.year),
            .month = date.month,
            .day = date.day,
            .has_time = true,
            .hour = static_cast<unsigned>(second_of_day / 3600),
            .minute = static_cast<unsigned>((second_of_day / 60) % 60),
            .second = static_cast<unsigned>(second_of_day % 60),
            .nanosecond = static_cast<std::uint32_t>(nanoseconds)};
}

std::string format_date(std::chrono::sys_time<std::chrono::nanoseconds> time)
{
    return format_date(to_date_time(time));
}

} // namespace openfits
