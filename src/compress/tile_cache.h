// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/reader.h>

#include "core/scratch_buffer.h"

#include <atomic>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>

// The state of a tile_cache, and how the reads use it. A tile is kept under the identity of the reader that decoded it,
// a control block that the reader shares while it exists: the cache holds weak references to it, which keep the block
// itself, so that no later reader takes the identity of one that is gone.

namespace openfits::detail {

/// The stored values of a decoded tile, big-endian, as an uncompressed unit holds them; shared, so that a read keeps
/// using a tile that the cache drops.
using decoded_tile = std::shared_ptr<const scratch_buffer>;

/// A tile of a unit of a reader.
struct tile_key
{
    std::weak_ptr<const void> owner{};
    std::uint64_t hdu = 0;
    std::uint64_t tile = 0;
};

/// The order of the keys: by owner, as std::owner_less orders control blocks, then by unit and by tile.
struct tile_key_less
{
    bool operator()(const tile_key& left, const tile_key& right) const noexcept;
};

/// What the reads do with a cache.
struct tile_cache_access
{
    /// The tile of key, or null; counts a hit or a miss.
    [[nodiscard]] static decoded_tile find(tile_cache& cache, const tile_key& key);

    /// Keeps tile under key, dropping the tiles used least recently to make room; a tile larger than the capacity is
    /// not kept.
    static void insert(tile_cache& cache, const tile_key& key, decoded_tile tile);
};

} // namespace openfits::detail

struct openfits::tile_cache::state
{
    struct entry
    {
        detail::tile_key key{};
        detail::decoded_tile tile{};
    };
    using recency = std::list<entry>;

    explicit state(std::uint64_t bytes) : capacity(bytes) {}

    const std::uint64_t capacity;
    std::mutex mutex{};
    // The tiles, the one used most recently first, and their positions by key.
    recency tiles{};
    std::map<detail::tile_key, recency::iterator, detail::tile_key_less> index{};
    std::uint64_t size = 0;
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> misses{0};
};
