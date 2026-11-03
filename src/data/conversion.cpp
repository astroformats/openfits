// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "data/conversion.h"

#include <openfits/error.h>

#include "data/conversion_kernels.h"

#include <bit>
#include <cmath>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace openfits::detail {

namespace {

// A little-endian host swaps the bytes of every stored value, a big-endian one none.
constexpr bool host_swaps = std::endian::native == std::endian::little;

constexpr double two_to_63 = 9223372036854775808.0;
constexpr double two_to_64 = 18446744073709551616.0;

bool is_integer_data(bitpix value) noexcept
{
    return static_cast<int>(value) > 0;
}

bool is_integer_type(numeric_type type) noexcept
{
    return type != numeric_type::float32 && type != numeric_type::float64;
}

bool integral(double value) noexcept
{
    return std::isfinite(value) && value == std::trunc(value);
}

// The range of the values of a type, as the exact integers of 64 bits that bound them.
struct integer_range
{
    std::int64_t min = 0;
    std::uint64_t max = 0;
};

template <typename T> integer_range range_of() noexcept
{
    return {.min = static_cast<std::int64_t>(std::numeric_limits<T>::min()),
            .max = static_cast<std::uint64_t>(std::numeric_limits<T>::max())};
}

integer_range range_of(numeric_type type) noexcept
{
    switch (type) {
    case numeric_type::int8:
        return range_of<std::int8_t>();
    case numeric_type::uint8:
        return range_of<std::uint8_t>();
    case numeric_type::int16:
        return range_of<std::int16_t>();
    case numeric_type::uint16:
        return range_of<std::uint16_t>();
    case numeric_type::int32:
        return range_of<std::int32_t>();
    case numeric_type::uint32:
        return range_of<std::uint32_t>();
    case numeric_type::int64:
        return range_of<std::int64_t>();
    case numeric_type::uint64:
    case numeric_type::float32:
    case numeric_type::float64:
        break;
    }
    return range_of<std::uint64_t>();
}

// True when the range inner lies within outer.
bool within(const integer_range& inner, const integer_range& outer) noexcept
{
    return inner.min >= outer.min && inner.max <= outer.max;
}

// True when every integer of [low, high] lies in the range of the integer type.
bool contains(numeric_type type, std::int64_t low, std::int64_t high) noexcept
{
    const integer_range range = range_of(type);
    return low >= range.min && std::cmp_less_equal(high, range.max);
}

bool contains(numeric_type type, std::uint64_t low, std::uint64_t high) noexcept
{
    const integer_range range = range_of(type);
    return std::cmp_greater_equal(low, range.min) && high <= range.max;
}

// True when a real value converts into the type, as the kernels convert it: within its range once truncated or
// rounded for an integer type, and finite within the range of float for float.
bool real_converts(numeric_type type, double value, rounding mode) noexcept
{
    switch (type) {
    case numeric_type::int8:
        return kernels::real_fits<std::int8_t>(value, mode);
    case numeric_type::uint8:
        return kernels::real_fits<std::uint8_t>(value, mode);
    case numeric_type::int16:
        return kernels::real_fits<std::int16_t>(value, mode);
    case numeric_type::uint16:
        return kernels::real_fits<std::uint16_t>(value, mode);
    case numeric_type::int32:
        return kernels::real_fits<std::int32_t>(value, mode);
    case numeric_type::uint32:
        return kernels::real_fits<std::uint32_t>(value, mode);
    case numeric_type::int64:
        return kernels::real_fits<std::int64_t>(value, mode);
    case numeric_type::uint64:
        return kernels::real_fits<std::uint64_t>(value, mode);
    case numeric_type::float32:
        return std::isfinite(value) && std::abs(value) <= static_cast<double>(std::numeric_limits<float>::max());
    case numeric_type::float64:
        break;
    }
    return true;
}

// zero + scale x value, as the kernels compute it.
double scaled(double value, double scale, double zero) noexcept
{
    const double product = value * scale;
    return product + zero;
}

// The arithmetic of BSCALE = 1 and an integer BZERO over the integers of [low, high]: exact in 64-bit signed integers
// when every sum fits there, in unsigned ones when every sum lies in [0, 2^64), the 64-bit unsigned convention, and
// scaled otherwise. The offset that it uses is set in offset or unsigned_offset.
arithmetic offset_arithmetic(std::int64_t low, std::int64_t high, double zero, std::int64_t& offset,
                             std::uint64_t& unsigned_offset) noexcept
{
    if (zero >= -two_to_63 && zero < two_to_63) {
        const auto z = static_cast<std::int64_t>(zero);
        const bool fits = z >= 0 ? high <= std::numeric_limits<std::int64_t>::max() - z
                                 : low >= std::numeric_limits<std::int64_t>::min() - z;
        if (fits) {
            offset = z;
            return arithmetic::signed_offset;
        }
        return arithmetic::scaled;
    }
    if (zero >= two_to_63 && zero < two_to_64) {
        const auto z = static_cast<std::uint64_t>(zero);
        // Every sum is at least 2^63 - 2^63; the largest must stay below 2^64.
        if (high < 0 || static_cast<std::uint64_t>(high) <= std::numeric_limits<std::uint64_t>::max() - z) {
            unsigned_offset = z;
            return arithmetic::unsigned_offset;
        }
    }
    return arithmetic::scaled;
}

// The range of the values that a BITPIX of integers stores.
std::pair<std::int64_t, std::int64_t> stored_range(bitpix value) noexcept
{
    switch (value) {
    case bitpix::uint8:
        return {0, 255};
    case bitpix::int16:
        return {std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max()};
    case bitpix::int32:
        return {std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()};
    case bitpix::int64:
    case bitpix::float32:
    case bitpix::float64:
        break;
    }
    return {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()};
}

// The BZERO of the offset of standard §5.2.5 (Table 11) for data of a BITPIX of integers: the most significant bit of
// their values, which it flips.
double sign_offset(bitpix value) noexcept
{
    switch (value) {
    case bitpix::uint8:
        return -128.0;
    case bitpix::int16:
        return 32768.0;
    case bitpix::int32:
        return 2147483648.0;
    case bitpix::int64:
        return two_to_63;
    case bitpix::float32:
    case bitpix::float64:
        break;
    }
    return 0.0;
}

// The type of the values of an offset of Table 11, which the flip of the bit gives.
numeric_type flipped_type(bitpix value) noexcept
{
    switch (value) {
    case bitpix::uint8:
        return numeric_type::int8;
    case bitpix::int16:
        return numeric_type::uint16;
    case bitpix::int32:
        return numeric_type::uint32;
    case bitpix::int64:
    case bitpix::float32:
    case bitpix::float64:
        break;
    }
    return numeric_type::uint64;
}

// Checks that the value of blank_policy::value converts into the destination type.
void check_blank_value(numeric_type to, double value)
{
    bool converts = true;
    if (is_integer_type(to)) {
        converts = integral(value) && real_converts(to, value, rounding::truncate);
    } else if (to == numeric_type::float32) {
        converts = !std::isfinite(value) || real_converts(to, value, rounding::truncate);
    }
    if (!converts) {
        throw usage_error(errc::invalid_argument, "the value for undefined values, " + std::to_string(value) +
                                                      ", is not one of the type of the destination");
    }
}

} // namespace

