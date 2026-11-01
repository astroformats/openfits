// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "core/decimal.h"

#include <gtest/gtest.h>

#include <limits>
#include <string>

namespace {

using openfits::detail::decimal_digits;
using openfits::detail::exact_digits;
using openfits::detail::round_fixed;
using openfits::detail::round_significant;

TEST(decimal, exact_digits_are_those_of_the_binary_value)
{
    const decimal_digits half = exact_digits(0.5);
    EXPECT_EQ(half.digits, "5");
    EXPECT_EQ(half.exponent, 0);

    const decimal_digits large = exact_digits(-1234.5);
    EXPECT_EQ(large.digits, "12345");
    EXPECT_EQ(large.exponent, 4);

    // 0.1 is not a binary fraction: its double has 55 significant digits.
    const decimal_digits tenth = exact_digits(0.1);
    EXPECT_EQ(tenth.digits, "1000000000000000055511151231257827021181583404541015625");
    EXPECT_EQ(tenth.exponent, 0);

    EXPECT_EQ(exact_digits(0.0).digits, "");
    EXPECT_EQ(exact_digits(-0.0).exponent, 0);
}

TEST(decimal, exact_digits_reach_the_ends_of_the_range_of_double)
{
    const decimal_digits smallest = exact_digits(std::numeric_limits<double>::denorm_min());
    EXPECT_EQ(smallest.exponent, -323);
    EXPECT_EQ(smallest.digits.substr(0, 10), "4940656458");
    EXPECT_EQ(smallest.digits.size(), 751U);

    const decimal_digits largest = exact_digits(std::numeric_limits<double>::max());
    EXPECT_EQ(largest.exponent, 309);
    EXPECT_EQ(largest.digits.substr(0, 10), "1797693134");
}

// Half away from zero, on the exact value: a tie rounds up, and so does anything above it.
TEST(decimal, significant_digits_round_half_away_from_zero)
{
    EXPECT_EQ(round_significant(exact_digits(0.125), 2).digits, "13");
    EXPECT_EQ(round_significant(exact_digits(0.375), 2).digits, "38");
    EXPECT_EQ(round_significant(exact_digits(2.5), 1).digits, "3");
    EXPECT_EQ(round_significant(exact_digits(2.4999), 1).digits, "2");
    // 0.15 is a little below its decimal text in binary, so it rounds down.
    EXPECT_EQ(round_significant(exact_digits(0.15), 1).digits, "1");
}

TEST(decimal, significant_digits_pad_with_zeros_and_carry_into_a_new_digit)
{
    const decimal_digits padded = round_significant(exact_digits(1.5), 4);
    EXPECT_EQ(padded.digits, "1500");
    EXPECT_EQ(padded.exponent, 1);

    const decimal_digits carried = round_significant(exact_digits(9.96), 2);
    EXPECT_EQ(carried.digits, "10");
    EXPECT_EQ(carried.exponent, 2);

    const decimal_digits zero = round_significant(exact_digits(0.0), 3);
    EXPECT_EQ(zero.digits, "000");
    EXPECT_EQ(zero.exponent, 0);
}

TEST(decimal, fixed_digits_round_to_their_places)
{
    auto fixed = round_fixed(exact_digits(123.456), 2);
    EXPECT_EQ(fixed.integer, "123");
    EXPECT_EQ(fixed.fraction, "46");

    fixed = round_fixed(exact_digits(0.05), 3);
    EXPECT_EQ(fixed.integer, "");
    EXPECT_EQ(fixed.fraction, "050");

    // 2.675 is a little below its decimal text in binary, and 99.995 a little above.
    fixed = round_fixed(exact_digits(2.675), 2);
    EXPECT_EQ(fixed.integer, "2");
    EXPECT_EQ(fixed.fraction, "67");

    fixed = round_fixed(exact_digits(99.995), 2);
    EXPECT_EQ(fixed.integer, "100");
    EXPECT_EQ(fixed.fraction, "00");

    fixed = round_fixed(exact_digits(99.9951), 2);
    EXPECT_EQ(fixed.integer, "100");
    EXPECT_EQ(fixed.fraction, "00");

    fixed = round_fixed(exact_digits(1e20), 0);
    EXPECT_EQ(fixed.integer, "100000000000000000000");
    EXPECT_EQ(fixed.fraction, "");
}

// A value whose first digit is just beyond the last place rounds to that place or to zero.
TEST(decimal, fixed_digits_of_small_values_round_to_one_unit_of_the_last_place_or_to_zero)
{
    auto fixed = round_fixed(exact_digits(0.006), 2);
    EXPECT_EQ(fixed.integer, "");
    EXPECT_EQ(fixed.fraction, "01");

    fixed = round_fixed(exact_digits(0.004), 2);
    EXPECT_EQ(fixed.fraction, "00");

    fixed = round_fixed(exact_digits(0.0004), 2);
    EXPECT_EQ(fixed.fraction, "00");

    fixed = round_fixed(exact_digits(0.7), 0);
    EXPECT_EQ(fixed.integer, "1");
    EXPECT_EQ(fixed.fraction, "");

    fixed = round_fixed(exact_digits(0.0), 3);
    EXPECT_EQ(fixed.integer, "");
    EXPECT_EQ(fixed.fraction, "000");
}

} // namespace
