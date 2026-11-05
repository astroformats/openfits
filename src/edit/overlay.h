// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

// The bytes that the editor writes into the data of a unit before a commit writes them into the file.

namespace openfits::detail {

/// Bytes written at offsets, each write over those before it: the latest bytes of every offset written, in stretches
/// that do not overlap. A commit lays them over the bytes of the file, which they replace, and writes each stretch
/// once, whatever the writes that made it.
class byte_overlay
{
public:
    /// Writes bytes at offset, over those written there before.
    void write(std::uint64_t offset, std::span<const std::byte> bytes);

    /// Lays the bytes written over buffer, which holds the bytes from offset on.
    void apply(std::uint64_t offset, std::span<std::byte> buffer) const;

    /// The stretches of bytes written, by offset, in increasing order and without overlaps.
    [[nodiscard]] const std::map<std::uint64_t, std::vector<std::byte>>& stretches() const noexcept
    {
        return stretches_;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return stretches_.empty();
    }

    /// The offset after the last byte written, 0 when nothing is.
    [[nodiscard]] std::uint64_t end() const noexcept;

    /// The bytes written, every stretch counted once.
    [[nodiscard]] std::uint64_t size() const noexcept
    {
        return size_;
    }

private:
    std::map<std::uint64_t, std::vector<std::byte>> stretches_{};
    std::uint64_t size_ = 0;
};

} // namespace openfits::detail
