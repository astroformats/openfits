// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "compress/quantize.h"

#include <openfits/compression.h>

#include "compress/dither.h"
#include "support/sequence.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace {

using openfits::quantization;
using openfits::detail::dequantize;
using openfits::detail::dequantize_request;
using openfits::detail::dither_numbers;
using openfits::detail::dither_walk;
using openfits::detail::quantize;
using openfits::detail::quantize_request;
using openfits::detail::quantized_null;
using openfits::detail::quantized_scaling;
using openfits::detail::quantized_zero;
using openfits::test::splitmix64;

// A sky of noise: a level, a slope and a uniform noise of the given amplitude.
std::vector<float> sky(std::size_t count, float amplitude, std::uint64_t seed)
{
    splitmix64 sequence(seed);
    std::vector<float> values(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double uniform = static_cast<double>(sequence.next() >> 11U) / 9007199254740992.0;
        values[i] = 1000.0F + (0.01F * static_cast<float>(i % 100)) + (amplitude * static_cast<float>(uniform - 0.5));
    }
    return values;
}

std::vector<float> restored(const std::vector<std::int32_t>& codes, const quantized_scaling& scaling,
                            quantization method, std::optional<std::int32_t> null = quantized_null)
{
    std::vector<float> values(codes.size());
    dequantize(codes, dequantize_request{.scaling = scaling, .method = method, .row = 1, .seed = 1, .null = null},
               std::span<float>(values));
    return values;
}

TEST(dither, numbers_are_those_of_appendix_i)
{
    const auto& numbers = dither_numbers();
    // The first number is the seed after one step, 16807 / (2^31 - 1).
    EXPECT_EQ(std::bit_cast<std::uint32_t>(numbers[0]), 0x37034E00U);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(numbers[1]), 0x3E06B1D8U);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(numbers[2]), 0x3F416F5AU);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(numbers[9998]), 0x3F310015U);
    // The 10,000th seed is 1043618065 (standard Appendix I).
    EXPECT_EQ(numbers[9999], static_cast<float>(1043618065.0 / 2147483647.0));
    EXPECT_EQ(std::bit_cast<std::uint32_t>(numbers[9999]), 0x3EF8D164U);
}

