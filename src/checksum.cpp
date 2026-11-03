// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/checksum.h>

#include <array>
#include <cstddef>

namespace openfits {

namespace {

// The ones' complement sums of 32 bits are the integers modulo 2^32 - 1.
constexpr std::int64_t modulus = 0xFFFFFFFF;
constexpr unsigned zero = 0x30U;

// The punctuation between the digits and the capital letters, and between those and the small letters, which the
// encoding leaves out (standard Appendix J.2).
constexpr bool is_punctuation(unsigned c) noexcept
{
    return (c >= 0x3AU && c <= 0x40U) || (c >= 0x5BU && c <= 0x60U);
}

} // namespace

std::string encode_checksum(std::uint32_t sum)
{
    // Each byte of the complement, the most significant first, becomes four characters that hold a quarter of it each,
    // the remainder in the first, plus the offset of '0': the four integers of the 16 characters, whose bytes take the
    // place of the byte they come from, sum to the complement, and the offsets cancel those of the 16 zeros they
    // replace. A pair of characters that holds punctuation moves one apart from the other, which keeps their sum.
    const std::uint32_t value = ~sum;
    std::array<char, 16> integers{};
    for (std::size_t i = 0; i < 4; ++i) {
        const unsigned byte = (value >> (24U - (8U * i))) & 0xFFU;
        const unsigned quarter = (byte / 4U) + zero;
        std::array<unsigned, 4> characters{quarter + (byte % 4U), quarter, quarter, quarter};
        for (std::size_t first = 0; first < 4; first += 2) {
            while (is_punctuation(characters[first]) || is_punctuation(characters[first + 1])) {
                ++characters[first];
                --characters[first + 1];
            }
        }
        for (std::size_t j = 0; j < 4; ++j) {
            integers[(4 * j) + i] = static_cast<char>(characters[j]);
        }
    }
    // One place to the right: the string starts at byte 12 of its record, the last byte of an integer of the unit.
    std::string text(16, ' ');
    for (std::size_t i = 0; i < 16; ++i) {
        text[i] = integers[(i + 15) % 16];
    }
    return text;
}

std::optional<std::uint32_t> decode_checksum(std::string_view text) noexcept
{
    if (text.size() != 16) {
        return std::nullopt;
    }
    // One place to the left, the offset of '0' taken from each character: the sum of the four integers is the
    // complement of the sum, exactly for an encoding, whose bytes carry nothing into each other.
    std::int64_t total = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        const std::int64_t character = static_cast<unsigned char>(text[(i + 1) % 16]);
        total += (character - std::int64_t{zero}) * (std::int64_t{1} << (8U * (3U - (i % 4U))));
    }
    // A ones' complement sum: negative zero for any multiple of 2^32 - 1 but 0.
    std::uint32_t complement = 0;
    if (total != 0) {
        const std::int64_t remainder = ((total % modulus) + modulus) % modulus;
        complement = remainder == 0 ? 0xFFFFFFFFU : static_cast<std::uint32_t>(remainder);
    }
    return ~complement;
}

} // namespace openfits
