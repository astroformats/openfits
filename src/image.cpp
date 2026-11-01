// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>
#include <openfits/image.h>

#include "data/conversion.h"

#include <array>
#include <cmath>
#include <utility>

namespace openfits {

namespace {

bool integral(double value) noexcept
{
    return std::isfinite(value) && value == std::trunc(value);
}

// The integer types from the narrowest, with their ranges as doubles; the upper bounds of the 64-bit types are 2^63 and
// 2^64, excluded, which a double holds exactly.
struct integer_type
{
    numeric_type type = numeric_type::int8;
    double low = 0.0;
    double high = 0.0;
    bool high_excluded = false;
};

constexpr std::array<integer_type, 8> integer_types{{
    {.type = numeric_type::int8, .low = -128.0, .high = 127.0},
    {.type = numeric_type::uint8, .low = 0.0, .high = 255.0},
    {.type = numeric_type::int16, .low = -32768.0, .high = 32767.0},
    {.type = numeric_type::uint16, .low = 0.0, .high = 65535.0},
    {.type = numeric_type::int32, .low = -2147483648.0, .high = 2147483647.0},
    {.type = numeric_type::uint32, .low = 0.0, .high = 4294967295.0},
    {.type = numeric_type::int64, .low = -9223372036854775808.0, .high = 9223372036854775808.0, .high_excluded = true},
    {.type = numeric_type::uint64, .low = 0.0, .high = 18446744073709551616.0, .high_excluded = true},
}};

} // namespace

numeric_type image_info::value_type() const
{
    if (!bitpix) {
        throw usage_error(errc::invalid_argument, "the image has no BITPIX, which the type of its values follows from");
    }
    const openfits::bitpix stored = *bitpix;
    if (stored == openfits::bitpix::float32 || stored == openfits::bitpix::float64) {
        return detail::stored_type(stored);
    }
    if (scaling.scale == 1.0 && scaling.zero == 0.0) {
        return detail::stored_type(stored);
    }
    const bool integers = integral(scaling.scale) && integral(scaling.zero);
    if (!integers) {
        return stored == openfits::bitpix::uint8 || stored == openfits::bitpix::int16 ? numeric_type::float32
                                                                                      : numeric_type::float64;
    }
    // 64-bit data hold every integer of their type, so that only the unsigned convention keeps an integer type.
    if (stored == openfits::bitpix::int64) {
        return scaling.scale == 1.0 && scaling.zero == 9223372036854775808.0 ? numeric_type::uint64
                                                                             : numeric_type::float64;
    }
    // The physical values of the extremes of the stored type, exact in double for every scaling that leaves an integer
    // type in reach.
    double low = 0.0;
    double high = 255.0;
    if (stored == openfits::bitpix::int16) {
        low = -32768.0;
        high = 32767.0;
    } else if (stored == openfits::bitpix::int32) {
        low = -2147483648.0;
        high = 2147483647.0;
    }
    double first = scaling.zero + (scaling.scale * low);
    double last = scaling.zero + (scaling.scale * high);
    if (first > last) {
        std::swap(first, last);
    }
    for (const integer_type& candidate : integer_types) {
        const bool below = candidate.high_excluded ? last < candidate.high : last <= candidate.high;
        if (first >= candidate.low && below) {
            return candidate.type;
        }
    }
    return numeric_type::float64;
}

} // namespace openfits
