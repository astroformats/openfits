// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "compress/tile_cache.h"

#include <openfits/reader.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

using openfits::tile_cache;
using openfits::detail::decoded_tile;
using openfits::detail::scratch_buffer;
using openfits::detail::tile_cache_access;
using openfits::detail::tile_key;

decoded_tile tile_of(std::size_t size)
{
    return std::make_shared<const scratch_buffer>(size);
}

TEST(tile_cache, finds_the_tiles_it_keeps)
{
    tile_cache cache(1000);
    const auto owner = std::make_shared<const int>(0);
    EXPECT_EQ(cache.capacity(), 1000U);
    EXPECT_EQ(tile_cache_access::find(cache, {.owner = owner, .hdu = 1, .tile = 0}), nullptr);
    const decoded_tile tile = tile_of(100);
    tile_cache_access::insert(cache, {.owner = owner, .hdu = 1, .tile = 0}, tile);
    EXPECT_EQ(tile_cache_access::find(cache, {.owner = owner, .hdu = 1, .tile = 0}), tile);
    // Another tile, unit or reader is another key.
    EXPECT_EQ(tile_cache_access::find(cache, {.owner = owner, .hdu = 1, .tile = 1}), nullptr);
    EXPECT_EQ(tile_cache_access::find(cache, {.owner = owner, .hdu = 2, .tile = 0}), nullptr);
    const auto other = std::make_shared<const int>(0);
    EXPECT_EQ(tile_cache_access::find(cache, {.owner = other, .hdu = 1, .tile = 0}), nullptr);
    EXPECT_EQ(cache.size(), 100U);
    EXPECT_EQ(cache.hits(), 1U);
    EXPECT_EQ(cache.misses(), 4U);
    cache.clear();
    EXPECT_EQ(cache.size(), 0U);
    EXPECT_EQ(cache.hits(), 0U);
    EXPECT_EQ(cache.misses(), 0U);
    EXPECT_EQ(tile_cache_access::find(cache, {.owner = owner, .hdu = 1, .tile = 0}), nullptr);
}

TEST(tile_cache, drops_the_tiles_used_least_recently)
{
    tile_cache cache(300);
    const auto owner = std::make_shared<const int>(0);
    const auto key = [&owner](std::uint64_t tile) { return tile_key{.owner = owner, .hdu = 0, .tile = tile}; };
    for (std::uint64_t i = 0; i < 3; ++i) {
        tile_cache_access::insert(cache, key(i), tile_of(100));
    }
    // Tile 0 used, so that tile 1 is the least recent when tile 3 needs room.
    EXPECT_NE(tile_cache_access::find(cache, key(0)), nullptr);
    tile_cache_access::insert(cache, key(3), tile_of(100));
    EXPECT_EQ(cache.size(), 300U);
    EXPECT_NE(tile_cache_access::find(cache, key(0)), nullptr);
    EXPECT_EQ(tile_cache_access::find(cache, key(1)), nullptr);
    EXPECT_NE(tile_cache_access::find(cache, key(2)), nullptr);
    EXPECT_NE(tile_cache_access::find(cache, key(3)), nullptr);
    // A tile of twice the room drops two.
    tile_cache_access::insert(cache, key(4), tile_of(200));
    EXPECT_EQ(cache.size(), 300U);
    EXPECT_EQ(tile_cache_access::find(cache, key(0)), nullptr);
    EXPECT_EQ(tile_cache_access::find(cache, key(2)), nullptr);
}

TEST(tile_cache, keeps_no_tile_larger_than_its_capacity)
{
    tile_cache none(0);
    const auto owner = std::make_shared<const int>(0);
    tile_cache_access::insert(none, {.owner = owner}, tile_of(1));
    EXPECT_EQ(none.size(), 0U);
    tile_cache small(10);
    tile_cache_access::insert(small, {.owner = owner}, tile_of(11));
    EXPECT_EQ(tile_cache_access::find(small, {.owner = owner}), nullptr);
    tile_cache_access::insert(small, {.owner = owner}, tile_of(10));
    EXPECT_NE(tile_cache_access::find(small, {.owner = owner}), nullptr);
}

TEST(tile_cache, a_reader_that_is_gone_shares_its_tiles_with_none)
{
    tile_cache cache(1000);
    auto owner = std::make_shared<const int>(0);
    tile_cache_access::insert(cache, {.owner = owner, .hdu = 0, .tile = 0}, tile_of(100));
    owner.reset();
    // New owners, whatever their addresses, are other readers.
    std::vector<std::shared_ptr<const int>> owners;
    for (int i = 0; i < 100; ++i) {
        owners.push_back(std::make_shared<const int>(i));
        EXPECT_EQ(tile_cache_access::find(cache, {.owner = owners.back(), .hdu = 0, .tile = 0}), nullptr) << i;
    }
}

} // namespace
