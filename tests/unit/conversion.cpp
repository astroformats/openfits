// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "data/conversion.h"

#include <openfits/error.h>
#include <openfits/types.h>

#include "data/conversion_kernels.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// The conversion matrix of the reads and writes of array values (standard §4.4.2.5, §5): every BITPIX into every
// destination type, under each kind of scaling, on hosts of both byte orders, against values that a reference computes
// in the plainest way: the physical value exactly where the scaling allows it and in double otherwise, then
// std::trunc() or a rounding half away from zero, and the bounds of the destination type.

namespace {

using openfits::bitpix;
using openfits::blank_policy;
using openfits::errc;
using openfits::invalid_data_error;
using openfits::numeric_type;
using openfits::overflow_policy;
using openfits::rounding;
using openfits::scaling;
using openfits::usage_error;
using openfits::detail::make_read_conversion;
using openfits::detail::make_write_conversion;
using openfits::detail::read_conversion;
using openfits::detail::write_conversion;
using openfits::test::throws;

namespace kernels = openfits::detail::kernels;

constexpr double two_to_63 = 9223372036854775808.0;

// The bytes that the kernel of a host that swaps, or not, reads as value: the bytes of value in the order of this
// host, reversed when it swaps. On a little-endian host the first are big-endian, the bytes of a file.
template <typename T> std::vector<std::byte> encode(T value, bool swap)
{
    std::vector<std::byte> bytes(sizeof(T));
    std::memcpy(bytes.data(), &value, sizeof(T));
    if (swap) {
        std::ranges::reverse(bytes);
    }
    return bytes;
}

template <typename T> T decode(std::span<const std::byte> bytes, bool swap)
{
    std::vector<std::byte> copy(bytes.begin(), bytes.end());
    if (swap) {
        std::ranges::reverse(copy);
    }
    T value{};
    std::memcpy(&value, copy.data(), sizeof(T));
    return value;
}

std::uint64_t read(bool swap, const read_conversion& conversion, const std::byte* in, std::ptrdiff_t stride,
                   std::byte* out, std::uint8_t* mask, std::size_t count)
{
    return swap ? kernels::read<true>(conversion, in, stride, out, mask, count)
                : kernels::read<false>(conversion, in, stride, out, mask, count);
}

std::uint64_t write(bool swap, const write_conversion& conversion, const std::byte* in, std::byte* out,
                    std::size_t count)
{
    return swap ? kernels::write<true>(conversion, in, out, count) : kernels::write<false>(conversion, in, out, count);
}

// The kinds of scaling of the matrix. The scales are powers of 2, so that the products are exact and a host that
// fuses a multiplication and an addition gives the same values as one that does not.
enum class scaling_kind
{
    none,
    unsigned_convention, // the BZERO of standard §5.2.5, Table 11, for integer data
    halves,              // BSCALE 0.5 and BZERO -1.5: every value a multiple of 0.5, many of them halves
    negative,            // BSCALE -2 and BZERO 10
};

scaling scaling_of(scaling_kind kind, bitpix stored)
{
    switch (kind) {
    case scaling_kind::none:
        break;
    case scaling_kind::unsigned_convention:
        switch (stored) {
        case bitpix::uint8:
            return {.zero = -128.0};
        case bitpix::int16:
            return {.zero = 32768.0};
        case bitpix::int32:
            return {.zero = 2147483648.0};
        case bitpix::int64:
            return {.zero = two_to_63};
        case bitpix::float32:
        case bitpix::float64:
            break;
        }
        break;
    case scaling_kind::halves:
        return {.scale = 0.5, .zero = -1.5};
    case scaling_kind::negative:
        return {.scale = -2.0, .zero = 10.0};
    }
    return {};
}

// What the reference expects of the conversion of one value: the value, or an overflow, with the value of the clamp.
template <typename T> struct expectation
{
    std::optional<T> value{};
    T clamped{};
};

template <typename T> T clamp_of(bool below)
{
    if constexpr (std::floating_point<T>) {
        return below ? std::numeric_limits<T>::lowest() : std::numeric_limits<T>::max();
    } else {
        return below ? std::numeric_limits<T>::min() : std::numeric_limits<T>::max();
    }
}

// A value rounded half away from zero, from its truncation and the fraction, which the subtraction gives exactly: the
// std::round() of some C runtimes adds 0.5 first, which takes the largest double below 0.5 to 1.
double half_away(double value)
{
    const double whole = std::trunc(value);
    const double fraction = value - whole;
    if (fraction >= 0.5) {
        return whole + 1.0;
    }
    if (fraction <= -0.5) {
        return whole - 1.0;
    }
    return whole;
}

// A real value into T: truncated, or rounded half away from zero, then within the bounds of T, the upper one being
// 2^digits, exact for every type.
template <typename T> expectation<T> expected_real(double value, rounding mode)
{
    if constexpr (std::floating_point<T>) {
        if constexpr (std::same_as<T, float>) {
            if (std::isfinite(value) && std::abs(value) > static_cast<double>(std::numeric_limits<float>::max())) {
                return {.clamped = clamp_of<T>(value < 0)};
            }
            return {.value = static_cast<float>(value)};
        } else {
            return {.value = value};
        }
    } else {
        const double whole = mode == rounding::truncate ? std::trunc(value) : half_away(value);
        const auto lower = static_cast<double>(std::numeric_limits<T>::min());
        const double upper = std::ldexp(1.0, std::numeric_limits<T>::digits);
        if (std::isnan(whole) || whole < lower || whole >= upper) {
            return {.clamped = clamp_of<T>(value < 0)};
        }
        return {.value = static_cast<T>(whole)};
    }
}

// An exact integer into T.
template <typename T, typename Integer> expectation<T> expected_integer(Integer value)
{
    if constexpr (std::floating_point<T>) {
        return {.value = static_cast<T>(value)};
    } else {
        if (!std::in_range<T>(value)) {
            return {.clamped = clamp_of<T>(std::cmp_less(value, 0))};
        }
        return {.value = static_cast<T>(value)};
    }
}

// The physical value of a stored one under a scaling, and its conversion into T.
template <typename Stored, typename T> expectation<T> expected_read(Stored stored, const scaling& values, rounding mode)
{
    if constexpr (std::floating_point<Stored>) {
        if (values.scale == 1.0 && values.zero == 0.0) {
            return expected_real<T>(static_cast<double>(stored), mode);
        }
        return expected_real<T>(static_cast<double>(stored) * values.scale + values.zero, mode);
    } else {
        if (values.scale == 1.0 && values.zero == 0.0) {
            return expected_integer<T>(static_cast<std::int64_t>(stored));
        }
        if (values.scale == 1.0 && values.zero == two_to_63) {
            // The 64-bit unsigned convention: stored + 2^63, in [0, 2^64).
            return expected_integer<T>(static_cast<std::uint64_t>(static_cast<std::int64_t>(stored)) +
                                       (std::uint64_t{1} << 63U));
        }
        if (values.scale == 1.0 && std::trunc(values.zero) == values.zero && sizeof(Stored) <= 4) {
            return expected_integer<T>(static_cast<std::int64_t>(stored) + static_cast<std::int64_t>(values.zero));
        }
        return expected_real<T>(static_cast<double>(stored) * values.scale + values.zero, mode);
    }
}

// The values of the matrix for each stored type: the extremes, their neighbours, and small values about zero.
template <typename Stored> std::vector<Stored> stored_values()
{
    if constexpr (std::unsigned_integral<Stored>) {
        constexpr Stored max = std::numeric_limits<Stored>::max();
        std::vector<Stored> values{0,
                                   1,
                                   2,
                                   3,
                                   kernels::cast<Stored>(max / 2),
                                   kernels::cast<Stored>((max / 2) + 1),
                                   kernels::cast<Stored>(max - 1),
                                   max};
        if constexpr (std::same_as<Stored, std::uint64_t>) {
            values.push_back((std::uint64_t{1} << 53) + 1);
        }
        return values;
    } else if constexpr (std::floating_point<Stored>) {
        constexpr Stored infinity = std::numeric_limits<Stored>::infinity();
        // Floats, so that both types hold the same values, each converted explicitly, as clang-cl wants.
        constexpr std::array<float, 16> both{-0.0F,   0.0F,   1.5F,     -1.5F,     2.5F,  -2.5F,  0.49F, 127.5F,
                                             -128.5F, 255.5F, 32767.5F, -32768.5F, 1e10F, -1e10F, 3e38F, -3e38F};
        // Appended one by one: GCC 14 reports an overflow in an insert of a list, which it cannot see is no overflow.
        std::vector<Stored> values;
        values.reserve(both.size() + 18);
        for (const float value : both) {
            values.push_back(kernels::cast<Stored>(value));
        }
        values.push_back(infinity);
        values.push_back(-infinity);
        values.push_back(std::numeric_limits<Stored>::denorm_min());
        if constexpr (std::same_as<Stored, double>) {
            // Beyond the integer types, and the doubles where a rounding by an addition can go wrong: the largest below
            // 0.5, a half and an odd integer where the spacing of doubles is 0.5 and 1, the largest below 2, which a
            // half added takes into the next binade, and the largest below the bound of the rounding into int32.
            for (const double value : {1e300, -1e300, two_to_63, -two_to_63, 18446744073709551616.0, 4294967295.5,
                                       -2147483648.5, 0.5 - 0x1p-54, -(0.5 - 0x1p-54), 0x1p51 + 0.5, -(0x1p51 + 0.5),
                                       0x1p52 + 1.0, -(0x1p52 + 1.0), 2.0 - 0x1p-52, 2147483647.5 - 0x1p-22}) {
                values.push_back(value);
            }
        }
        return values;
    } else {
        constexpr Stored min = std::numeric_limits<Stored>::min();
        constexpr Stored max = std::numeric_limits<Stored>::max();
        std::vector<Stored> values{min, kernels::cast<Stored>(min + 1), -3, -1, 0, 1,
                                   3,   kernels::cast<Stored>(max - 1), max};
        if constexpr (std::same_as<Stored, std::int64_t>) {
            // Beyond 2^53, which a double does not hold exactly.
            values.push_back((std::int64_t{1} << 53) + 1);
        }
        return values;
    }
}

template <typename T> bool same_value(T left, T right)
{
    if constexpr (std::floating_point<T>) {
        // The bits, so that a signed zero or a NaN is compared too.
        return std::bit_cast<kernels::bits_of<T>>(left) == std::bit_cast<kernels::bits_of<T>>(right) ||
               (std::isnan(left) && std::isnan(right));
    } else {
        return left == right;
    }
}

struct matrix_case
{
    bitpix stored = bitpix::uint8;
    numeric_type target = numeric_type::uint8;
};

std::string name_of(numeric_type type)
{
    constexpr std::array<std::string_view, 10> names{"int8",   "uint8", "int16",  "uint16",  "int32",
                                                     "uint32", "int64", "uint64", "float32", "float64"};
    return std::string(names.at(static_cast<std::size_t>(type)));
}

// BITPIX -32 is minus32, so that the name of a test holds letters, digits and underscores alone.
std::string name_of(const matrix_case& entry)
{
    const int value = static_cast<int>(entry.stored);
    return (value < 0 ? "minus" + std::to_string(-value) : std::to_string(value)) + "_and_" + name_of(entry.target);
}

// CTest names each test after the printed parameter.
// NOLINTNEXTLINE(readability-identifier-naming)
void PrintTo(const matrix_case& entry, std::ostream* output)
{
    *output << name_of(entry);
}

class conversion_matrix : public testing::TestWithParam<matrix_case>
{};

template <typename Stored, typename T> void check_reads(bitpix stored_bitpix, numeric_type target)
{
    for (const scaling_kind kind :
         {scaling_kind::none, scaling_kind::unsigned_convention, scaling_kind::halves, scaling_kind::negative}) {
        if (kind == scaling_kind::unsigned_convention && std::floating_point<Stored>) {
            continue;
        }
        const scaling values = scaling_of(kind, stored_bitpix);
        for (const bool swap : {true, false}) {
            for (const rounding mode : {rounding::truncate, rounding::nearest}) {
                for (const overflow_policy overflow : {overflow_policy::refuse, overflow_policy::clamp}) {
                    const read_conversion conversion = make_read_conversion(stored_bitpix, target, values, std::nullopt,
                                                                            blank_policy::nan, 0.0, overflow, mode, 0);
                    for (const Stored stored : stored_values<Stored>()) {
                        SCOPED_TRACE(testing::Message()
                                     << "stored " << kernels::text_of(stored) << ", scale " << values.scale << ", zero "
                                     << values.zero << ", swap " << swap << ", rounding " << static_cast<int>(mode)
                                     << ", overflow " << static_cast<int>(overflow));
                        const expectation<T> expected = expected_read<Stored, T>(stored, values, mode);
                        const std::vector<std::byte> in = encode(stored, swap);
                        std::array<std::byte, sizeof(T)> out{};
                        if (!expected.value && overflow == overflow_policy::refuse) {
                            EXPECT_TRUE(throws<invalid_data_error>(errc::value_out_of_range, [&] {
                                (void)read(swap, conversion, in.data(), 1, out.data(), nullptr, 1);
                            }));
                            continue;
                        }
                        const std::uint64_t clamped = read(swap, conversion, in.data(), 1, out.data(), nullptr, 1);
                        const T value = decode<T>(out, false);
                        EXPECT_TRUE(same_value(value, expected.value.value_or(expected.clamped)))
                            << kernels::text_of(value) << " instead of "
                            << kernels::text_of(expected.value.value_or(expected.clamped));
                        EXPECT_EQ(clamped, expected.value ? 0U : 1U);
                    }
                }
            }
        }
    }
}

template <typename T> void with_stored(bitpix stored, numeric_type target)
{
    switch (stored) {
    case bitpix::uint8:
        check_reads<std::uint8_t, T>(stored, target);
        return;
    case bitpix::int16:
        check_reads<std::int16_t, T>(stored, target);
        return;
    case bitpix::int32:
        check_reads<std::int32_t, T>(stored, target);
        return;
    case bitpix::int64:
        check_reads<std::int64_t, T>(stored, target);
        return;
    case bitpix::float32:
        check_reads<float, T>(stored, target);
        return;
    case bitpix::float64:
        check_reads<double, T>(stored, target);
        return;
    }
}

// Every stored value of the matrix read into the destination type, alone.
TEST_P(conversion_matrix, reads_each_value_as_the_reference_converts_it)
{
    const auto [stored, target] = GetParam();
    switch (target) {
    case numeric_type::int8:
        with_stored<std::int8_t>(stored, target);
        return;
    case numeric_type::uint8:
        with_stored<std::uint8_t>(stored, target);
        return;
    case numeric_type::int16:
        with_stored<std::int16_t>(stored, target);
        return;
    case numeric_type::uint16:
        with_stored<std::uint16_t>(stored, target);
        return;
    case numeric_type::int32:
        with_stored<std::int32_t>(stored, target);
        return;
    case numeric_type::uint32:
        with_stored<std::uint32_t>(stored, target);
        return;
    case numeric_type::int64:
        with_stored<std::int64_t>(stored, target);
        return;
    case numeric_type::uint64:
        with_stored<std::uint64_t>(stored, target);
        return;
    case numeric_type::float32:
        with_stored<float>(stored, target);
        return;
    case numeric_type::float64:
        with_stored<double>(stored, target);
        return;
    }
}

std::vector<matrix_case> matrix()
{
    std::vector<matrix_case> cases;
    for (const bitpix stored :
         {bitpix::uint8, bitpix::int16, bitpix::int32, bitpix::int64, bitpix::float32, bitpix::float64}) {
        for (int target = 0; target <= static_cast<int>(numeric_type::float64); ++target) {
            cases.push_back({.stored = stored, .target = static_cast<numeric_type>(target)});
        }
    }
    return cases;
}

INSTANTIATE_TEST_SUITE_P(every_type, conversion_matrix, testing::ValuesIn(matrix()),
                         [](const testing::TestParamInfo<matrix_case>& instance) { return name_of(instance.param); });

TEST(conversion, a_run_of_values_converts_as_each_value_alone_forwards_backwards_and_in_place)
{
    // BITPIX 16 with the unsigned convention into std::uint16_t, which converts in place, and into float.
    const std::vector<std::int16_t> stored{-32768, -32767, -1, 0, 1, 32767};
    std::vector<std::byte> in;
    for (const std::int16_t value : stored) {
        const std::vector<std::byte> bytes = encode(value, true);
        in.insert(in.end(), bytes.begin(), bytes.end());
    }
    const read_conversion to_unsigned =
        make_read_conversion(bitpix::int16, numeric_type::uint16, {.zero = 32768.0}, std::nullopt, blank_policy::nan,
                             0.0, overflow_policy::refuse, rounding::truncate, 0);
    const read_conversion to_float =
        make_read_conversion(bitpix::int16, numeric_type::float32, {.zero = 32768.0}, std::nullopt, blank_policy::nan,
                             0.0, overflow_policy::refuse, rounding::truncate, 0);
    const std::vector<std::uint16_t> forwards{0, 1, 32767, 32768, 32769, 65535};

    std::vector<std::uint16_t> out(stored.size());
    (void)read(true, to_unsigned, in.data(), 1, reinterpret_cast<std::byte*>(out.data()), nullptr, out.size());
    EXPECT_EQ(out, forwards);

    // Backwards from the last value, and every other value of them.
    (void)read(true, to_unsigned, in.data() + 10, -1, reinterpret_cast<std::byte*>(out.data()), nullptr, out.size());
    EXPECT_EQ(out, (std::vector<std::uint16_t>{65535, 32769, 32768, 32767, 1, 0}));
    std::vector<std::uint16_t> every_other(3);
    (void)read(true, to_unsigned, in.data() + 2, 2, reinterpret_cast<std::byte*>(every_other.data()), nullptr, 3);
    EXPECT_EQ(every_other, (std::vector<std::uint16_t>{1, 32768, 65535}));

    std::vector<float> floats(stored.size());
    (void)read(true, to_float, in.data(), 1, reinterpret_cast<std::byte*>(floats.data()), nullptr, floats.size());
    EXPECT_EQ(floats, (std::vector<float>{0.0F, 1.0F, 32767.0F, 32768.0F, 32769.0F, 65535.0F}));

    std::vector<std::byte> data = in;
    (void)kernels::read_in_place<true>(to_unsigned, data.data(), nullptr, stored.size());
    std::vector<std::uint16_t> in_place(stored.size());
    std::memcpy(in_place.data(), data.data(), data.size());
    EXPECT_EQ(in_place, forwards);
}

// BLANK of integer data under each policy (standard §4.4.2.5): the value in the array before scaling.
TEST(conversion, blank_of_integer_data_under_each_policy)
{
    const std::vector<std::byte> in = [] {
        std::vector<std::byte> bytes;
        for (const std::int16_t value : {std::int16_t{-32768}, std::int16_t{7}}) {
            const std::vector<std::byte> encoded = encode(value, true);
            bytes.insert(bytes.end(), encoded.begin(), encoded.end());
        }
        return bytes;
    }();
    const auto conversion = [](numeric_type to, blank_policy policy, double value = 0.0) {
        return make_read_conversion(bitpix::int16, to, {.zero = 32768.0}, -32768, policy, value,
                                    overflow_policy::refuse, rounding::truncate, 0);
    };

    // NaN in a floating-point destination; in an integer one, BLANK converted as any stored value.
    std::array<float, 2> floats{};
    (void)read(true, conversion(numeric_type::float32, blank_policy::nan), in.data(), 1,
               reinterpret_cast<std::byte*>(floats.data()), nullptr, 2);
    EXPECT_TRUE(std::isnan(floats[0]));
    EXPECT_EQ(floats[1], 32775.0F);
    std::array<std::uint16_t, 2> integers{};
    (void)read(true, conversion(numeric_type::uint16, blank_policy::nan), in.data(), 1,
               reinterpret_cast<std::byte*>(integers.data()), nullptr, 2);
    EXPECT_EQ(integers, (std::array<std::uint16_t, 2>{0, 32775}));

    // The caller's value.
    (void)read(true, conversion(numeric_type::uint16, blank_policy::value, 65535.0), in.data(), 1,
               reinterpret_cast<std::byte*>(integers.data()), nullptr, 2);
    EXPECT_EQ(integers, (std::array<std::uint16_t, 2>{65535, 32775}));
    (void)read(true, conversion(numeric_type::float32, blank_policy::value, -1.0), in.data(), 1,
               reinterpret_cast<std::byte*>(floats.data()), nullptr, 2);
    EXPECT_EQ(floats, (std::array<float, 2>{-1.0F, 32775.0F}));

    // A mask: NaN or 0 in the destination.
    std::array<std::uint8_t, 2> mask{9, 9};
    (void)read(true, conversion(numeric_type::uint16, blank_policy::mask), in.data(), 1,
               reinterpret_cast<std::byte*>(integers.data()), mask.data(), 2);
    EXPECT_EQ(integers, (std::array<std::uint16_t, 2>{0, 32775}));
    EXPECT_EQ(mask, (std::array<std::uint8_t, 2>{1, 0}));
    mask = {9, 9};
    (void)read(true, conversion(numeric_type::float64, blank_policy::mask), in.data(), 1,
               reinterpret_cast<std::byte*>(floats.data()), mask.data(), 1);
    EXPECT_EQ(mask[0], 1U);
}

TEST(conversion, a_mask_without_undefined_values_is_all_zeros)
{
    const std::vector<std::byte> in = encode(std::int16_t{5}, true);
    const read_conversion conversion =
        make_read_conversion(bitpix::int16, numeric_type::int16, {}, std::nullopt, blank_policy::mask, 0.0,
                             overflow_policy::refuse, rounding::truncate, 0);
    std::int16_t out = 0;
    std::uint8_t mask = 9;
    (void)read(true, conversion, in.data(), 1, reinterpret_cast<std::byte*>(&out), &mask, 1);
    EXPECT_EQ(out, 5);
    EXPECT_EQ(mask, 0U);
}

// NaN in floating-point data (standard §5.3), into every kind of destination.
TEST(conversion, nan_of_floating_point_data_under_each_policy)
{
    // A NaN with a payload, which a floating-point destination of its type keeps bit for bit.
    const auto payload = std::bit_cast<float>(std::uint32_t{0x7FC0'1234U});
    const std::vector<std::byte> in = encode(payload, true);
    const auto conversion = [](numeric_type to, blank_policy policy, double value = 0.0) {
        return make_read_conversion(bitpix::float32, to, {}, std::nullopt, policy, value, overflow_policy::clamp,
                                    rounding::truncate, 0);
    };

    float same = 0.0F;
    (void)read(true, conversion(numeric_type::float32, blank_policy::nan), in.data(), 1,
               reinterpret_cast<std::byte*>(&same), nullptr, 1);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(same), 0x7FC0'1234U);
    double wider = 0.0;
    (void)read(true, conversion(numeric_type::float64, blank_policy::nan), in.data(), 1,
               reinterpret_cast<std::byte*>(&wider), nullptr, 1);
    EXPECT_TRUE(std::isnan(wider));

    // An integer destination holds no NaN: refused, even when overflows are clamped, but for a policy.
    for (int to = 0; to <= static_cast<int>(numeric_type::uint64); ++to) {
        std::array<std::byte, 8> out{};
        EXPECT_TRUE(throws<invalid_data_error>(errc::undefined_value, [&] {
            (void)read(true, conversion(static_cast<numeric_type>(to), blank_policy::nan), in.data(), 1, out.data(),
                       nullptr, 1);
        })) << to;
    }
    std::int32_t integer = 0;
    (void)read(true, conversion(numeric_type::int32, blank_policy::value, -999.0), in.data(), 1,
               reinterpret_cast<std::byte*>(&integer), nullptr, 1);
    EXPECT_EQ(integer, -999);
    std::uint8_t mask = 0;
    integer = 5;
    (void)read(true, conversion(numeric_type::int32, blank_policy::mask), in.data(), 1,
               reinterpret_cast<std::byte*>(&integer), &mask, 1);
    EXPECT_EQ(integer, 0);
    EXPECT_EQ(mask, 1U);
}

TEST(conversion, a_value_for_undefined_values_must_be_one_of_the_destination_type)
{
    for (const double value : {0.5, 256.0, -1.0, std::numeric_limits<double>::quiet_NaN()}) {
        EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] {
            (void)make_read_conversion(bitpix::int16, numeric_type::uint8, {}, 0, blank_policy::value, value,
                                       overflow_policy::refuse, rounding::truncate, 0);
        })) << value;
    }
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] {
        (void)make_read_conversion(bitpix::int16, numeric_type::float32, {}, 0, blank_policy::value, 1e300,
                                   overflow_policy::refuse, rounding::truncate, 0);
    }));
    // NaN and infinities are values of floating-point types.
    EXPECT_NO_THROW((void)make_read_conversion(bitpix::int16, numeric_type::float32, {}, 0, blank_policy::value,
                                               std::numeric_limits<double>::infinity(), overflow_policy::refuse,
                                               rounding::truncate, 0));
}

