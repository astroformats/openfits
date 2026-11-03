// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "compress/quantize.h"

#include "compress/dither.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

// Every step below is one rounding, in the order of CFITSIO's arithmetic, so that the codes are its codes: each is a
// statement of its own, and products and sums are never fused into multiply-adds, which GCC does across statements on
// arm64 and with -march=x86-64-v3, and Clang within one.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

namespace openfits::detail {

namespace {

// The noise of each difference relative to the median of its absolute values, for Gaussian noise (CFITSIO's constants
// of FnNoise5).
constexpr double noise2_factor = 1.0483579;
constexpr double noise3_factor = 0.6052697;
constexpr double noise5_factor = 0.1772048;

// The codes of a tile of nulls start just above the reserved integers, 10 of them above the null one.
constexpr double reserved_start = -2147483637.0;
// The largest range, in levels, that the codes of a tile hold: 2 x (2^31 - 1) less the reserved integers.
constexpr double max_levels = 4294967284.0;
constexpr double half_levels = 2147483637.0;

// CFITSIO's flags of null pixels in the memory of a tile, whose bytes the seed of the first tile sums.
constexpr std::uint32_t float_null_bits = 0x8541F136U;
constexpr std::uint64_t double_null_bits = 0xB8A83E285EBAB4B7U;

// The pixels of a window of nine valid pixels along a row: v[0] is the oldest.
template <typename T> struct window
{
    T v1{}, v2{}, v3{}, v4{}, v5{}, v6{}, v7{}, v8{}, v9{};

    void shift() noexcept
    {
        v1 = v2;
        v2 = v3;
        v3 = v4;
        v4 = v5;
        v5 = v6;
        v6 = v7;
        v7 = v8;
        v8 = v9;
    }
};

// The value at index (n - 1) / 2 of values once ordered, the lower median, by the quickselect of N. Devillard after
// Numerical Recipes, whose exchanges leave values in the order that the next selection over the same array finds:
// CFITSIO's noise2 selects over entries that an earlier row left there, which these exchanges reproduce.
template <typename T> T select_median(std::span<T> values) noexcept
{
    std::size_t low = 0;
    std::size_t high = values.size() - 1;
    const std::size_t median = (values.size() - 1) / 2;
    const auto order = [&values](std::size_t a, std::size_t b) {
        if (values[a] > values[b]) {
            std::swap(values[a], values[b]);
        }
    };
    for (;;) {
        if (high <= low) {
            return values[median];
        }
        if (high == low + 1) {
            order(low, high);
            return values[median];
        }
        const std::size_t middle = (low + high) / 2;
        order(middle, high);
        order(low, high);
        if (values[middle] > values[low]) {
            std::swap(values[middle], values[low]);
        }
        std::swap(values[middle], values[low + 1]);
        std::size_t up = low + 1;
        std::size_t down = high;
        for (;;) {
            do {
                ++up;
            } while (values[low] > values[up]);
            do {
                --down;
            } while (values[down] > values[low]);
            if (down < up) {
                break;
            }
            std::swap(values[up], values[down]);
        }
        std::swap(values[low], values[down]);
        if (down <= median) {
            low = up;
        }
        if (down >= median) {
            high = down - 1;
        }
    }
}

// The bits of a float but its sign, which order the floats that are not NaN as their magnitudes, the zeros of either
// sign as one value; above those of the infinity for NaN.
std::uint32_t magnitude_key(float value) noexcept
{
    return std::bit_cast<std::uint32_t>(value) & 0x7FFF'FFFFU;
}

constexpr std::uint32_t infinity_key = 0x7F80'0000U;

// The value at index (n - 1) / 2 of values once ordered, as select_median() finds it, for the magnitudes of the
// differences of a row of floats, from their keys, 8 bits at a time from the top: the count of the keys by their next 8
// bits finds those of the median, the others are left out, and a selection among the few left completes it. A few
// passes, without the unpredictable branches of the exchanges. Its zero is +0 where select_median() may give -0, which
// every use of the noise takes as the same. Values with a NaN, which do not order, go to select_median().
float median_of_magnitudes(std::span<float> values, std::vector<std::uint32_t>& keys)
{
    keys.resize(values.size());
    std::uint32_t largest = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        keys[i] = magnitude_key(values[i]);
        largest = std::max(largest, keys[i]);
    }
    if (largest > infinity_key) {
        return select_median(values);
    }
    // The candidates are the first count keys, the median at rank among them.
    std::size_t rank = (values.size() - 1) / 2;
    std::size_t count = values.size();
    for (const unsigned shift : {23U, 15U, 7U}) {
        if (count <= 16) {
            break;
        }
        // Two counts, of the even and the odd keys, so that keys of the same digit, which most share, do not wait for
        // each other's increment.
        std::array<std::uint32_t, 256> even_counts{};
        std::array<std::uint32_t, 256> odd_counts{};
        std::size_t i = 0;
        for (; i + 1 < count; i += 2) {
            ++even_counts[(keys[i] >> shift) & 0xFFU];
            ++odd_counts[(keys[i + 1] >> shift) & 0xFFU];
        }
        if (i < count) {
            ++even_counts[(keys[i] >> shift) & 0xFFU];
        }
        std::uint32_t digit = 0;
        for (; even_counts[digit] + odd_counts[digit] <= rank; ++digit) {
            rank -= even_counts[digit] + odd_counts[digit];
        }
        // The keys of the digit moved to the front, without a branch, which half of them would mispredict.
        std::size_t kept = 0;
        for (std::size_t k = 0; k < count; ++k) {
            const std::uint32_t key = keys[k];
            keys[kept] = key;
            kept += ((key >> shift) & 0xFFU) == digit ? 1 : 0;
        }
        count = kept;
    }
    const auto median = keys.begin() + static_cast<std::ptrdiff_t>(rank);
    std::nth_element(keys.begin(), median, keys.begin() + static_cast<std::ptrdiff_t>(count));
    return std::bit_cast<float>(*median);
}

// The median of the third or the fifth differences of a row, whose order of exchanges no later selection sees: each
// row selects over the differences it wrote.
template <typename T> T median_of_differences(std::span<T> values, std::vector<std::uint32_t>& keys)
{
    if constexpr (std::is_same_v<T, float>) {
        return median_of_magnitudes(values, keys);
    } else {
        (void)keys;
        return select_median(values);
    }
}

// The median of the values of the rows: the mean of the two middle values once sorted.
double median_of_rows(std::vector<double>& rows)
{
    if (rows.empty()) {
        return 0.0;
    }
    std::ranges::sort(rows);
    const std::size_t count = rows.size();
    const double sum = rows[(count - 1) / 2] + rows[count / 2];
    return sum / 2.0;
}

// The differences of a window, in the arithmetic of T: of floats in single precision, of integers in 64 bits.
template <typename T> struct differences
{
    using value = std::conditional_t<std::is_integral_v<T>, std::int64_t, T>;

    static value second(const window<value>& w) noexcept
    {
        const value difference = w.v5 - w.v7;
        return difference < 0 ? -difference : difference;
    }

    static value third(const window<value>& w) noexcept
    {
        const value twice = static_cast<value>(2) * w.v5;
        const value first = twice - w.v3;
        const value difference = first - w.v7;
        return difference < 0 ? -difference : difference;
    }

    static value fifth(const window<value>& w) noexcept
    {
        const value six = static_cast<value>(6) * w.v5;
        const value four3 = static_cast<value>(4) * w.v3;
        const value four7 = static_cast<value>(4) * w.v7;
        value sum = six - four3;
        sum = sum - four7;
        sum = sum + w.v1;
        sum = sum + w.v9;
        return sum < 0 ? -sum : sum;
    }
};

// CFITSIO's FnNoise5 over a tile of nx x ny pixels, valid() telling the pixels that are not null.
template <typename T, typename Valid>
noise_estimate noise_of(std::span<const T> values, std::size_t nx, std::size_t ny, Valid valid)
{
    using value = typename differences<T>::value;
    noise_estimate result;
    // The range starts beyond every value, as CFITSIO's, in the type of the pixels.
    T low = std::numeric_limits<T>::max();
    T high = std::numeric_limits<T>::lowest();
    const auto take = [&](T pixel) {
        ++result.valid;
        if (pixel < low) {
            low = pixel;
        }
        if (pixel > high) {
            high = pixel;
        }
    };
    if (nx < 9) {
        nx *= ny;
        ny = 1;
    }
    if (nx < 9) {
        for (std::size_t i = 0; i < nx; ++i) {
            if (valid(values[i])) {
                take(values[i]);
            }
        }
        result.min = static_cast<double>(low);
        result.max = static_cast<double>(high);
        return result;
    }

    // The work arrays of the rows, filled with zeros once for the tile and reused by each row.
    std::vector<value> second(nx, value{});
    std::vector<value> third(nx, value{});
    std::vector<value> fifth(nx, value{});
    std::vector<double> rows2;
    std::vector<double> rows3;
    std::vector<double> rows5;
    std::vector<std::uint32_t> keys;
    for (std::size_t row = 0; row < ny; ++row) {
        const std::span<const T> pixels = values.subspan(row * nx, nx);
        window<value> w;
        std::size_t i = 0;
        // The first eight valid pixels of the row.
        std::size_t found = 0;
        for (; i < nx && found < 8; ++i) {
            if (!valid(pixels[i])) {
                continue;
            }
            take(pixels[i]);
            const auto pixel = static_cast<value>(pixels[i]);
            w.v1 = w.v2;
            w.v2 = w.v3;
            w.v3 = w.v4;
            w.v4 = w.v5;
            w.v5 = w.v6;
            w.v6 = w.v7;
            w.v7 = w.v8;
            w.v8 = pixel;
            ++found;
        }
        if (found < 8) {
            continue;
        }
        std::size_t count = 0;
        std::size_t count2 = 0;
        for (; i < nx; ++i) {
            if (!valid(pixels[i])) {
                continue;
            }
            take(pixels[i]);
            w.v9 = static_cast<value>(pixels[i]);
            if (!(w.v5 == w.v6 && w.v6 == w.v7)) {
                second[count2] = differences<T>::second(w);
                ++count2;
            }
            // A constant region of the row gives no difference.
            if (!(w.v3 == w.v4 && w.v4 == w.v5 && w.v5 == w.v6 && w.v6 == w.v7)) {
                third[count] = differences<T>::third(w);
                fifth[count] = differences<T>::fifth(w);
                ++count;
            }
            w.shift();
        }
        if (count == 0) {
            continue;
        }
        if (count == 1) {
            rows3.push_back(static_cast<double>(third[0]));
            rows5.push_back(static_cast<double>(fifth[0]));
            if (count2 == 1) {
                rows2.push_back(static_cast<double>(second[0]));
            }
            continue;
        }
        rows3.push_back(static_cast<double>(median_of_differences(std::span(third).first(count), keys)));
        rows5.push_back(static_cast<double>(median_of_differences(std::span(fifth).first(count), keys)));
        if (count2 >= 2) {
            // Over count entries, as CFITSIO selects them: those after count2 are what earlier rows left.
            rows2.push_back(static_cast<double>(select_median(std::span(second).first(count))));
        }
    }
    result.min = static_cast<double>(low);
    result.max = static_cast<double>(high);
    result.noise2 = noise2_factor * median_of_rows(rows2);
    result.noise3 = noise3_factor * median_of_rows(rows3);
    result.noise5 = noise5_factor * median_of_rows(rows5);
    return result;
}

// The range of the valid pixels alone, which a negative level needs.
template <typename T> noise_estimate range_of(std::span<const T> values, bool nulls)
{
    noise_estimate result;
    T low = std::numeric_limits<T>::max();
    T high = std::numeric_limits<T>::lowest();
    for (const T pixel : values) {
        if (nulls && std::isnan(pixel)) {
            continue;
        }
        ++result.valid;
        low = std::min(low, pixel);
        high = std::max(high, pixel);
    }
    result.min = static_cast<double>(low);
    result.max = static_cast<double>(high);
    return result;
}

// NINT of CFITSIO: half away from zero, through a real addition of a half.
std::int32_t nearest(double value) noexcept
{
    const double shifted = value >= 0.0 ? value + 0.5 : value - 0.5;
    // Within the codes of a tile that passed the range check; beyond them only for an extreme tile, which C leaves
    // undefined and x86-64 makes -2^31.
    if (std::isnan(shifted) || shifted <= -2147483649.0 || shifted >= 2147483648.0) {
        return std::numeric_limits<std::int32_t>::min();
    }
    return static_cast<std::int32_t>(shifted);
}

template <typename T>
std::optional<quantized_scaling> quantize_tile(std::span<const T> values, std::size_t nx, std::size_t ny, bool nulls,
                                               const quantize_request& request, std::span<std::int32_t> codes)
{
    const std::size_t count = values.size();
    if (count <= 1) {
        return std::nullopt;
    }
    // The level in single precision, as CFITSIO keeps it.
    const auto level = static_cast<float>(request.level);
    noise_estimate statistics;
    double delta = 0.0;
    if (level >= 0.0F) {
        statistics = nulls ? noise_of(values, nx, ny, [](T pixel) { return !std::isnan(pixel); })
                           : noise_of(values, nx, ny, [](T) { return true; });
        double noise = 0.0;
        if (nulls && statistics.valid == 0) {
            statistics.min = 0.0;
            statistics.max = 1.0;
            noise = 1.0;
        } else {
            noise = statistics.noise3;
            if (statistics.noise2 != 0.0 && statistics.noise2 < noise) {
                noise = statistics.noise2;
            }
            if (statistics.noise5 != 0.0 && statistics.noise5 < noise) {
                noise = statistics.noise5;
            }
        }
        delta = level == 0.0F ? noise / 4.0 : noise / static_cast<double>(level);
        if (delta == 0.0) {
            return std::nullopt;
        }
    } else {
        statistics = range_of(values, nulls);
        delta = static_cast<double>(-level);
    }

    // The range in the precision of the pixels.
    const auto low = static_cast<T>(statistics.min);
    const auto high = static_cast<T>(statistics.max);
    const T range = high - low;
    const double levels = static_cast<double>(range) / delta;
    if (levels > max_levels) {
        return std::nullopt;
    }
    const bool any_null = statistics.valid != count;
    double zero = 0.0;
    if (any_null || request.method == quantization::subtractive_dither_2) {
        const double offset = delta * reserved_start;
        zero = static_cast<double>(low) - offset;
    } else if (levels < half_levels) {
        // A multiple of the spacing, so that compressing the values again gives the same codes.
        const double steps = static_cast<double>(low) / delta;
        const double shifted = steps + 0.5;
        // The truncation of CFITSIO's conversion to a 64-bit integer, which gives the same value wherever that integer
        // holds it, and beyond, where C leaves the conversion undefined, a multiple that restores the values.
        const double multiple = std::trunc(shifted);
        zero = multiple * delta;
    } else {
        const T sum = low + high;
        zero = static_cast<double>(sum) / 2.0;
    }
    // A tile whose zero point, or whose values once restored, would leave the range of its type is not quantized: its
    // codes would restore infinities or NaN, as those that CFITSIO writes for it do.
    double largest = std::numeric_limits<double>::max();
    if constexpr (std::is_same_v<T, float>) {
        largest = static_cast<double>(std::numeric_limits<float>::max());
    }
    if (!std::isfinite(zero) || std::abs(static_cast<double>(low)) + delta > largest ||
        std::abs(static_cast<double>(high)) + delta > largest) {
        return std::nullopt;
    }

    const bool dithered = request.method != quantization::no_dither;
    const bool keep_zeros = request.method == quantization::subtractive_dither_2;
    dither_walk walk(request.row, request.seed);
    for (std::size_t i = 0; i < count; ++i) {
        const T pixel = values[i];
        const float random = dithered ? walk.next() : 0.0F;
        if (any_null && std::isnan(pixel)) {
            codes[i] = quantized_null;
            continue;
        }
        if (keep_zeros && pixel == 0) {
            codes[i] = quantized_zero;
            continue;
        }
        const double offset = static_cast<double>(pixel) - zero;
        const double scaled = offset / delta;
        if (!dithered) {
            codes[i] = nearest(scaled);
            continue;
        }
        const double dithered_value = scaled + static_cast<double>(random);
        const double centred = dithered_value - 0.5;
        codes[i] = nearest(centred);
    }
    return quantized_scaling{.scale = delta, .zero = zero};
}

template <typename T> bool prepare(std::span<T> values) noexcept
{
    bool nulls = false;
    for (T& value : values) {
        if (!std::isfinite(value)) {
            value = std::numeric_limits<T>::quiet_NaN();
            nulls = true;
        } else if (std::fpclassify(value) == FP_ZERO || std::fpclassify(value) == FP_SUBNORMAL) {
            value = 0;
        }
    }
    return nulls;
}

template <typename T, typename Bits> std::uint32_t seed_of(std::span<const T> values, Bits null_bits) noexcept
{
    std::uint64_t sum = 0;
    for (const T value : values) {
        Bits bits = std::isnan(value) ? null_bits : std::bit_cast<Bits>(value);
        for (std::size_t i = 0; i < sizeof(Bits); ++i) {
            sum += bits & 0xFFU;
            bits >>= 8U;
        }
    }
    return static_cast<std::uint32_t>(sum % dither_count) + 1;
}

template <typename T>
void dequantize_tile(std::span<const std::int32_t> codes, const dequantize_request& request, std::span<T> values)
{
    const bool dithered = request.method != quantization::no_dither;
    // The integers of zeros: CFITSIO's under its ZBLANK, the standard's under the ZBLANK it recommends, and either
    // without one; the null integer is tested first.
    const bool keeps_zeros = dithered && request.method == quantization::subtractive_dither_2 && request.wide;
    const bool cfitsio_zero = keeps_zeros && request.null != standard_null;
    const bool standard_zero = keeps_zeros && (!request.null || *request.null == standard_null);
    const double scale = request.scaling.scale;
    const double zero = request.scaling.zero;
    dither_walk walk(request.row, request.seed);
    for (std::size_t i = 0; i < codes.size(); ++i) {
        const std::int32_t code = codes[i];
        const float random = dithered ? walk.next() : 0.0F;
        if (request.null && code == *request.null) {
            values[i] = std::numeric_limits<T>::quiet_NaN();
            continue;
        }
        if ((cfitsio_zero && code == quantized_zero) || (standard_zero && code == quantized_null)) {
            values[i] = 0;
            continue;
        }
        double value = 0.0;
        if (dithered) {
            const double undithered = static_cast<double>(code) - static_cast<double>(random);
            const double centred = undithered + 0.5;
            const double scaled = centred * scale;
            value = scaled + zero;
        } else if (scale == 1.0 && zero == 0.0) {
            value = static_cast<double>(code);
        } else {
            const double scaled = static_cast<double>(code) * scale;
            value = scaled + zero;
        }
        if constexpr (std::is_same_v<T, float>) {
            values[i] = static_cast<float>(value);
        } else {
            values[i] = value;
        }
    }
}

} // namespace

