// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/compression.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

// The quantization of floating-point tiles (standard §10.2): the noise of a tile estimated as CFITSIO estimates it, the
// spacing of the levels and the zero point that follow from it, the integers of the pixels with the subtractive
// dithering of standard Appendix I, and their inverse, each step rounded as CFITSIO rounds it, so that a tile given the
// same level and seed gives the same integers, on every host: the arithmetic is never fused into multiply-adds.
//
// The pixels of a tile are those of a buffer of nx x ny values, as CFITSIO shapes a tile: nx the length of the first
// axis of the tile longer than 1, and ny the product of the lengths of the axes after it. Null pixels are NaN, which
// prepare_tile() makes of the infinities too.

namespace openfits::detail {

/// The integer of null pixels, which ZBLANK gives (CFITSIO's NULL_VALUE).
inline constexpr std::int32_t quantized_null = -2147483647;
/// The integer of the pixels that are exactly 0 under SUBTRACTIVE_DITHER_2 in the files of CFITSIO (its ZERO_VALUE),
/// which the text of standard §10.2.1 gives as -2147483647.
inline constexpr std::int32_t quantized_zero = -2147483646;
/// The value of ZBLANK that standard §10.1.3 recommends, under which -2147483647 is the zero of standard §10.2.1.
inline constexpr std::int32_t standard_null = -2147483647 - 1;

/// What the noise estimate of a tile finds: its pixels that are not null, their range, and the noise of its rows from
/// the differences of second, third and fifth order (CFITSIO's FnNoise5).
struct noise_estimate
{
    std::uint64_t valid = 0;
    double min = 0.0;
    double max = 0.0;
    double noise2 = 0.0;
    double noise3 = 0.0;
    double noise5 = 0.0;
};

/// The estimate of a tile of floating-point values, whose null pixels are NaN when nulls is true; the differences are
/// taken in the precision of the values.
[[nodiscard]] noise_estimate estimate_noise(std::span<const float> values, std::size_t nx, std::size_t ny, bool nulls);
[[nodiscard]] noise_estimate estimate_noise(std::span<const double> values, std::size_t nx, std::size_t ny, bool nulls);

/// The estimate of a tile of integers, as H-Compress takes the scale of a tile from it, the differences in 64 bits;
/// the pixels equal to null, when given, are null.
[[nodiscard]] noise_estimate estimate_noise(std::span<const std::int32_t> values, std::size_t nx, std::size_t ny,
                                            std::optional<std::int32_t> null);

/// Makes the values of a tile those that fpack quantizes: NaN and the infinities NaN, the null pixels; negative zero
/// and subnormal values +0. Returns whether a pixel is null.
bool prepare_tile(std::span<float> values) noexcept;
bool prepare_tile(std::span<double> values) noexcept;

/// The seed of the dithering taken from the values of the first tile, as prepare_tile() leaves them: the sum of their
/// bytes, each null pixel counting as CFITSIO's in-memory null value does, modulo 10000, plus 1 (CFITSIO's checksum of
/// the first tile).
[[nodiscard]] std::uint32_t dither_seed_of(std::span<const float> values) noexcept;
[[nodiscard]] std::uint32_t dither_seed_of(std::span<const double> values) noexcept;

/// How a tile is quantized.
struct quantize_request
{
    /// The level: positive, the spacing is the noise of the tile divided by it; negative, the spacing is its absolute
    /// value. Taken in single precision, as fpack and CFITSIO take it.
    double level = 4.0;
    quantization method = quantization::subtractive_dither_1;
    /// The row of the tile in the table, from 1, and ZDITHER0, for the dithering.
    std::uint64_t row = 1;
    std::uint32_t seed = 1;
};

/// ZSCALE and ZZERO of a quantized tile.
struct quantized_scaling
{
    double scale = 1.0;
    double zero = 0.0;
};

/// Quantizes the values of a tile, prepared, into codes (Eq. 12 and 13), with -2147483647 for null pixels and, under
/// SUBTRACTIVE_DITHER_2, -2147483646 for the pixels that are exactly 0. Nothing, and codes unspecified, when the tile
/// cannot be quantized: a tile of one pixel, one without noise for a positive level, one whose range is beyond 32-bit
/// integers, or one in which a value would take a code beyond them or one of the integers of nulls and zeros.
[[nodiscard]] std::optional<quantized_scaling> quantize(std::span<const float> values, std::size_t nx, std::size_t ny,
                                                        bool nulls, const quantize_request& request,
                                                        std::span<std::int32_t> codes);
[[nodiscard]] std::optional<quantized_scaling> quantize(std::span<const double> values, std::size_t nx, std::size_t ny,
                                                        bool nulls, const quantize_request& request,
                                                        std::span<std::int32_t> codes);

/// How a quantized tile is restored.
struct dequantize_request
{
    quantized_scaling scaling{};
    quantization method = quantization::no_dither;
    std::uint64_t row = 1;
    std::uint32_t seed = 1;
    /// The integer of null pixels, which become NaN; none without ZBLANK.
    std::optional<std::int32_t> null{};
    /// Whether the codes are 32-bit integers, among which SUBTRACTIVE_DITHER_2 reserves the zero.
    bool wide = true;
};

/// Restores the values of a tile from its codes (Eq. 14): NaN for the null integer; under SUBTRACTIVE_DITHER_2, 0 for
/// the integer of zeros, -2147483646 under the ZBLANK -2147483647 of CFITSIO's files, -2147483647 under the ZBLANK
/// -2147483648 that the standard recommends, and either without ZBLANK.
void dequantize(std::span<const std::int32_t> codes, const dequantize_request& request, std::span<float> values);
void dequantize(std::span<const std::int32_t> codes, const dequantize_request& request, std::span<double> values);

} // namespace openfits::detail
