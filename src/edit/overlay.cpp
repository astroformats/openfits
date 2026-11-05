// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "edit/overlay.h"

#include "core/checked_math.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace openfits::detail {

void byte_overlay::write(std::uint64_t offset, std::span<const std::byte> bytes)
{
    if (bytes.empty()) {
        return;
    }
    const auto end = checked_add<std::uint64_t>(offset, bytes.size());
    auto next = stretches_.lower_bound(offset);
    if (next != stretches_.begin()) {
        const auto previous = std::prev(next);
        const std::uint64_t start = previous->first;
        std::vector<std::byte>& held = previous->second;
        const std::uint64_t held_end = start + held.size();
        if (held_end >= end) {
            // Within a stretch, which takes the bytes in place.
            std::ranges::copy(bytes, held.begin() + static_cast<std::ptrdiff_t>(offset - start));
            return;
        }
        if (held_end >= offset) {
            // A stretch that the bytes overlap or continue: it is cut where they start and takes them, so that bytes
            // written in order make one stretch, grown as a vector grows.
            size_ -= held_end - offset;
            // Both are 64-bit on every supported platform: a cast would be a useless cast for GCC where they are one
            // type.
            const std::size_t kept = offset - start;
            held.resize(kept);
            held.insert(held.end(), bytes.begin(), bytes.end());
            size_ += bytes.size();
            // The stretches after it that the bytes cover, or begin to.
            while (next != stretches_.end() && next->first < end) {
                const std::uint64_t next_end = next->first + next->second.size();
                if (next_end <= end) {
                    size_ -= next->second.size();
                    next = stretches_.erase(next);
                    continue;
                }
                held.insert(held.end(), next->second.begin() + static_cast<std::ptrdiff_t>(end - next->first),
                            next->second.end());
                size_ -= end - next->first;
                stretches_.erase(next);
                break;
            }
            return;
        }
    }
    // The stretches that the bytes cover are dropped, and the one that they begin to cover keeps what follows them.
    std::vector<std::byte> tail;
    while (next != stretches_.end() && next->first < end) {
        const std::uint64_t next_end = next->first + next->second.size();
        if (next_end > end) {
            tail.assign(next->second.begin() + static_cast<std::ptrdiff_t>(end - next->first), next->second.end());
            size_ -= end - next->first;
            stretches_.erase(next);
            break;
        }
        size_ -= next->second.size();
        next = stretches_.erase(next);
    }
    std::vector<std::byte> stretch(bytes.begin(), bytes.end());
    stretch.insert(stretch.end(), tail.begin(), tail.end());
    size_ += bytes.size();
    stretches_.emplace(offset, std::move(stretch));
}

void byte_overlay::apply(std::uint64_t offset, std::span<std::byte> buffer) const
{
    if (buffer.empty() || stretches_.empty()) {
        return;
    }
    const std::uint64_t end = offset + buffer.size();
    auto stretch = stretches_.upper_bound(offset);
    if (stretch != stretches_.begin()) {
        --stretch;
    }
    for (; stretch != stretches_.end() && stretch->first < end; ++stretch) {
        const std::uint64_t start = stretch->first;
        const std::uint64_t stretch_end = start + stretch->second.size();
        const std::uint64_t from = std::max(start, offset);
        const std::uint64_t to = std::min(stretch_end, end);
        if (from >= to) {
            continue;
        }
        std::copy_n(stretch->second.begin() + static_cast<std::ptrdiff_t>(from - start),
                    static_cast<std::ptrdiff_t>(to - from),
                    buffer.begin() + static_cast<std::ptrdiff_t>(from - offset));
    }
}

std::uint64_t byte_overlay::end() const noexcept
{
    if (stretches_.empty()) {
        return 0;
    }
    const auto& last = *stretches_.rbegin();
    return last.first + last.second.size();
}

} // namespace openfits::detail
