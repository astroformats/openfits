// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/reader.h>

#include "support/counting_source.h"
#include "support/fixture_builder.h"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace {

using openfits::hdu_info;
using openfits::reader;
using openfits::test::card_record;
using openfits::test::counting_source;
using openfits::test::fixture;

constexpr std::size_t thread_count = 16;

fixture many_units()
{
    fixture file;
    file.primary();
    for (std::size_t i = 1; i <= 4 * thread_count; ++i) {
        file.image({100 * i}, {card_record("EXTNAME", "'UNIT" + std::to_string(i) + "'")});
    }
    return file;
}

// Threads that ask for different units of a file that no one walked yet walk each unit once, and all see the same
// units at the same addresses.
TEST(concurrency_walk, threads_walk_a_file_together)
{
    const fixture file = many_units();
    for (const bool concurrent : {false, true}) {
        auto source = std::make_unique<counting_source>(file.bytes(), concurrent);
        const counting_source& counter = *source;
        const reader input(std::move(source));
        std::vector<const hdu_info*> seen(thread_count * 3);
        std::atomic<bool> start{false};
        std::vector<std::thread> threads;
        threads.reserve(thread_count);
        for (std::size_t t = 0; t < thread_count; ++t) {
            threads.emplace_back([&, t] {
                while (!start.load()) {
                    std::this_thread::yield();
                }
                // Each thread asks for units from the end backwards, by index and by name, and counts them.
                const std::size_t index = (4 * thread_count) - t;
                seen[t * 3] = &input.hdu(index);
                seen[(t * 3) + 1] = &input.hdu("UNIT" + std::to_string(index - 1));
                seen[(t * 3) + 2] = &input.hdus().back().get();
            });
        }
        start = true;
        for (std::thread& thread : threads) {
            thread.join();
        }
        for (std::size_t t = 0; t < thread_count; ++t) {
            const std::size_t index = (4 * thread_count) - t;
            EXPECT_EQ(seen[t * 3], &input.hdu(index));
            EXPECT_EQ(seen[t * 3]->data_size(), 100U * index);
            EXPECT_EQ(seen[(t * 3) + 1], &input.hdu(index - 1));
            EXPECT_EQ(seen[(t * 3) + 2], &input.hdu(4 * thread_count));
        }
        // Each header was read once.
        EXPECT_EQ(counter.reads().size(), 4 * thread_count + 1) << concurrent;
    }
}

} // namespace
