// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/types.h>

#include "benchmarks.h"
#include "checksum/checksum.h"
#include "compress/shuffle.h"
#include "core/endian.h"
#include "data/conversion.h"
#include "data/conversion_kernels.h"
#include "header/record.h"

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace openfits::bench {

namespace {

// The bytes that the kernels work on at once: blocks that the first-level and second-level caches hold, and the staging
// chunk of the reads and writes, which the third-level cache holds.
struct block_size
{
    std::string_view name{};
    std::size_t bytes = 0;
};

constexpr std::array<block_size, 3> blocks{{{.name = "32k", .bytes = std::size_t{32} << 10},
                                            {.name = "256k", .bytes = std::size_t{256} << 10},
                                            {.name = "4m", .bytes = std::size_t{4} << 20}}};

// The bytes of a 16-bit sky, as a file stores it.
std::vector<std::byte> stored_sky(std::size_t bytes)
{
    const std::vector<std::uint16_t> values = unsigned_sky(1024, (bytes / 2 + 1023) / 1024);
    std::vector<std::byte> result(bytes);
    std::memcpy(result.data(), values.data(), bytes);
    return result;
}

const char* kernel_name(detail::simd_kernel kernel)
{
    switch (kernel) {
    case detail::simd_kernel::scalar:
        return "scalar";
    case detail::simd_kernel::baseline:
        return "baseline";
    case detail::simd_kernel::best:
        break;
    }
    return "best";
}

// The byte order of a block of items of item_size bytes reversed in place, as a read reverses it.
void swap_block(benchmark::State& state, std::size_t item_size, detail::simd_kernel kernel, std::size_t bytes)
{
    std::vector<std::byte> data = stored_sky(bytes);
    while (state.KeepRunning()) {
        detail::swap_byte_order(data, item_size, kernel);
        benchmark::DoNotOptimize(data.data());
        benchmark::ClobberMemory();
    }
    count_bytes(state, bytes);
}

// The shuffle of GZIP_2 over a block of values of value_size bytes, or its reverse.
void shuffle_block(benchmark::State& state, std::size_t value_size, bool reverse, detail::simd_kernel kernel,
                   std::size_t bytes)
{
    const std::vector<std::byte> data = stored_sky(bytes);
    std::vector<std::byte> output(bytes);
    while (state.KeepRunning()) {
        if (reverse) {
            detail::unshuffle_bytes(data, output, value_size, kernel);
        } else {
            detail::shuffle_bytes(data, output, value_size, kernel);
        }
        benchmark::DoNotOptimize(output.data());
        benchmark::ClobberMemory();
    }
    count_bytes(state, bytes);
}

// The checksum of a block (standard §4.4.2.7), the sum of its bytes as big-endian 32-bit integers.
void sum_block(benchmark::State& state, detail::simd_kernel kernel, std::size_t bytes)
{
    const std::vector<std::byte> data = stored_sky(bytes);
    while (state.KeepRunning()) {
        benchmark::DoNotOptimize(detail::sum_of(data, detail::sum_round, kernel));
    }
    count_bytes(state, bytes);
}

// The parse of a record of an integer, a real or a string with a comment, as the open of a header parses each of its
// records, into a card with its value.
void parse_record(benchmark::State& state, std::string record)
{
    record.resize(80, ' ');
    std::vector<diagnostic> departures;
    while (state.KeepRunning()) {
        departures.clear();
        card value = detail::parse_record(record, departures);
        benchmark::DoNotOptimize(value);
    }
    state.SetItemsProcessed(state.iterations());
}

detail::read_conversion conversion_of(bitpix from, numeric_type to, const scaling& values)
{
    return detail::make_read_conversion(from, to, values, std::nullopt, blank_policy::nan, 0.0, overflow_policy::refuse,
                                        rounding::truncate, 0);
}

// The conversion of the stored values of a block into values of the application, as a read converts its staging chunk:
// in one pass, the swap within the conversion, or in two, the block swapped and then converted.
template <typename T>
void convert_block(benchmark::State& state, const detail::read_conversion& conversion, bool fused, std::size_t bytes)
{
    const std::vector<std::byte> stored = stored_sky(bytes);
    std::vector<std::byte> staging = stored;
    std::vector<T> values(bytes / detail::stored_size(conversion.from));
    while (state.KeepRunning()) {
        std::memcpy(staging.data(), stored.data(), bytes);
        if (fused) {
            (void)detail::convert_read(conversion, staging.data(), 1, reinterpret_cast<std::byte*>(values.data()),
                                       nullptr, values.size());
        } else {
            detail::convert_big_endian(staging, detail::stored_size(conversion.from));
            (void)detail::kernels::read<false>(conversion, staging.data(), 1,
                                               reinterpret_cast<std::byte*>(values.data()), nullptr, values.size());
        }
        benchmark::DoNotOptimize(values.data());
        benchmark::ClobberMemory();
    }
    count_bytes(state, bytes);
}

} // namespace

void register_kernel_benchmarks()
{
    benchmark::RegisterBenchmark("kernel/parse_record/integer", parse_record,
                                 std::string("K12345  =                12345 / an integer"));
    benchmark::RegisterBenchmark("kernel/parse_record/real", parse_record,
                                 std::string("K12346  =     1763.71428571429 / a real number"));
    benchmark::RegisterBenchmark("kernel/parse_record/string", parse_record,
                                 std::string("K12347  = 'value 12347'        / a string"));
    for (const block_size& block : blocks) {
        for (const std::size_t item_size : {2U, 4U, 8U}) {
            for (const detail::simd_kernel kernel :
                 {detail::simd_kernel::scalar, detail::simd_kernel::baseline, detail::simd_kernel::best}) {
                benchmark::RegisterBenchmark("kernel/swap/" + std::to_string(item_size) + "/" + kernel_name(kernel) +
                                                 "/" + std::string(block.name),
                                             swap_block, item_size, kernel, block.bytes);
            }
        }
        for (const std::size_t value_size : {2U, 4U, 8U}) {
            for (const detail::simd_kernel kernel : {detail::simd_kernel::scalar, detail::simd_kernel::best}) {
                const std::string label =
                    std::to_string(value_size) + "/" + kernel_name(kernel) + "/" + std::string(block.name);
                benchmark::RegisterBenchmark("kernel/shuffle/" + label, shuffle_block, value_size, false, kernel,
                                             block.bytes);
                benchmark::RegisterBenchmark("kernel/unshuffle/" + label, shuffle_block, value_size, true, kernel,
                                             block.bytes);
            }
        }
        for (const detail::simd_kernel kernel : {detail::simd_kernel::scalar, detail::simd_kernel::best}) {
            benchmark::RegisterBenchmark("kernel/checksum/" + std::string(kernel_name(kernel)) + "/" +
                                             std::string(block.name),
                                         sum_block, kernel, block.bytes);
        }
        const detail::read_conversion u16 =
            conversion_of(bitpix::int16, numeric_type::float32, {.scale = 1.0, .zero = 32768.0});
        const detail::read_conversion scaled =
            conversion_of(bitpix::int16, numeric_type::float32, {.scale = 0.01, .zero = 100.0});
        for (const bool fused : {true, false}) {
            const std::string pass = fused ? "/fused/" : "/two_passes/";
            benchmark::RegisterBenchmark("kernel/read/u16_as_float" + pass + std::string(block.name),
                                         convert_block<float>, u16, fused, block.bytes);
            benchmark::RegisterBenchmark("kernel/read/i16_scaled_as_float" + pass + std::string(block.name),
                                         convert_block<float>, scaled, fused, block.bytes);
        }
    }
}

} // namespace openfits::bench