numeric_type stored_type(bitpix value) noexcept
{
    switch (value) {
    case bitpix::uint8:
        return numeric_type::uint8;
    case bitpix::int16:
        return numeric_type::int16;
    case bitpix::int32:
        return numeric_type::int32;
    case bitpix::int64:
        return numeric_type::int64;
    case bitpix::float32:
        return numeric_type::float32;
    case bitpix::float64:
        break;
    }
    return numeric_type::float64;
}

std::size_t stored_size(bitpix value) noexcept
{
    return static_cast<std::size_t>(std::abs(static_cast<int>(value))) / 8;
}

std::optional<bitpix> bitpix_of(std::int64_t value) noexcept
{
    switch (value) {
    case 8:
        return bitpix::uint8;
    case 16:
        return bitpix::int16;
    case 32:
        return bitpix::int32;
    case 64:
        return bitpix::int64;
    case -32:
        return bitpix::float32;
    case -64:
        return bitpix::float64;
    default:
        return std::nullopt;
    }
}

read_conversion make_read_conversion(bitpix from, numeric_type to, const scaling& values,
                                     std::optional<std::int64_t> blank, blank_policy policy, double blank_value,
                                     overflow_policy overflow, openfits::rounding rounding, std::uint64_t hdu)
{
    const bool integer_data = is_integer_data(from);
    const bool integer_target = is_integer_type(to);
    if (policy == blank_policy::value) {
        check_blank_value(to, blank_value);
    }
    read_conversion conversion{.from = from,
                               .to = to,
                               .scale = values.scale,
                               .zero = values.zero,
                               .blank = integer_data && policy != blank_policy::ignore ? blank : std::nullopt,
                               .policy = policy,
                               .blank_value = blank_value,
                               .overflow = overflow,
                               .rounding = rounding,
                               .hdu = hdu};

    if (values.scale == 1.0 && values.zero == 0.0) {
        conversion.arithmetic = arithmetic::identity;
    } else if (integer_data && values.scale == 1.0 && values.zero == sign_offset(from)) {
        conversion.arithmetic = arithmetic::sign_flip;
    } else if (integer_data && values.scale == 1.0 && integral(values.zero)) {
        const auto [low, high] = stored_range(from);
        conversion.arithmetic =
            offset_arithmetic(low, high, values.zero, conversion.offset, conversion.unsigned_offset);
    } else {
        conversion.arithmetic = arithmetic::scaled;
    }

    if (integer_data) {
        const auto [low, high] = stored_range(from);
        switch (conversion.arithmetic) {
        case arithmetic::identity:
            conversion.checks_range = integer_target && !contains(to, low, high);
            break;
        case arithmetic::sign_flip:
            conversion.checks_range = integer_target && !within(range_of(flipped_type(from)), range_of(to));
            break;
        case arithmetic::signed_offset:
            conversion.checks_range =
                integer_target && !contains(to, low + conversion.offset, high + conversion.offset);
            break;
        case arithmetic::unsigned_offset:
            conversion.checks_range =
                integer_target && !contains(to, static_cast<std::uint64_t>(low) + conversion.unsigned_offset,
                                            static_cast<std::uint64_t>(high) + conversion.unsigned_offset);
            break;
        case arithmetic::scaled: {
            const double first = scaled(static_cast<double>(low), values.scale, values.zero);
            const double last = scaled(static_cast<double>(high), values.scale, values.zero);
            conversion.checks_range = to != numeric_type::float64 &&
                                      !(real_converts(to, first, rounding) && real_converts(to, last, rounding));
            break;
        }
        }
        conversion.checks_undefined = conversion.blank.has_value() && (policy != blank_policy::nan || !integer_target);
    } else {
        // Floating-point data hold infinities and values of any size, which integer types and float overflow, but a
        // float read as itself or widened.
        const bool keeps = to == numeric_type::float64 || (to == numeric_type::float32 && from == bitpix::float32 &&
                                                           conversion.arithmetic == arithmetic::identity);
        conversion.checks_range = !keeps;
        conversion.checks_undefined = integer_target || (policy != blank_policy::nan && policy != blank_policy::ignore);
    }
    return conversion;
}

