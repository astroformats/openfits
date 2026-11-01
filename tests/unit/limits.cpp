// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/limits.h>

#include <gtest/gtest.h>

#include <cstdint>

namespace {

constexpr std::uint64_t mib = std::uint64_t{1} << 20;
constexpr std::uint64_t gib = std::uint64_t{1} << 30;

// The defaults are documented, and applications rely on them.
TEST(limits, defaults)
{
    const openfits::limits defaults;

    EXPECT_EQ(defaults.max_hdus, 100'000U);
    EXPECT_EQ(defaults.max_header_bytes, 128 * mib);
    EXPECT_EQ(defaults.max_total_header_bytes, gib);
    EXPECT_EQ(defaults.max_allocation, 16 * gib);
    EXPECT_EQ(defaults.max_heap_bytes, 1024 * gib);
    EXPECT_EQ(defaults.max_tile_bytes, 4 * gib);
    EXPECT_EQ(defaults.max_tiles, std::uint64_t{1} << 26);
    EXPECT_EQ(defaults.max_vla_elements, std::uint64_t{1} << 32);
    EXPECT_EQ(defaults.max_inflated_bytes, 16 * gib);
    EXPECT_EQ(defaults.max_diagnostics, 10'000U);
}

// A header of a million cards is within the defaults.
TEST(limits, a_header_of_a_million_cards_is_within_the_defaults)
{
    const openfits::limits defaults;
    constexpr std::uint64_t block = 2880;
    constexpr std::uint64_t cards = 1'000'000;
    constexpr std::uint64_t bytes = ((cards + 1) * 80 + block - 1) / block * block;

    EXPECT_LE(bytes, defaults.max_header_bytes);
    EXPECT_LE(defaults.max_header_bytes, defaults.max_total_header_bytes);
}

} // namespace