// The writes, from each type of the application into each BITPIX: the inverse of the scaling, rounded half away from
// zero into integers.
template <typename Source, typename Stored> void check_writes(numeric_type source, bitpix stored_bitpix)
{
    for (const scaling_kind kind :
         {scaling_kind::none, scaling_kind::unsigned_convention, scaling_kind::halves, scaling_kind::negative}) {
        if (kind == scaling_kind::unsigned_convention && std::floating_point<Stored>) {
            continue;
        }
        const scaling values = scaling_of(kind, stored_bitpix);
        for (const bool swap : {true, false}) {
            for (const overflow_policy overflow : {overflow_policy::refuse, overflow_policy::clamp}) {
                const write_conversion conversion =
                    make_write_conversion(source, stored_bitpix, values, std::nullopt, overflow, 0);
                std::vector<Source> inputs;
                for (const auto value : stored_values<Source>()) {
                    inputs.push_back(value);
                }
                for (const Source value : inputs) {
                    SCOPED_TRACE(testing::Message() << "value " << kernels::text_of(value) << ", scale " << values.scale
                                                    << ", zero " << values.zero << ", swap " << swap);
                    if constexpr (std::floating_point<Source> && !std::floating_point<Stored>) {
                        if (std::isnan(value)) {
                            continue;
                        }
                    }
                    // The reference: the stored value is the physical one minus BZERO, over BSCALE, rounded.
                    expectation<Stored> expected{};
                    if constexpr (!std::floating_point<Source>) {
                        if (values.scale == 1.0 && values.zero == 0.0) {
                            expected = expected_integer<Stored>(value);
                        } else if (values.scale == 1.0 && values.zero == two_to_63 && stored_bitpix == bitpix::int64) {
                            expected = std::cmp_less(value, 0)
                                           ? expectation<Stored>{.clamped = clamp_of<Stored>(true)}
                                           : expected_integer<Stored>(static_cast<std::int64_t>(
                                                 static_cast<std::uint64_t>(value) - (std::uint64_t{1} << 63U)));
                        } else if (values.scale == 1.0 && std::trunc(values.zero) == values.zero &&
                                   sizeof(Source) <= 4) {
                            expected = expected_integer<Stored>(static_cast<std::int64_t>(value) -
                                                                static_cast<std::int64_t>(values.zero));
                        } else {
                            expected = expected_real<Stored>((static_cast<double>(value) - values.zero) / values.scale,
                                                             rounding::nearest);
                        }
                    } else {
                        expected =
                            expected_real<Stored>(values.scale == 1.0 && values.zero == 0.0
                                                      ? static_cast<double>(value)
                                                      : (static_cast<double>(value) - values.zero) / values.scale,
                                                  rounding::nearest);
                    }
                    std::array<std::byte, sizeof(Stored)> out{};
                    if (!expected.value && overflow == overflow_policy::refuse) {
                        EXPECT_TRUE(throws<invalid_data_error>(errc::value_out_of_range, [&] {
                            (void)write(swap, conversion, reinterpret_cast<const std::byte*>(&value), out.data(), 1);
                        }));
                        continue;
                    }
                    const std::uint64_t clamped =
                        write(swap, conversion, reinterpret_cast<const std::byte*>(&value), out.data(), 1);
                    const auto written = decode<Stored>(out, swap);
                    EXPECT_TRUE(same_value(written, expected.value.value_or(expected.clamped)))
                        << kernels::text_of(written) << " instead of "
                        << kernels::text_of(expected.value.value_or(expected.clamped));
                    EXPECT_EQ(clamped, expected.value ? 0U : 1U);
                }
            }
        }
    }
}