std::uint64_t convert_read(const read_conversion& conversion, const std::byte* in, std::ptrdiff_t stride,
                           std::byte* out, std::uint8_t* mask, std::size_t count)
{
    return kernels::read<host_swaps>(conversion, in, stride, out, mask, count);
}

std::uint64_t convert_read_in_place(const read_conversion& conversion, std::byte* data, std::uint8_t* mask,
                                    std::size_t count)
{
    return kernels::read_in_place<host_swaps>(conversion, data, mask, count);
}

write_conversion make_write_conversion(numeric_type from, bitpix to, const scaling& values,
                                       std::optional<std::int64_t> blank, overflow_policy overflow, std::uint64_t hdu)
{
    const bool integer_source = is_integer_type(from);
    const bool integer_data = is_integer_data(to);
    write_conversion conversion{.from = from,
                                .to = to,
                                .scale = values.scale,
                                .zero = values.zero,
                                .blank = integer_data ? blank : std::nullopt,
                                .overflow = overflow,
                                .hdu = hdu};
    const integer_range source = range_of(from);
    const numeric_type stored = stored_type(to);

    if (values.scale == 1.0 && values.zero == 0.0) {
        conversion.arithmetic = arithmetic::identity;
    } else if (integer_data && values.scale == 1.0 && values.zero == sign_offset(to) && from == flipped_type(to)) {
        // The values of the type that an offset of Table 11 stores, whose bit the write flips back.
        conversion.arithmetic = arithmetic::sign_flip;
    } else if (integer_source && integer_data && values.scale == 1.0 && integral(values.zero)) {
        // The value minus BZERO, exact: in signed integers for sources of up to 32 bits, and in unsigned ones for the
        // 64-bit unsigned convention.
        if (size_of(from) <= 4 && std::abs(values.zero) <= 4611686018427387904.0) {
            conversion.arithmetic = arithmetic::signed_offset;
            conversion.offset = static_cast<std::int64_t>(values.zero);
        } else if (to == bitpix::int64 && values.zero == two_to_63) {
            conversion.arithmetic = arithmetic::unsigned_offset;
            conversion.unsigned_offset = static_cast<std::uint64_t>(values.zero);
        } else {
            conversion.arithmetic = arithmetic::scaled;
        }
    } else {
        conversion.arithmetic = arithmetic::scaled;
    }

    if (integer_source) {
        const bool source_signed = source.min < 0;
        switch (conversion.arithmetic) {
        case arithmetic::identity:
            conversion.checks_range = integer_data && !within(source, range_of(stored));
            break;
        case arithmetic::sign_flip:
            conversion.checks_range = false;
            break;
        case arithmetic::signed_offset:
            // A source of up to 32 bits: its bounds are exact in 64-bit signed integers.
            conversion.checks_range = !contains(stored, source.min - conversion.offset,
                                                static_cast<std::int64_t>(source.max) - conversion.offset);
            break;
        case arithmetic::unsigned_offset:
            // value - 2^63 lies in the range of int64 for every unsigned value, and below it for a negative one.
            conversion.checks_range = source_signed;
            break;
        case arithmetic::scaled: {
            const double low = (static_cast<double>(source.min) - values.zero) / values.scale;
            const double high = (static_cast<double>(source.max) - values.zero) / values.scale;
            conversion.checks_range = to != bitpix::float64 && !(real_converts(stored, low, rounding::nearest) &&
                                                                 real_converts(stored, high, rounding::nearest));
            break;
        }
        }
    } else {
        // Floating-point values of any size overflow integer data and float, but a float written as itself or widened.
        const bool keeps = to == bitpix::float64 || (to == bitpix::float32 && from == numeric_type::float32 &&
                                                     conversion.arithmetic == arithmetic::identity);
        conversion.checks_range = !keeps;
        conversion.checks_undefined = integer_data;
    }
    return conversion;
}

std::uint64_t convert_write(const write_conversion& conversion, const std::byte* in, std::byte* out, std::size_t count)
{
    return kernels::write<host_swaps>(conversion, in, out, count);
}

} // namespace openfits::detail
