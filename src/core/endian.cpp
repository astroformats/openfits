// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "core/endian.h"

#include <cstdint>

namespace openfits::detail {

namespace {

template <std::unsigned_integral T> void swap_items(std::span<const std::byte> from, std::span<std::byte> to) noexcept
{
    const std::size_t count = from.size() / sizeof(T);
    for (std::size_t i = 0; i < count; ++i) {
        T value = 0;
        std::memcpy(&value, from.data() + (i * sizeof(T)), sizeof(T));
        value = byte_swap(value);
        std::memcpy(to.data() + (i * sizeof(T)), &value, sizeof(T));
    }
}

// Swaps the items of from into to, which may be the same memory: each item is read whole before it is written. Returns
// false for an item size that has no byte order to swap.
bool swap_into(std::span<const std::byte> from, std::span<std::byte> to, std::size_t item_size) noexcept
{
    switch (item_size) {
    case 2:
        swap_items<std::uint16_t>(from, to);
        return true;
    case 4:
        swap_items<std::uint32_t>(from, to);
        return true;
    case 8:
        swap_items<std::uint64_t>(from, to);
        return true;
    default:
        return false;
    }
}

} // namespace

void swap_byte_order(std::span<std::byte> data, std::size_t item_size) noexcept
{
    (void)swap_into(data, data, item_size);
}

void convert_big_endian(std::span<std::byte> data, std::size_t item_size) noexcept
{
    if constexpr (std::endian::native == std::endian::little) {
        (void)swap_into(data, data, item_size);
    }
}

void copy_big_endian(std::span<const std::byte> from, std::span<std::byte> to, std::size_t item_size) noexcept
{
    if (from.empty()) {
        return;
    }
    std::size_t copied = 0;
    if constexpr (std::endian::native == std::endian::little) {
        if (swap_into(from, to, item_size)) {
            copied = from.size() - (from.size() % item_size);
        }
    }
    // Everything on a big-endian host, and the bytes after the last complete item on a little-endian one.
    std::memcpy(to.data() + copied, from.data() + copied, from.size() - copied);
}

} // namespace openfits::detail