template <typename Source> void with_stored_of_writes(numeric_type source, bitpix stored)
{
    switch (stored) {
    case bitpix::uint8:
        check_writes<Source, std::uint8_t>(source, stored);
        return;
    case bitpix::int16:
        check_writes<Source, std::int16_t>(source, stored);
        return;
    case bitpix::int32:
        check_writes<Source, std::int32_t>(source, stored);
        return;
    case bitpix::int64:
        check_writes<Source, std::int64_t>(source, stored);
        return;
    case bitpix::float32:
        check_writes<Source, float>(source, stored);
        return;
    case bitpix::float64:
        check_writes<Source, double>(source, stored);
        return;
    }
}

TEST_P(conversion_matrix, writes_each_value_as_the_reference_converts_it)
{
    const auto [stored, source] = GetParam();
    switch (source) {
    case numeric_type::int8:
        with_stored_of_writes<std::int8_t>(source, stored);
        return;
    case numeric_type::uint8:
        with_stored_of_writes<std::uint8_t>(source, stored);
        return;
    case numeric_type::int16:
        with_stored_of_writes<std::int16_t>(source, stored);
        return;
    case numeric_type::uint16:
        with_stored_of_writes<std::uint16_t>(source, stored);
        return;
    case numeric_type::int32:
        with_stored_of_writes<std::int32_t>(source, stored);
        return;
    case numeric_type::uint32:
        with_stored_of_writes<std::uint32_t>(source, stored);
        return;
    case numeric_type::int64:
        with_stored_of_writes<std::int64_t>(source, stored);
        return;
    case numeric_type::uint64:
        with_stored_of_writes<std::uint64_t>(source, stored);
        return;
    case numeric_type::float32:
        with_stored_of_writes<float>(source, stored);
        return;
    case numeric_type::float64:
        with_stored_of_writes<double>(source, stored);
        return;
    }
}

