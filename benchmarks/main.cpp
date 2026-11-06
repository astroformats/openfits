// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "benchmarks.h"

#include <benchmark/benchmark.h>

#include <cmath>
#include <cstdint>

namespace openfits::bench {

namespace {

// The value of the background at (x, y), plus noise from a linear congruential generator.
double sky_value(std::size_t x, std::size_t y, std::uint32_t& state)
{
    state = (state * 1'103'515'245U) + 12'345U;
    const double noise = static_cast<double>((state >> 8U) & 0xFFFFU) / 65536.0;
    return 0.1 + (0.05 * std::sin(static_cast<double>(x) / 200.0) * std::cos(static_cast<double>(y) / 150.0)) +
           (0.001 * noise);
}

} // namespace

std::vector<float> sky(std::size_t width, std::size_t height)
{
    std::vector<float> values(width * height);
    std::uint32_t state = 1;
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            values[(y * width) + x] = static_cast<float>(sky_value(x, y, state));
        }
    }
    return values;
}

std::vector<std::uint16_t> unsigned_sky(std::size_t width, std::size_t height)
{
    const std::vector<float> values = sky(width, height);
    std::vector<std::uint16_t> result(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        result[i] = static_cast<std::uint16_t>(std::lround(values[i] * 65535.0F));
    }
    return result;
}

} // namespace openfits::bench

int main(int argc, char** argv)
{
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
        return 1;
    }
    openfits::bench::register_image_benchmarks();
    openfits::bench::register_table_benchmarks();
    openfits::bench::register_compression_benchmarks();
    openfits::bench::register_codec_benchmarks();
    openfits::bench::register_edit_benchmarks();
    openfits::bench::register_kernel_benchmarks();
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    openfits::bench::remove_benchmark_files();
    return 0;
}
