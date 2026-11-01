// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/card.h>
#include <openfits/error.h>

#include "support/sequence.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace {

using openfits::card_value;
using openfits::complex_integer;
using openfits::errc;
using openfits::value_kind;
using openfits::test::throws;

TEST(card_value, a_default_value_has_no_value)
{
    const card_value value;
    EXPECT_EQ(value.kind(), value_kind::none);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(value.data()));
}

TEST(card_value, each_kind_is_held_in_its_type)
{
    EXPECT_EQ(card_value(openfits::undefined).kind(), value_kind::undefined);
    EXPECT_EQ(card_value(true).kind(), value_kind::logical);
    EXPECT_EQ(card_value(std::int64_t{-3}).kind(), value_kind::integer);
    EXPECT_EQ(card_value(2.5).kind(), value_kind::real);
    EXPECT_EQ(card_value(complex_integer{.real = 1, .imag = -2}).kind(), value_kind::complex_integer);
    EXPECT_EQ(card_value(std::complex<double>(1.5, -2)).kind(), value_kind::complex_real);
    EXPECT_EQ(card_value("M 31").kind(), value_kind::string);

    EXPECT_TRUE(card_value(true).get<bool>());
    EXPECT_EQ(card_value(2.5).get<double>(), 2.5);
    EXPECT_EQ(card_value(std::string_view("M 31")).get<std::string>(), "M 31");
    EXPECT_EQ(card_value(std::string("M 31")).get<std::string>(), "M 31");
}

// The integers of every C++ type are held as std::int64_t, and as std::uint64_t from 2^63 on only, so that the same
// integer is always held the same way.
TEST(card_value, integers_are_held_as_int64_and_above_its_range_as_uint64)
{
    EXPECT_EQ(card_value(42).get<std::int64_t>(), 42);
    EXPECT_EQ(card_value(std::int8_t{-128}).get<std::int64_t>(), -128);
    EXPECT_EQ(card_value(std::uint8_t{255}).get<std::int64_t>(), 255);
    EXPECT_EQ(card_value(std::uint16_t{65535}).get<std::int64_t>(), 65535);
    EXPECT_EQ(card_value(4'000'000'000U).get<std::int64_t>(), 4'000'000'000);
    EXPECT_EQ(card_value(7ULL).get<std::int64_t>(), 7);
    EXPECT_EQ(card_value(-7LL).get<std::int64_t>(), -7);
    EXPECT_EQ(card_value(std::numeric_limits<std::int64_t>::min()).get<std::int64_t>(),
              std::numeric_limits<std::int64_t>::min());

    // 2^63, the BZERO of the 64-bit unsigned convention (standard §5.2.5), and 2^64 - 1.
    constexpr std::uint64_t two_to_63 = std::uint64_t{1} << 63U;
    EXPECT_EQ(card_value(two_to_63).get<std::uint64_t>(), two_to_63);
    EXPECT_EQ(card_value(std::numeric_limits<std::uint64_t>::max()).get<std::uint64_t>(),
              std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(card_value(two_to_63 - 1).get<std::int64_t>(), std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(card_value(two_to_63).kind(), value_kind::integer);

    EXPECT_EQ(card_value(std::uint64_t{5}), card_value(std::int64_t{5}));
}

// 'T' would otherwise make the integer 84.
TEST(card_value, character_types_are_refused)
{
    static_assert(!std::is_constructible_v<card_value, char>);
    static_assert(!std::is_constructible_v<card_value, wchar_t>);
    static_assert(!std::is_constructible_v<card_value, char8_t>);
    static_assert(!std::is_constructible_v<card_value, char16_t>);
    static_assert(!std::is_constructible_v<card_value, char32_t>);
    static_assert(!std::is_constructible_v<card_value, std::nullptr_t>);
    static_assert(std::is_constructible_v<card_value, const char*>);
    static_assert(std::is_constructible_v<card_value, signed char>);
    static_assert(std::is_constructible_v<card_value, unsigned char>);
}

// A float is held as the double of its shortest decimal form: 0.1F is 0.1, written as 0.1.
TEST(card_value, a_float_is_held_as_the_double_of_its_shortest_decimal_form)
{
    EXPECT_EQ(card_value(0.1F).get<double>(), 0.1);
    EXPECT_EQ(card_value(300.0F).get<double>(), 300.0);
    EXPECT_EQ(card_value(-1.5e-7F).get<double>(), -1.5e-7);
    EXPECT_EQ(card_value(std::numeric_limits<float>::max()).get<double>(), 3.4028235e38);
    EXPECT_EQ(card_value(std::numeric_limits<float>::denorm_min()).get<double>(), 1e-45);
    EXPECT_TRUE(std::signbit(card_value(-0.0F).get<double>()));
    EXPECT_TRUE(std::isinf(card_value(std::numeric_limits<float>::infinity()).get<double>()));
    EXPECT_TRUE(std::isnan(card_value(std::numeric_limits<float>::quiet_NaN()).get<double>()));

    const card_value parts(std::complex<float>(0.1F, -0.2F));
    EXPECT_EQ(parts.get<std::complex<double>>(), std::complex<double>(0.1, -0.2));
}

// The double reads back as the float it was made of, for every float.
TEST(card_value, a_float_reads_back_as_the_same_float)
{
    openfits::test::splitmix64 bits(20261026);
    for (int i = 0; i < 200'000; ++i) {
        const auto value = std::bit_cast<float>(static_cast<std::uint32_t>(bits.next() >> 32U));
        if (!std::isfinite(value)) {
            continue;
        }
        const double held = card_value(value).get<double>();
        EXPECT_EQ(std::bit_cast<std::uint32_t>(static_cast<float>(held)), std::bit_cast<std::uint32_t>(value)) << value;
    }
}

TEST(card_value, get_of_another_type_is_a_usage_error)
{
    EXPECT_TRUE(
        throws<openfits::usage_error>(errc::invalid_argument, [] { (void)card_value(1.5).get<std::string>(); }));
    EXPECT_TRUE(
        throws<openfits::usage_error>(errc::invalid_argument, [] { (void)card_value(5).get<std::uint64_t>(); }));
    EXPECT_TRUE(throws<openfits::usage_error>(errc::invalid_argument, [] { (void)card_value().get<bool>(); }));
}

TEST(card_value, copies_are_equal_and_independent)
{
    card_value original(std::string(100, 'x'));
    const card_value copy(original);
    card_value assigned;
    assigned = original;
    EXPECT_EQ(copy, original);
    EXPECT_EQ(assigned, original);

    original = card_value(3);
    EXPECT_EQ(copy.get<std::string>(), std::string(100, 'x'));
    EXPECT_NE(copy, original);
    static_assert(std::is_nothrow_move_constructible_v<card_value>);
    static_assert(std::is_nothrow_move_assignable_v<card_value>);
}

} // namespace
