// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "compress/tile_cache.h"

#include <utility>

namespace openfits::detail {

bool tile_key_less::operator()(const tile_key& left, const tile_key& right) const noexcept
{
    if (left.owner.owner_before(right.owner)) {
        return true;
    }
    if (right.owner.owner_before(left.owner)) {
        return false;
    }
    if (left.hdu != right.hdu) {
        return left.hdu < right.hdu;
    }
    return left.tile < right.tile;
}

decoded_tile tile_cache_access::find(tile_cache& cache, const tile_key& key)
{
    tile_cache::state& state = *cache.state_;
    const std::scoped_lock lock(state.mutex);
    const auto found = state.index.find(key);
    if (found == state.index.end()) {
        state.misses.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    state.hits.fetch_add(1, std::memory_order_relaxed);
    state.tiles.splice(state.tiles.begin(), state.tiles, found->second);
    return found->second->tile;
}

void tile_cache_access::insert(tile_cache& cache, const tile_key& key, decoded_tile tile)
{
    tile_cache::state& state = *cache.state_;
    const std::uint64_t bytes = tile->bytes().size();
    if (bytes > state.capacity) {
        return;
    }
    const std::scoped_lock lock(state.mutex);
    if (state.index.contains(key)) {
        // Another read decoded the same tile meanwhile.
        return;
    }
    // The tiles of readers that are gone are used no more, and leave as the least recently used.
    while (state.size + bytes > state.capacity) {
        const tile_cache::state::entry& last = state.tiles.back();
        state.size -= last.tile->bytes().size();
        state.index.erase(last.key);
        state.tiles.pop_back();
    }
    state.tiles.push_front({.key = key, .tile = std::move(tile)});
    state.index.emplace(key, state.tiles.begin());
    state.size += bytes;
}

} // namespace openfits::detail