noise_estimate estimate_noise(std::span<const float> values, std::size_t nx, std::size_t ny, bool nulls)
{
    if (nulls) {
        return noise_of(values, nx, ny, [](float pixel) { return !std::isnan(pixel); });
    }
    return noise_of(values, nx, ny, [](float) { return true; });
}

noise_estimate estimate_noise(std::span<const double> values, std::size_t nx, std::size_t ny, bool nulls)
{
    if (nulls) {
        return noise_of(values, nx, ny, [](double pixel) { return !std::isnan(pixel); });
    }
    return noise_of(values, nx, ny, [](double) { return true; });
}

noise_estimate estimate_noise(std::span<const std::int32_t> values, std::size_t nx, std::size_t ny,
                              std::optional<std::int32_t> null)
{
    if (null) {
        return noise_of(values, nx, ny, [blank = *null](std::int32_t pixel) { return pixel != blank; });
    }
    return noise_of(values, nx, ny, [](std::int32_t) { return true; });
}

bool prepare_tile(std::span<float> values) noexcept
{
    return prepare(values);
}

bool prepare_tile(std::span<double> values) noexcept
{
    return prepare(values);
}

std::uint32_t dither_seed_of(std::span<const float> values) noexcept
{
    return seed_of(values, float_null_bits);
}

std::uint32_t dither_seed_of(std::span<const double> values) noexcept
{
    return seed_of(values, double_null_bits);
}

std::optional<quantized_scaling> quantize(std::span<const float> values, std::size_t nx, std::size_t ny, bool nulls,
                                          const quantize_request& request, std::span<std::int32_t> codes)
{
    return quantize_tile(values, nx, ny, nulls, request, codes);
}

std::optional<quantized_scaling> quantize(std::span<const double> values, std::size_t nx, std::size_t ny, bool nulls,
                                          const quantize_request& request, std::span<std::int32_t> codes)
{
    return quantize_tile(values, nx, ny, nulls, request, codes);
}

void dequantize(std::span<const std::int32_t> codes, const dequantize_request& request, std::span<float> values)
{
    dequantize_tile(codes, request, values);
}

void dequantize(std::span<const std::int32_t> codes, const dequantize_request& request, std::span<double> values)
{
    dequantize_tile(codes, request, values);
}

} // namespace openfits::detail