// The kernels convert real values a block at a time, without checks when every value of a block lies in the range:
// a run converts as each value alone, whatever its blocks hold, values beyond the range, NaN and values beyond the
// range that round into it among them.
TEST(conversion, a_run_of_real_values_written_converts_as_each_value_alone)
{
    const write_conversion conversion = make_write_conversion(
        numeric_type::float64, bitpix::int16, {.scale = 0.5, .zero = 10.0}, -32768, overflow_policy::clamp, 0);
    // Many of them halves, over three blocks of the kernels and part of a fourth, each block with one value that needs
    // a check: below the range by less than a half, which rounds to its bound, beyond it, NaN, and below it.
    std::vector<double> values(1700);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = -100.0 + (static_cast<double>(i) * 0.25);
    }
    values[300] = 10.0 - (32768.3 * 0.5);
    values[700] = 1e9;
    values[1200] = std::numeric_limits<double>::quiet_NaN();
    values[1699] = -1e9;
    std::vector<std::byte> run(values.size() * 2);
    for (const bool swap : {true, false}) {
        const auto stored = [&](std::size_t i) { return decode<std::int16_t>(std::span(run).subspan(i * 2, 2), swap); };
        EXPECT_EQ(write(swap, conversion, reinterpret_cast<const std::byte*>(values.data()), run.data(), values.size()),
                  2U);
        for (std::size_t i = 0; i < values.size(); ++i) {
            std::array<std::byte, 2> alone{};
            (void)write(swap, conversion, reinterpret_cast<const std::byte*>(&values[i]), alone.data(), 1);
            EXPECT_EQ(stored(i), decode<std::int16_t>(alone, swap)) << "value " << i;
        }
        // -99.75 is -219.5 stored, a half, which rounds away from zero.
        EXPECT_EQ(stored(1), -220);
        EXPECT_EQ(stored(300), -32768);
        EXPECT_EQ(stored(700), 32767);
        EXPECT_EQ(stored(1200), -32768);
        EXPECT_EQ(stored(1699), -32768);
    }

    const write_conversion refusing = make_write_conversion(
        numeric_type::float64, bitpix::int16, {.scale = 0.5, .zero = 10.0}, -32768, overflow_policy::refuse, 0);
    EXPECT_TRUE(throws<invalid_data_error>(errc::value_out_of_range, [&] {
        (void)write(true, refusing, reinterpret_cast<const std::byte*>(values.data()), run.data(), values.size());
    }));
}

