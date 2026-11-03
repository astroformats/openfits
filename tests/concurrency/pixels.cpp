// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/checksum.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/types.h>

#include "support/checksums.h"
#include "support/counting_source.h"
#include "support/fixture_builder.h"
#include "support/threads.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace {

using openfits::reader;
using openfits::region;
using openfits::test::big_endian;
using openfits::test::card_record;
using openfits::test::counting_source;
using openfits::test::fixture;
using openfits::test::primary_records;
using openfits::test::run_threads;
using openfits::test::with;

constexpr std::size_t thread_count = 16;
constexpr std::uint64_t width = 300;
constexpr std::uint64_t height = 200;

// An image of the unsigned convention, value i of the data i mod 65536, and its extensions not yet walked.
fixture image_file()
{
    std::vector<std::int16_t> stored(width * height);
    for (std::size_t i = 0; i < stored.size(); ++i) {
        stored[i] = static_cast<std::int16_t>(static_cast<int>(i % 65536) - 32768);
    }
    fixture file = fixture()
                       .header(with(primary_records(16, {width, height}), {card_record("BZERO", "32768")}))
                       .data(big_endian(stored));
    for (std::size_t t = 0; t < thread_count; ++t) {
        file.image({64, 64});
    }
    return file;
}

// Threads read regions and whole images of one reader at once, from memory, through a source that serializes its
// reads, and through one that does not; those that read extensions walk the file meanwhile.
TEST(concurrency_pixels, threads_read_the_values_of_one_reader)
{
    const fixture file = image_file();
    for (const int kind : {0, 1, 2}) {
        std::unique_ptr<openfits::input_source> source;
        if (kind == 0) {
            source = std::make_unique<openfits::memory_source>(std::vector<std::byte>(file.bytes()));
        } else {
            source = std::make_unique<counting_source>(file.bytes(), kind == 2);
        }
        const reader input(std::move(source));
        std::vector<std::vector<float>> regions(thread_count);
        std::vector<std::vector<std::uint16_t>> wholes(thread_count);
        std::vector<std::size_t> extension_values(thread_count);
        run_threads(thread_count, [&](std::size_t t) {
            const region box{.start = {t * 10, t * 5}, .size = {40, 30}, .step = {1, t % 2 == 0 ? 1 : -1}};
            regions[t] = input.read_pixels<float>(0, {.region = box});
            wholes[t] = input.read_pixels<std::uint16_t>(0);
            extension_values[t] = input.read_pixels<std::uint8_t>(1 + t).size();
        });
        for (std::size_t t = 0; t < thread_count; ++t) {
            ASSERT_EQ(regions[t].size(), 40U * 30U);
            for (std::uint64_t row = 0; row < 30; ++row) {
                const std::uint64_t y = t % 2 == 0 ? (t * 5) + row : (t * 5) + 29 - row;
                for (std::uint64_t x = 0; x < 40; ++x) {
                    const std::uint64_t index = (y * width) + (t * 10) + x;
                    EXPECT_EQ(regions[t][(row * 40) + x], static_cast<float>(index % 65536)) << t;
                }
            }
            ASSERT_EQ(wholes[t].size(), width * height);
            EXPECT_EQ(wholes[t][12345], 12345U);
            EXPECT_EQ(extension_values[t], 64U * 64U);
        }
    }
}

// Threads verify the units of one reader and read its image whole, which verifies DATASUM as it is read, at once.
TEST(concurrency_pixels, threads_verify_the_checksums_of_one_reader)
{
    std::vector<std::int16_t> stored(width * height);
    for (std::size_t i = 0; i < stored.size(); ++i) {
        stored[i] = static_cast<std::int16_t>(i % 30000);
    }
    std::vector<std::byte> file =
        openfits::test::checked_unit(primary_records(16, {width, height}), big_endian(stored));
    constexpr std::size_t units = 5;
    for (std::size_t u = 1; u < units; ++u) {
        const std::vector<std::byte> unit =
            openfits::test::checked_unit(openfits::test::extension_records("IMAGE", 8, {64, 64}),
                                         std::vector<std::byte>(std::size_t{64} * 64, std::byte{7}));
        file.insert(file.end(), unit.begin(), unit.end());
    }
    for (const int kind : {0, 1, 2}) {
        std::unique_ptr<openfits::input_source> source;
        if (kind == 0) {
            // The file outlives the source, which borrows it.
            source = std::make_unique<openfits::memory_source>(std::span<const std::byte>(file));
        } else {
            source = std::make_unique<counting_source>(file, kind == 2);
        }
        const reader input(std::move(source));
        std::vector<openfits::checksum_report> reports(thread_count);
        std::vector<std::size_t> sizes(thread_count);
        run_threads(thread_count, [&](std::size_t t) {
            reports[t] = input.verify(t % units);
            sizes[t] = input.read_pixels<std::int16_t>(0).size();
        });
        for (std::size_t t = 0; t < thread_count; ++t) {
            EXPECT_EQ(reports[t].datasum, openfits::checksum_state::valid) << t;
            EXPECT_EQ(reports[t].checksum, openfits::checksum_state::valid) << t;
            EXPECT_EQ(sizes[t], width * height) << t;
        }
    }
}

} // namespace
