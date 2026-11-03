// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The tiles of one compressed image read by many threads of one reader (standard §10.1), with a cache that they share
// and without one: the const functions of a reader run concurrently, and a tile_cache is shared under its lock. Under
// ThreadSanitizer, the workload of the contract of thread safety; with oneTBB, each read decodes in parallel too.

#include <openfits/compression.h>
#include <openfits/image.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/writer.h>

#include "support/threads.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <ostream>
#include <span>
#include <vector>

namespace {

using openfits::compression;
using openfits::compression_options;
using openfits::reader;
using openfits::region;

constexpr std::uint64_t width = 300;
constexpr std::uint64_t height = 200;

std::vector<std::byte> compressed_file(const std::vector<std::int32_t>& values, compression type)
{
    openfits::image_info info;
    info.geometry.axes = {width, height};
    openfits::writer output;
    output.add_image(info, std::span<const std::int32_t>(values),
                     {.compression = compression_options{.type = type, .tile = {64, 16}}});
    openfits::memory_sink sink;
    output.save(sink);
    return sink.release();
}

// An algorithm and its name, which CTest names the test after.
struct algorithm
{
    compression type = compression::rice;
    const char* name = "";
};

// NOLINTNEXTLINE(readability-identifier-naming)
void PrintTo(const algorithm& value, std::ostream* output)
{
    *output << value.name;
}

class concurrency_tiles : public testing::TestWithParam<algorithm>
{};

TEST_P(concurrency_tiles, threads_read_the_tiles_of_one_reader)
{
    std::vector<std::int32_t> values(width * height);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = static_cast<std::int32_t>((i * 7919) % 100'000);
    }
    const reader input(std::make_unique<openfits::memory_source>(compressed_file(values, GetParam().type)));
    openfits::tile_cache cache(std::uint64_t{1} << 20);
    constexpr std::size_t threads = 16;
    std::vector<int> failures(threads, 0);
    openfits::test::run_threads(threads, [&](std::size_t t) {
        // Each thread reads every row of its band and the whole image, through the cache or not.
        openfits::tile_cache* shared = t % 2 == 0 ? &cache : nullptr;
        std::vector<std::int32_t> row(width);
        for (std::uint64_t y = t; y < height; y += 3) {
            input.read_pixels(1, std::span<std::int32_t>(row),
                              {.region = region{.start = {0, y}, .size = {width, 1}}, .cache = shared});
            for (std::uint64_t x = 0; x < width; ++x) {
                failures[t] += row[x] == values[(y * width) + x] ? 0 : 1;
            }
        }
        const std::vector<std::int32_t> whole = input.read_pixels<std::int32_t>(1, {.cache = shared});
        failures[t] += whole == values ? 0 : 1;
    });
    for (std::size_t t = 0; t < threads; ++t) {
        EXPECT_EQ(failures[t], 0) << t;
    }
    // 5 x 13 tiles, each decoded at least once by the threads of the cache, which then found it.
    EXPECT_GE(cache.misses(), 65U);
    EXPECT_GT(cache.hits(), 0U);
}

INSTANTIATE_TEST_SUITE_P(algorithms, concurrency_tiles,
                         testing::Values(algorithm{.type = compression::rice, .name = "rice"},
                                         algorithm{.type = compression::gzip_2, .name = "gzip2"},
                                         algorithm{.type = compression::hcompress, .name = "hcompress"}),
                         [](const testing::TestParamInfo<algorithm>& instance) { return instance.param.name; });

} // namespace
