// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "compress/tile_cache.h"

#include <openfits/reader.h>

namespace openfits {

tile_cache::tile_cache(std::uint64_t capacity) : state_(std::make_unique<state>(capacity)) {}

tile_cache::~tile_cache() = default;

std::uint64_t tile_cache::capacity() const noexcept
{
    return state_->capacity;
}

std::uint64_t tile_cache::size() const
{
    const std::scoped_lock lock(state_->mutex);
    return state_->size;
}

std::uint64_t tile_cache::hits() const noexcept
{
    return state_->hits.load(std::memory_order_relaxed);
}

std::uint64_t tile_cache::misses() const noexcept
{
    return state_->misses.load(std::memory_order_relaxed);
}

void tile_cache::clear()
{
    const std::scoped_lock lock(state_->mutex);
    state_->index.clear();
    state_->tiles.clear();
    state_->size = 0;
    state_->hits.store(0, std::memory_order_relaxed);
    state_->misses.store(0, std::memory_order_relaxed);
}

} // namespace openfits
