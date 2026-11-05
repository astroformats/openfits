// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/editor.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/writer.h>

#include "support/threads.h"
#include "support/written_file.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

// An editor is thread-compatible: its file can be read from any number of threads at once between commits, the const
// member functions of its reader running together, and a commit runs alone.

namespace {

TEST(concurrency_editor, threads_read_the_file_of_an_editor_between_commits)
{
    std::vector<std::uint16_t> values(std::size_t{64} * 64);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = static_cast<std::uint16_t>(i);
    }
    openfits::image_info info;
    info.geometry.axes = {64, 64};
    openfits::writer output;
    (void)output.add_image(info, std::span<const std::uint16_t>(values));
    (void)output.add_image(info, std::span<const std::uint16_t>(values));
    openfits::editor file(std::make_unique<openfits::memory_device>(openfits::test::written(output)));
    for (std::uint16_t round = 0; round < 4; ++round) {
        std::atomic<int> mismatches{0};
        const openfits::reader& current = file.file();
        openfits::test::run_threads(16, [&](std::size_t t) {
            const std::size_t unit = t % 2;
            const std::vector<std::uint16_t> read = current.read_pixels<std::uint16_t>(
                unit, {.region = openfits::region{.start = {0, t}, .size = {64, 1}}});
            for (std::size_t x = 0; x < read.size(); ++x) {
                const std::uint16_t expected =
                    x == 0 && t == 0 && round > 0 ? static_cast<std::uint16_t>(9000 + round) : values[(t * 64) + x];
                if (read[x] != expected) {
                    ++mismatches;
                }
            }
        });
        EXPECT_EQ(mismatches.load(), 0) << "round " << round;
        // A commit alone, between the reads.
        const std::vector<std::uint16_t> first{static_cast<std::uint16_t>(9001 + round)};
        for (std::size_t unit = 0; unit < 2; ++unit) {
            file.write_pixels(unit, std::span<const std::uint16_t>(first),
                              {.region = openfits::region{.start = {0, 0}, .size = {1, 1}}});
        }
        file.commit();
    }
}

} // namespace