TEST(conversion, a_nan_written_into_integer_data_is_blank_or_refused)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::array<std::byte, 2> out{};
    const write_conversion with_blank =
        make_write_conversion(numeric_type::float32, bitpix::int16, {}, -32768, overflow_policy::refuse, 0);
    (void)write(true, with_blank, reinterpret_cast<const std::byte*>(&nan), out.data(), 1);
    EXPECT_EQ(decode<std::int16_t>(out, true), -32768);

    const write_conversion without =
        make_write_conversion(numeric_type::float32, bitpix::int16, {}, std::nullopt, overflow_policy::clamp, 0);
    EXPECT_TRUE(throws<invalid_data_error>(errc::undefined_value, [&] {
        (void)write(true, without, reinterpret_cast<const std::byte*>(&nan), out.data(), 1);
    }));

    // Floating-point data hold it as it is.
    const auto payload = std::bit_cast<float>(std::uint32_t{0x7FC0'4321U});
    std::array<std::byte, 4> stored{};
    const write_conversion to_float =
        make_write_conversion(numeric_type::float32, bitpix::float32, {}, std::nullopt, overflow_policy::refuse, 0);
    (void)write(true, to_float, reinterpret_cast<const std::byte*>(&payload), stored.data(), 1);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(decode<float>(stored, true)), 0x7FC0'4321U);
}

TEST(conversion, bitpix_of_keyword_values)
{
    EXPECT_EQ(openfits::detail::bitpix_of(8), bitpix::uint8);
    EXPECT_EQ(openfits::detail::bitpix_of(-64), bitpix::float64);
    for (const std::int64_t invalid : {0, 1, 12, -8, -16, 128}) {
        EXPECT_FALSE(openfits::detail::bitpix_of(invalid).has_value()) << invalid;
    }
    EXPECT_EQ(openfits::detail::stored_size(bitpix::float32), 4U);
    EXPECT_EQ(openfits::detail::stored_type(bitpix::uint8), numeric_type::uint8);
}

} // namespace
