// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>
#include <openfits/image.h>
#include <openfits/types.h>

#include "support/throws.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>

namespace {

using openfits::bitpix;
using openfits::errc;
using openfits::image_info;
using openfits::numeric_type;
using openfits::scaling;
using openfits::usage_error;
using openfits::test::throws;

numeric_type type_of(bitpix stored, scaling values = {})
{
    image_info image;
    image.bitpix = stored;
    image.scaling = values;
    return image.value_type();
}

TEST(image_info, the_type_of_unscaled_data_is_the_stored_type)
{
    EXPECT_EQ(type_of(bitpix::uint8), numeric_type::uint8);
    EXPECT_EQ(type_of(bitpix::int16), numeric_type::int16);
    EXPECT_EQ(type_of(bitpix::int32), numeric_type::int32);
    EXPECT_EQ(type_of(bitpix::int64), numeric_type::int64);
    EXPECT_EQ(type_of(bitpix::float32), numeric_type::float32);
    EXPECT_EQ(type_of(bitpix::float64), numeric_type::float64);
}

// Standard §5.2.5, Table 11.
TEST(image_info, the_conventions_of_unsigned_and_signed_integers)
{
    EXPECT_EQ(type_of(bitpix::uint8, {.zero = -128.0}), numeric_type::int8);
    EXPECT_EQ(type_of(bitpix::int16, {.zero = 32768.0}), numeric_type::uint16);
    EXPECT_EQ(type_of(bitpix::int32, {.zero = 2147483648.0}), numeric_type::uint32);
    EXPECT_EQ(type_of(bitpix::int64, {.zero = 9223372036854775808.0}), numeric_type::uint64);
}

TEST(image_info, integer_scalings_give_the_narrowest_integer_type_of_their_values)
{
    // [0, 255] by BZERO 255 and BSCALE -1.
    EXPECT_EQ(type_of(bitpix::uint8, {.scale = -1.0, .zero = 255.0}), numeric_type::uint8);
    EXPECT_EQ(type_of(bitpix::uint8, {.zero = 1.0}), numeric_type::int16);
    EXPECT_EQ(type_of(bitpix::uint8, {.scale = 2.0}), numeric_type::int16);
    EXPECT_EQ(type_of(bitpix::uint8, {.scale = 1000.0}), numeric_type::int32);
    EXPECT_EQ(type_of(bitpix::int16, {.zero = 1.0}), numeric_type::int32);
    EXPECT_EQ(type_of(bitpix::int16, {.scale = 2.0}), numeric_type::int32);
    EXPECT_EQ(type_of(bitpix::int16, {.zero = 32767.0}), numeric_type::int32);
    EXPECT_EQ(type_of(bitpix::int32, {.zero = 1.0}), numeric_type::int64);
    EXPECT_EQ(type_of(bitpix::int32, {.zero = 4294967296.0}), numeric_type::int64);
    // Of two types of one size, the signed one first.
    EXPECT_EQ(type_of(bitpix::int32, {.zero = 2147483648.0 + 4294967296.0}), numeric_type::int64);
    // 64-bit data have every 64-bit integer: any other scaling leaves the integer types.
    EXPECT_EQ(type_of(bitpix::int64, {.zero = 1.0}), numeric_type::float64);
    EXPECT_EQ(type_of(bitpix::int64, {.scale = -1.0}), numeric_type::float64);
}

TEST(image_info, real_scalings_give_float_for_small_integers_and_double_for_large_ones)
{
    EXPECT_EQ(type_of(bitpix::uint8, {.scale = 0.5}), numeric_type::float32);
    EXPECT_EQ(type_of(bitpix::int16, {.scale = 0.01, .zero = 100.0}), numeric_type::float32);
    EXPECT_EQ(type_of(bitpix::int16, {.zero = 0.5}), numeric_type::float32);
    EXPECT_EQ(type_of(bitpix::int32, {.scale = 0.5}), numeric_type::float64);
    EXPECT_EQ(type_of(bitpix::int64, {.scale = 0.5}), numeric_type::float64);
    // Floating-point data keep their type whatever the scaling.
    EXPECT_EQ(type_of(bitpix::float32, {.scale = 2.0, .zero = 1.0}), numeric_type::float32);
}

TEST(image_info, a_model_without_bitpix_has_no_type_of_values)
{
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [] { (void)image_info{}.value_type(); }));
}

TEST(geometry, counts_the_elements_of_its_axes)
{
    EXPECT_EQ((openfits::geometry{}).element_count(), 0U);
    EXPECT_EQ((openfits::geometry{.axes = {37, 23}}).element_count(), 851U);
    EXPECT_EQ((openfits::geometry{.axes = {5, 0, 3}}).element_count(), 0U);
    EXPECT_EQ((openfits::geometry{.axes = {std::uint64_t{1} << 32U, std::uint64_t{1} << 31U}}).element_count(),
              std::uint64_t{1} << 63U);
    EXPECT_TRUE(throws<usage_error>(errc::arithmetic_overflow, [] {
        (void)(openfits::geometry{.axes = {std::uint64_t{1} << 32U, std::uint64_t{1} << 32U}}).element_count();
    }));
}

TEST(numeric_type, of_every_type_of_values)
{
    EXPECT_EQ(openfits::numeric_type_of<std::int8_t>(), numeric_type::int8);
    EXPECT_EQ(openfits::numeric_type_of<std::uint16_t>(), numeric_type::uint16);
    EXPECT_EQ(openfits::numeric_type_of<long long>(), numeric_type::int64);
    EXPECT_EQ(openfits::numeric_type_of<unsigned long long>(), numeric_type::uint64);
    EXPECT_EQ(openfits::numeric_type_of<float>(), numeric_type::float32);
    EXPECT_EQ(openfits::numeric_type_of<double>(), numeric_type::float64);
    static_assert(!openfits::numeric_value<bool>);
    static_assert(!openfits::numeric_value<char>);
    static_assert(!openfits::numeric_value<char8_t>);
    static_assert(!openfits::numeric_value<long double>);
    static_assert(!openfits::numeric_value<const float>);
    EXPECT_EQ(openfits::size_of(numeric_type::uint32), 4U);
    EXPECT_EQ(openfits::size_of(numeric_type::float64), 8U);
}

} // namespace
