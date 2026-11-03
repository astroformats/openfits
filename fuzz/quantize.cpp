// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The quantization of floating-point tiles (standard §10.2) on untrusted bytes: codes taken from the input dequantized
// under a hostile ZSCALE, ZZERO, ZBLANK, method and seed, which must give values or NaN without failing; and values
// taken from the input, as floats and as doubles, quantized at a level that the input chooses, whose dequantized
// values must lie within the spacing of the originals, NaN for NaN. A failure must be an error of the library's
// kinds; any other exception escapes and fails the run.

#include "compress/quantize.h"

#include <openfits/compression.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace {

void require(bool condition)
{
    if (!condition) {
        std::abort();
    }
}

template <typename T> T value_at(std::span<const std::byte> input, std::size_t offset) noexcept
{
    T value{};
    if (offset + sizeof(T) <= input.size()) {
        std::memcpy(&value, input.data() + offset, sizeof(T));
    }
    return value;
}

openfits::quantization method_of(std::byte selector) noexcept
{
    switch (std::to_integer<unsigned>(selector) % 3U) {
    case 0:
        return openfits::quantization::no_dither;
    case 1:
        return openfits::quantization::subtractive_dither_1;
    default:
        return openfits::quantization::subtractive_dither_2;
    }
}

template <typename T> void quantize_values(std::span<const std::byte> input, openfits::quantization method)
{
    std::vector<T> values(input.size() / sizeof(T));
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = value_at<T>(input, i * sizeof(T));
    }
    const bool nulls = openfits::detail::prepare_tile(std::span<T>(values));
    const std::size_t nx = values.size() < 9 ? values.size() : 9;
    const std::size_t ny = nx == 0 ? 0 : values.size() / nx;
    values.resize(nx * ny);
    std::vector<std::int32_t> codes(values.size());
    const double level = input.empty() ? 4.0 : (std::to_integer<int>(input[0]) % 16) - 8.5;
    const std::uint32_t seed = openfits::detail::dither_seed_of(std::span<const T>(values));
    require(seed >= 1 && seed <= 10'000);
    const std::optional<openfits::detail::quantized_scaling> scaling = openfits::detail::quantize(
        std::span<const T>(values), nx, ny, nulls, {.level = level, .method = method, .row = 3, .seed = seed}, codes);
    if (!scaling) {
        return;
    }
    std::vector<T> back(values.size());
    openfits::detail::dequantize(
        codes,
        {.scaling = *scaling, .method = method, .row = 3, .seed = seed, .null = openfits::detail::quantized_null},
        std::span<T>(back));
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (std::isnan(values[i])) {
            require(std::isnan(back[i]));
        } else if (method == openfits::quantization::subtractive_dither_2 && values[i] == 0) {
            require(back[i] == 0);
        } else {
            // Within the spacing, and the rounding of the type of the values.
            const double error = std::abs(static_cast<double>(back[i]) - static_cast<double>(values[i]));
            require(error <= (scaling->scale * 1.000001) + (std::abs(static_cast<double>(values[i])) * 1e-6));
        }
    }
}

} // namespace

// The entry point that libFuzzer, and the replay driver, call for every input. Its name is fixed by libFuzzer.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::span<const std::byte> input(reinterpret_cast<const std::byte*>(data), size);
    if (size < 24) {
        return 0;
    }
    // Codes under a hostile scaling.
    const openfits::quantization method = method_of(input[0]);
    const auto scale = value_at<double>(input, 1);
    const auto zero = value_at<double>(input, 9);
    const auto blank = value_at<std::int32_t>(input, 17);
    const auto seed = 1 + (value_at<std::uint32_t>(input, 21) % 10'000);
    std::vector<std::int32_t> codes((size - 24) / 4);
    for (std::size_t i = 0; i < codes.size(); ++i) {
        codes[i] = value_at<std::int32_t>(input, 24 + (i * 4));
    }
    std::vector<float> floats(codes.size());
    openfits::detail::dequantize(
        codes,
        {.scaling = {.scale = scale, .zero = zero},
         .method = method,
         .row = 1 + size,
         .seed = seed,
         .null = (std::to_integer<unsigned>(input[0]) & 8U) != 0 ? std::optional<std::int32_t>(blank) : std::nullopt,
         .wide = (std::to_integer<unsigned>(input[0]) & 16U) != 0},
        std::span<float>(floats));

    // Values of the input quantized and restored.
    quantize_values<float>(input.subspan(24), method);
    quantize_values<double>(input.subspan(24), method);
    return 0;
}