TEST(dither, a_tile_starts_from_its_row_and_the_seed)
{
    const auto& numbers = dither_numbers();
    // Row 1 under ZDITHER0 1 starts from number 0, whose 500th part is 0; row 2 from number 65 of number 1.
    EXPECT_EQ(dither_walk(1, 1).next(), numbers[0]);
    dither_walk second(2, 1);
    EXPECT_EQ(second.next(), numbers[65]);
    EXPECT_EQ(second.next(), numbers[66]);
    EXPECT_EQ(dither_walk(1, 3).next(), numbers[377]);
    // The rows after 10000 - ZDITHER0 wrap around.
    EXPECT_EQ(dither_walk(10'000, 1).next(), numbers[static_cast<std::size_t>(numbers[9999] * 500.0F)]);
    EXPECT_EQ(dither_walk(10'001, 1).next(), numbers[0]);
}

TEST(dither, a_walk_moves_to_the_next_start_at_the_end_of_the_numbers)
{
    const auto& numbers = dither_numbers();
    // Row 2 under ZDITHER0 1 starts at number 65 and reaches the end after 9935 pixels; then it starts again from
    // number 2's part, 377, and not after 500 pixels as the text of standard §10.2.1 says.
    dither_walk walk(2, 1);
    for (int i = 0; i < 9934; ++i) {
        (void)walk.next();
    }
    EXPECT_EQ(walk.next(), numbers[9999]);
    EXPECT_EQ(walk.next(), numbers[377]);
}

TEST(quantize, restores_noisy_values_within_the_spacing)
{
    const std::vector<float> values = sky(std::size_t{37} * 23, 4.0F, 1);
    for (const quantization method :
         {quantization::no_dither, quantization::subtractive_dither_1, quantization::subtractive_dither_2}) {
        std::vector<std::int32_t> codes(values.size());
        const std::optional<quantized_scaling> scaling =
            quantize(values, 37, 23, false, quantize_request{.level = 4.0, .method = method}, codes);
        ASSERT_TRUE(scaling.has_value());
        if (!scaling) {
            continue;
        }
        // About a quarter of the noise, whose deviation is 4 / sqrt(12).
        EXPECT_GT(scaling->scale, 0.1);
        EXPECT_LT(scaling->scale, 0.5);
        const std::vector<float> back = restored(codes, *scaling, method);
        for (std::size_t i = 0; i < values.size(); ++i) {
            ASSERT_LE(std::abs(back[i] - values[i]), scaling->scale) << i;
        }
    }
}

TEST(quantize, a_negative_level_is_the_spacing_in_single_precision)
{
    const std::vector<float> values = sky(100, 1.0F, 2);
    std::vector<std::int32_t> codes(values.size());
    const std::optional<quantized_scaling> scaling =
        quantize(values, 100, 1, false, quantize_request{.level = -0.01}, codes);
    ASSERT_TRUE(scaling.has_value());
    if (scaling) {
        EXPECT_EQ(scaling->scale, 0.009999999776482582);
        // A multiple of the spacing.
        EXPECT_EQ(scaling->zero, std::trunc(scaling->zero / scaling->scale + 0.5) * scaling->scale);
    }
}

TEST(quantize, keeps_nulls_and_zeros)
{
    std::vector<float> values = sky(200, 4.0F, 3);
    values[5] = std::numeric_limits<float>::quiet_NaN();
    values[6] = std::numeric_limits<float>::infinity();
    values[7] = 0.0F;
    values[8] = -0.0F;
    values[9] = std::numeric_limits<float>::denorm_min();
    EXPECT_TRUE(openfits::detail::prepare_tile(values));
    EXPECT_TRUE(std::isnan(values[6]));
    EXPECT_FALSE(std::signbit(values[8]));
    EXPECT_EQ(values[9], 0.0F);
    std::vector<std::int32_t> codes(values.size());
    const std::optional<quantized_scaling> scaling = quantize(
        values, 200, 1, true, quantize_request{.level = 4.0, .method = quantization::subtractive_dither_2}, codes);
    ASSERT_TRUE(scaling.has_value());
    if (!scaling) {
        return;
    }
    EXPECT_EQ(codes[5], quantized_null);
    EXPECT_EQ(codes[6], quantized_null);
    EXPECT_EQ(codes[7], quantized_zero);
    EXPECT_EQ(codes[8], quantized_zero);
    // The codes of a tile with nulls start just above the reserved integers.
    EXPECT_GE(*std::min_element(codes.begin() + 10, codes.end()), -2147483638);
    const std::vector<float> back = restored(codes, *scaling, quantization::subtractive_dither_2);
    EXPECT_TRUE(std::isnan(back[5]));
    EXPECT_TRUE(std::isnan(back[6]));
    EXPECT_EQ(back[7], 0.0F);
    EXPECT_EQ(back[8], 0.0F);
    EXPECT_LE(std::abs(back[20] - values[20]), scaling->scale);
}

TEST(quantize, reads_the_zeros_of_either_reading_of_the_standard)
{
    const std::vector<std::int32_t> codes{quantized_null, quantized_zero, openfits::detail::standard_null};
    const quantized_scaling scaling{.scale = 0.5, .zero = 100.0};
    // CFITSIO's files: ZBLANK -2147483647 is null, -2147483646 is 0.
    std::vector<float> cfitsio = restored(codes, scaling, quantization::subtractive_dither_2, quantized_null);
    EXPECT_TRUE(std::isnan(cfitsio[0]));
    EXPECT_EQ(cfitsio[1], 0.0F);
    EXPECT_FALSE(std::isnan(cfitsio[2]));
    EXPECT_NE(cfitsio[2], 0.0F);
    // The standard's recommendation: ZBLANK -2147483648 is null, -2147483647 is 0.
    std::vector<float> standard =
        restored(codes, scaling, quantization::subtractive_dither_2, openfits::detail::standard_null);
    EXPECT_EQ(standard[0], 0.0F);
    EXPECT_NE(standard[1], 0.0F);
    EXPECT_TRUE(std::isnan(standard[2]));
    // Without ZBLANK, either is 0.
    std::vector<float> neither = restored(codes, scaling, quantization::subtractive_dither_2, std::nullopt);
    EXPECT_EQ(neither[0], 0.0F);
    EXPECT_EQ(neither[1], 0.0F);
}

TEST(quantize, refuses_tiles_it_cannot_quantize)
{
    std::vector<std::int32_t> codes(100, 7);
    // A tile of one pixel, a constant one under a positive level, a ramp without noise.
    EXPECT_FALSE(quantize(std::vector<float>{1.0F}, 1, 1, false, {}, std::span(codes).first(1)).has_value());
    EXPECT_FALSE(quantize(std::vector<float>(100, 5.0F), 100, 1, false, {}, codes).has_value());
    std::vector<float> ramp(100);
    for (std::size_t i = 0; i < ramp.size(); ++i) {
        ramp[i] = static_cast<float>(i);
    }
    EXPECT_FALSE(quantize(ramp, 100, 1, false, {}, codes).has_value());
    // A range beyond 32-bit integers at the spacing asked for.
    std::vector<float> wide = sky(100, 1.0F, 4);
    wide[0] = 1e30F;
    EXPECT_FALSE(quantize(wide, 100, 1, false, quantize_request{.level = -0.001}, codes).has_value());
    // Tiles whose zero point, or whose values once restored, would leave the range of their type: doubles of a noise
    // near the largest double, whose spacing puts the zero point of SUBTRACTIVE_DITHER_2 beyond it, and floats up to
    // the largest float, which a spacing of 1e37 would restore beyond it.
    splitmix64 sequence(5);
    std::vector<double> huge(100);
    std::vector<float> top(100);
    for (std::size_t i = 0; i < huge.size(); ++i) {
        const double uniform = static_cast<double>(sequence.next() >> 11U) / 9007199254740992.0;
        huge[i] = (uniform - 0.5) * 2e306;
        top[i] = static_cast<float>(3e38 + (uniform * 4e37));
    }
    top[0] = std::numeric_limits<float>::max();
    EXPECT_FALSE(quantize(huge, 100, 1, false,
                          quantize_request{.level = 1.5, .method = quantization::subtractive_dither_2}, codes)
                     .has_value());
    EXPECT_FALSE(quantize(top, 100, 1, false, quantize_request{.level = -1e37}, codes).has_value());
    // A constant tile under a negative level is quantized.
    EXPECT_TRUE(quantize(std::vector<float>(100, 5.0F), 100, 1, false, quantize_request{.level = -0.5}, codes));
}

// The zero point of a tile without nulls whose range takes more than 2^31 levels is the mean of its extremes in the
// precision of the pixels, as CFITSIO takes it, which may lie so far from their middle that the codes of one end leave
// 32-bit integers, or reach those of nulls and zeros: such a tile is not quantized, since its codes would restore other
// values. Here the sum of the extremes, 28422671 rounded to a float, puts the zero point half a unit off, 2^27 levels
// at a spacing of 2^-28, so that the larger value would take the code 2^31 (found by fuzz_quantize, whose input had
// these values times 2^27).
TEST(quantize, refuses_tiles_whose_codes_would_restore_other_values)
{
    std::vector<std::int32_t> codes(2);
    for (const quantization method : {quantization::no_dither, quantization::subtractive_dither_1}) {
        EXPECT_FALSE(quantize(std::vector<float>{-14211343.0F, -14211328.0F}, 2, 1, false,
                              quantize_request{.level = -0x1p-28, .method = method}, codes)
                         .has_value())
            << static_cast<int>(method);
    }
    // With a null, the zero point puts the smaller value just above the reserved integers, and the codes hold both.
    std::vector<std::int32_t> three(3);
    const std::vector<float> with_null{-14211343.0F, -14211328.0F, std::numeric_limits<float>::quiet_NaN()};
    const std::optional<quantized_scaling> scaling =
        quantize(with_null, 3, 1, true, quantize_request{.level = -0x1p-28, .method = quantization::no_dither}, three);
    ASSERT_TRUE(scaling.has_value());
    if (scaling) {
        const std::vector<float> back = restored(three, *scaling, quantization::no_dither);
        EXPECT_EQ(back[0], with_null[0]);
        EXPECT_EQ(back[1], with_null[1]);
        EXPECT_TRUE(std::isnan(back[2]));
    }
}

TEST(quantize, a_tile_of_nulls_is_all_null)
{
    std::vector<float> values(50, std::numeric_limits<float>::quiet_NaN());
    std::vector<std::int32_t> codes(values.size());
    const std::optional<quantized_scaling> scaling = quantize(values, 50, 1, true, {}, codes);
    ASSERT_TRUE(scaling.has_value());
    EXPECT_EQ(codes, std::vector<std::int32_t>(50, quantized_null));
    if (scaling) {
        EXPECT_EQ(scaling->scale, 0.25);
        EXPECT_EQ(scaling->zero, 2147483637.0 * 0.25);
    }
}

TEST(quantize, the_seed_of_the_data_sums_the_bytes_of_the_first_tile)
{
    // 1.0 is 3F 80 00 00: 191, and a null counts as CFITSIO's null value in memory, 493.
    EXPECT_EQ(openfits::detail::dither_seed_of(std::vector<float>{1.0F}), 192U);
    EXPECT_EQ(openfits::detail::dither_seed_of(std::vector<float>{1.0F, std::numeric_limits<float>::quiet_NaN()}),
              192U + 493U);
    EXPECT_EQ(openfits::detail::dither_seed_of(std::vector<double>{std::numeric_limits<double>::quiet_NaN()}), 1098U);
    // Modulo 10000, plus 1.
    EXPECT_EQ(openfits::detail::dither_seed_of(std::vector<float>(53, 1.0F)), (53U * 191U) % 10000U + 1U);
}

TEST(quantize, the_noise_of_a_tile_follows_its_deviation)
{
    // Uniform noise of amplitude a has a deviation of a / sqrt(12); the estimators measure it within a few percent.
    const std::vector<float> values = sky(std::size_t{100} * 100, 12.0F, 5);
    const openfits::detail::noise_estimate noise = openfits::detail::estimate_noise(values, 100, 100, false);
    EXPECT_EQ(noise.valid, 10'000U);
    EXPECT_NEAR(noise.noise3, std::sqrt(12.0), 0.5);
    EXPECT_GT(noise.noise2, 0.0);
    EXPECT_GT(noise.noise5, 0.0);
    EXPECT_LT(noise.min, noise.max);
}

} // namespace
