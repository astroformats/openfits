// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// A gzip-compressed file read by 16 threads at once, its content inflated into memory and into a temporary file, whose
// reads run in parallel. ThreadSanitizer runs these tests in CI.

#include <openfits/io.h>
#include <openfits/reader.h>

#include "support/compressed_fixtures.h"
#include "support/fixture_builder.h"
#include "support/temp_directory.h"
#include "support/threads.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <vector>

namespace {

using openfits::reader;
using openfits::region;

constexpr std::size_t thread_count = 16;
constexpr std::uint64_t width = 500;
constexpr std::uint64_t height = 400;

std::int16_t value_at(std::uint64_t x, std::uint64_t y)
{
    return static_cast<std::int16_t>(((x * 7) + (y * 13)) % 30'000);
}

// Each thread reads rows of its own, again and again, and counts the wrong ones and the failures.
int read_from_threads(const reader& file)
{
    std::atomic<int> wrong{0};
    openfits::test::run_threads(thread_count, [&file, &wrong](std::size_t t) {
        for (std::uint64_t y = t; y < height; y += thread_count) {
            try {
                const std::vector<std::int16_t> row =
                    file.read_pixels<std::int16_t>(0, {.region = region{.start = {0, y}, .size = {width, 1}}});
                for (std::uint64_t x = 0; x < width; ++x) {
                    if (row[x] != value_at(x, y)) {
                        ++wrong;
                        break;
                    }
                }
            } catch (const std::exception&) {
                ++wrong;
            }
        }
    });
    return wrong;
}

TEST(concurrency_gzip, threads_read_a_file_inflated_into_memory_and_into_a_temporary_file)
{
    std::vector<std::int16_t> values;
    for (std::uint64_t y = 0; y < height; ++y) {
        for (std::uint64_t x = 0; x < width; ++x) {
            values.push_back(value_at(x, y));
        }
    }
    const openfits::test::fixture file = openfits::test::fixture()
                                             .header(openfits::test::primary_records(16, {width, height}))
                                             .data(openfits::test::big_endian(values));
    const std::vector<std::byte> stream = openfits::test::gzip(file.bytes());
    const openfits::test::temp_directory directory;

    const reader in_memory(
        std::make_unique<openfits::memory_source>(std::vector<std::byte>(stream.begin(), stream.end())));
    const reader in_a_file(
        std::make_unique<openfits::memory_source>(std::vector<std::byte>(stream.begin(), stream.end())),
        {.inflate = {.temporary_directory = openfits::test::utf8(directory.path()), .memory_threshold = 0}});

    EXPECT_EQ(read_from_threads(in_memory), 0);
    EXPECT_EQ(read_from_threads(in_a_file), 0);
}

} // namespace
