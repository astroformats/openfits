// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "core/checked_math.h"

#include "support/throws.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace {

using openfits::errc;
using openfits::invalid_data_error;
using openfits::detail::checked_add;
using openfits::detail::checked_cast;
using openfits::detail::checked_multiply;
using openfits::detail::checked_subtract;
using openfits::test::throws;

constexpr std::uint64_t max64 = std::numeric_limits<std::uint64_t>::max();
constexpr std::int64_t smax = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t smin = std::numeric_limits<std::int64_t>::min();
constexpr std::size_t max_size = std::numeric_limits<std::size_t>::max();

template <typename Function> testing::AssertionResult overflows(Function&& function)
{
    return throws<invalid_data_error>(errc::arithmetic_overflow, std::forward<Function>(function));
}

TEST(checked_math, add_returns_every_unsigned_sum_that_fits)
{
    EXPECT_EQ(checked_add<std::uint64_t>(max64 - 1, 1), max64);
    EXPECT_EQ(checked_add<std::uint64_t>(0, max64), max64);
    EXPECT_EQ(checked_add<std::size_t>(max_size - 2, 2), max_size);
    EXPECT_EQ(checked_add<std::uint32_t>(0xFFFF'FF00U, 0xFFU), 0xFFFF'FFFFU);
}

TEST(checked_math, add_throws_on_unsigned_overflow)
{
    EXPECT_TRUE(overflows([] { (void)checked_add<std::uint64_t>(max64, 1); }));
    EXPECT_TRUE(overflows([] { (void)checked_add<std::size_t>(1, max_size); }));
    EXPECT_TRUE(overflows([] { (void)checked_add<std::uint32_t>(0xFFFF'FF00U, 0x100U); }));
}

TEST(checked_math, add_returns_every_signed_sum_that_fits)
{
    EXPECT_EQ(checked_add<std::int64_t>(smax - 1, 1), smax);
    EXPECT_EQ(checked_add<std::int64_t>(smin + 1, -1), smin);
    EXPECT_EQ(checked_add<std::int64_t>(smax, smin), -1);
    EXPECT_EQ(checked_add<std::int64_t>(-5, 3), -2);
}

TEST(checked_math, add_throws_on_signed_overflow_in_either_direction)
{
    EXPECT_TRUE(overflows([] { (void)checked_add<std::int64_t>(smax, 1); }));
    EXPECT_TRUE(overflows([] { (void)checked_add<std::int64_t>(smin, -1); }));
    EXPECT_TRUE(overflows([] { (void)checked_add<std::int64_t>(smin, smin); }));
}

TEST(checked_math, subtract_returns_every_difference_that_fits)
{
    EXPECT_EQ(checked_subtract<std::uint64_t>(max64, max64), 0U);
    EXPECT_EQ(checked_subtract<std::size_t>(5, 2), 3U);
    EXPECT_EQ(checked_subtract<std::int64_t>(smin + 1, 1), smin);
    EXPECT_EQ(checked_subtract<std::int64_t>(smax - 1, -1), smax);
    EXPECT_EQ(checked_subtract<std::int64_t>(-1, smin), smax);
}

TEST(checked_math, subtract_throws_below_zero_and_on_signed_overflow)
{
    EXPECT_TRUE(overflows([] { (void)checked_subtract<std::uint64_t>(0, 1); }));
    EXPECT_TRUE(overflows([] { (void)checked_subtract<std::int64_t>(smin, 1); }));
    EXPECT_TRUE(overflows([] { (void)checked_subtract<std::int64_t>(smax, -1); }));
    EXPECT_TRUE(overflows([] { (void)checked_subtract<std::int64_t>(0, smin); }));
}

TEST(checked_math, multiply_returns_every_unsigned_product_that_fits)
{
    EXPECT_EQ(checked_multiply<std::uint64_t>(max64 / 3, 3), max64);
    EXPECT_EQ(checked_multiply<std::uint64_t>(0, max64), 0U);
    EXPECT_EQ(checked_multiply<std::uint64_t>(max64, 0), 0U);
    EXPECT_EQ(checked_multiply<std::size_t>(max_size, 1), max_size);
    EXPECT_EQ(checked_multiply<std::uint32_t>(65535, 65537), 0xFFFF'FFFFU);
}

TEST(checked_math, multiply_throws_on_unsigned_overflow)
{
    EXPECT_TRUE(
        overflows([] { (void)checked_multiply<std::uint64_t>(std::uint64_t{1} << 32, std::uint64_t{1} << 32); }));
    EXPECT_TRUE(overflows([] { (void)checked_multiply<std::uint64_t>(max64 / 3 + 1, 3); }));
    EXPECT_TRUE(overflows([] { (void)checked_multiply<std::size_t>(max_size, 2); }));
    EXPECT_TRUE(overflows([] { (void)checked_multiply<std::uint32_t>(65536, 65536); }));
}

// The four combinations of signs, each at the bound of its result.
TEST(checked_math, multiply_returns_every_signed_product_that_fits)
{
    EXPECT_EQ(checked_multiply<std::int64_t>(smax / 2, 2), smax - 1);
    EXPECT_EQ(checked_multiply<std::int64_t>(smin / 2, 2), smin);
    EXPECT_EQ(checked_multiply<std::int64_t>(2, smin / 2), smin);
    EXPECT_EQ(checked_multiply<std::int64_t>(-(smax / 2), -2), smax - 1);
    EXPECT_EQ(checked_multiply<std::int64_t>(smin, 1), smin);
    EXPECT_EQ(checked_multiply<std::int64_t>(smax, -1), -smax);
    EXPECT_EQ(checked_multiply<std::int64_t>(smin, 0), 0);
    EXPECT_EQ(checked_multiply<std::int64_t>(0, smin), 0);
}

TEST(checked_math, multiply_throws_on_signed_overflow_in_every_combination_of_signs)
{
    EXPECT_TRUE(overflows([] { (void)checked_multiply<std::int64_t>(smax / 2 + 1, 2); }));
    EXPECT_TRUE(overflows([] { (void)checked_multiply<std::int64_t>(smin / 2 - 1, 2); }));
    EXPECT_TRUE(overflows([] { (void)checked_multiply<std::int64_t>(2, smin / 2 - 1); }));
    EXPECT_TRUE(overflows([] { (void)checked_multiply<std::int64_t>(-(smax / 2) - 1, -2); }));
    // The product that two's complement cannot negate.
    EXPECT_TRUE(overflows([] { (void)checked_multiply<std::int64_t>(smin, -1); }));
    EXPECT_TRUE(overflows([] { (void)checked_multiply<std::int64_t>(-1, smin); }));
}

TEST(checked_math, cast_returns_every_value_the_target_represents)
{
    EXPECT_EQ(checked_cast<std::uint8_t>(255), 255);
    EXPECT_EQ(checked_cast<std::int8_t>(-128), -128);
    EXPECT_EQ(checked_cast<std::size_t>(max64), max64);
    EXPECT_EQ(checked_cast<std::uint64_t>(smax), static_cast<std::uint64_t>(smax));
    EXPECT_EQ(checked_cast<std::int64_t>(std::uint64_t{0}), 0);
}

TEST(checked_math, cast_throws_when_the_target_cannot_represent_the_value)
{
    EXPECT_TRUE(overflows([] { (void)checked_cast<std::uint8_t>(256); }));
    EXPECT_TRUE(overflows([] { (void)checked_cast<std::int8_t>(-129); }));
    EXPECT_TRUE(overflows([] { (void)checked_cast<std::uint64_t>(-1); }));
    EXPECT_TRUE(overflows([] { (void)checked_cast<std::int64_t>(max64); }));
    EXPECT_TRUE(overflows([] { (void)checked_cast<std::size_t>(smin); }));
}

} // namespace
