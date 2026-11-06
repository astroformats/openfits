// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/card.h>
#include <openfits/header.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace openfits::detail {

/// The most cards of a header that a lookup searches in order, which takes less than an index takes to make: the walk
/// of a file reads the header of every unit, most of them short.
inline constexpr std::size_t searched_cards = 32;

/// The first card of each canonical keyword, for a header of more than searched_cards cards: a table of open addressing
/// over keyword_prefix() and the size of each keyword, of a power of two slots, at least twice its keywords, which a
/// header of n cards takes in one allocation. Keywords longer than their prefix are hashed and compared whole, as their
/// cards give them. Made from the cards in their order, it finds as it goes the cards with a value whose keyword has a
/// value in an earlier card.
struct keyword_table
{
    /// The position that find() gives for a keyword that no card has.
    static constexpr std::size_t none = std::numeric_limits<std::size_t>::max();
    /// The position of a vacant slot, which no card has: the slots hold positions in 32 bits, so that one takes 16
    /// bytes, and a header of more cards than that is searched in order.
    static constexpr std::uint32_t vacant = std::numeric_limits<std::uint32_t>::max();

    struct slot
    {
        std::uint64_t prefix = 0;
        /// The first card of the keyword; vacant in a vacant slot.
        std::uint32_t position = vacant;
        /// The size of the keyword, 9 for any keyword longer than its prefix: keywords of the same prefix and of a size
        /// up to 8 are the same, which tells apart those that differ in zero bytes at their end, as a file that departs
        /// from the standard may hold them.
        std::uint8_t size = 0;
        /// True once a card of the keyword has a value.
        bool valued = false;
    };

    /// An index for about cards cards, which adds them without growing.
    explicit keyword_table(std::size_t cards);

    /// True when a header of cards cards is indexed: more than searched_cards, and positions within the 32 bits of a
    /// slot.
    [[nodiscard]] static bool indexes(std::size_t cards) noexcept
    {
        return cards > searched_cards && cards < vacant;
    }

    /// The first card of keyword, as cards holds them, or none.
    [[nodiscard]] std::size_t find(std::string_view keyword, std::span<const card> cards) const noexcept;

    /// The slot of keyword, or nothing when no card has it.
    [[nodiscard]] slot* slot_of(std::string_view keyword, std::span<const card> cards) noexcept;

    /// Adds the card at position, of keyword, the first of its keyword unless an earlier card has it; a card with a
    /// value, valued, after one of the same keyword with a value goes into repeated.
    void add(std::string_view keyword, std::size_t position, bool valued, std::span<const card> cards);

    std::vector<slot> slots;
    std::size_t used = 0;
    /// The cards with a value whose keyword has a value in an earlier card, in the order they were added.
    std::vector<std::size_t> repeated;

private:
    // The slot that holds keyword, or the vacant slot where it goes.
    [[nodiscard]] std::size_t probe(std::uint64_t prefix, std::uint8_t size, std::uint64_t hash,
                                    std::string_view keyword, std::span<const card> cards) const noexcept;
    void grow(std::span<const card> cards);
};

/// Access to the index of a header, which a reader of a header asks for its repeated keywords.
struct header_access
{
    /// The cards with a value whose keyword has a value in an earlier card, in their order.
    [[nodiscard]] static std::vector<std::size_t> repeated_keywords(const header& cards);
};

} // namespace openfits::detail
