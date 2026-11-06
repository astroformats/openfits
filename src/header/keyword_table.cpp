// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "header/keyword_table.h"

#include "header/keywords.h"

#include <algorithm>
#include <bit>
#include <utility>

namespace openfits::detail {

namespace {

// The fewest slots of an index.
constexpr std::size_t least_slots = 64;

// The key of a keyword: its prefix and its size, as a slot holds them, and its hash.
struct key
{
    std::uint64_t prefix = 0;
    std::uint8_t size = 0;
    std::uint64_t hash = 0;
};

// A multiplicative hash of the prefix and the size, the bytes of a longer keyword after its prefix folded in, its high
// bits folded back into the low ones that pick a slot.
key key_of(std::string_view keyword) noexcept
{
    key result{.prefix = keyword_prefix(keyword),
               .size = static_cast<std::uint8_t>(std::min<std::size_t>(keyword.size(), 9))};
    std::uint64_t hash = (result.prefix * 0x9E3779B97F4A7C15ULL) + result.size;
    for (std::size_t i = 8; i < keyword.size(); ++i) {
        hash = (hash ^ static_cast<unsigned char>(keyword[i])) * 0x100000001B3ULL;
    }
    hash ^= hash >> 32U;
    hash *= 0xD6E8FEB86659FD93ULL;
    hash ^= hash >> 32U;
    result.hash = hash;
    return result;
}

} // namespace

keyword_table::keyword_table(std::size_t cards) : slots(std::bit_ceil(std::max(least_slots, cards * 2))) {}

std::size_t keyword_table::probe(std::uint64_t prefix, std::uint8_t size, std::uint64_t hash, std::string_view keyword,
                                 std::span<const card> cards) const noexcept
{
    // A table at most half full always has a vacant slot, where a keyword that it does not hold ends the search.
    const std::size_t mask = slots.size() - 1;
    for (std::size_t i = hash & mask;; i = (i + 1) & mask) {
        const slot& candidate = slots[i];
        if (candidate.position == vacant) {
            return i;
        }
        if (candidate.prefix == prefix && candidate.size == size &&
            (size <= 8 || cards[candidate.position].keyword() == keyword)) {
            return i;
        }
    }
}

std::size_t keyword_table::find(std::string_view keyword, std::span<const card> cards) const noexcept
{
    const key wanted = key_of(keyword);
    const std::uint32_t position = slots[probe(wanted.prefix, wanted.size, wanted.hash, keyword, cards)].position;
    return position == vacant ? none : position;
}

keyword_table::slot* keyword_table::slot_of(std::string_view keyword, std::span<const card> cards) noexcept
{
    const key wanted = key_of(keyword);
    slot& found = slots[probe(wanted.prefix, wanted.size, wanted.hash, keyword, cards)];
    return found.position == vacant ? nullptr : &found;
}

void keyword_table::add(std::string_view keyword, std::size_t position, bool valued, std::span<const card> cards)
{
    if ((used + 1) * 2 > slots.size()) {
        grow(cards);
    }
    const key wanted = key_of(keyword);
    slot& found = slots[probe(wanted.prefix, wanted.size, wanted.hash, keyword, cards)];
    // A header that indexes() takes holds positions within 32 bits.
    const auto held = static_cast<std::uint32_t>(position);
    if (found.position == vacant) {
        found = {.prefix = wanted.prefix, .position = held, .size = wanted.size, .valued = valued};
        ++used;
        return;
    }
    found.position = std::min(found.position, held);
    if (valued) {
        if (found.valued) {
            repeated.push_back(position);
        }
        found.valued = true;
    }
}

void keyword_table::grow(std::span<const card> cards)
{
    std::vector<slot> old = std::move(slots);
    slots.assign(old.size() * 2, slot{});
    for (const slot& entry : old) {
        if (entry.position == vacant) {
            continue;
        }
        const std::string_view keyword = cards[entry.position].keyword();
        const key moved = key_of(keyword);
        slots[probe(moved.prefix, moved.size, moved.hash, keyword, cards)] = entry;
    }
}

std::vector<std::size_t> header_access::repeated_keywords(const header& cards)
{
    if (cards.index_) {
        return cards.index_->repeated;
    }
    // A header short enough to be searched in order.
    std::vector<std::size_t> repeated;
    for (std::size_t i = 0; i < cards.size(); ++i) {
        if (!cards[i].has_value_indicator()) {
            continue;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (cards[j].has_value_indicator() && cards[j].keyword() == cards[i].keyword()) {
                repeated.push_back(i);
                break;
            }
        }
    }
    return repeated;
}

} // namespace openfits::detail
