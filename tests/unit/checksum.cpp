// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "checksum/checksum.h"

#include <openfits/checksum.h>
#include <openfits/header.h>

#include "header/emission.h"
#include "support/checksums.h"
#include "support/fixture_builder.h"
#include "support/sequence.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace {

using openfits::detail::add_sums;
using openfits::detail::checksum_accumulator;
using openfits::detail::datasum_of;
using openfits::detail::has_checksum;
using openfits::detail::sum_of;
using openfits::test::card_record;
using openfits::test::reference_sum;
using openfits::test::splitmix64;

constexpr std::uint32_t negative_zero = 0xFFFFFFFFU;

std::vector<std::byte> random_bytes(std::size_t count, std::uint64_t seed)
{
    splitmix64 sequence(seed);
    std::vector<std::byte> bytes(count);
    for (std::byte& value : bytes) {
        value = static_cast<std::byte>(sequence.next() & 0xFFU);
    }
    return bytes;
}

std::span<const std::byte> bytes_of(const std::string& text)
{
    return std::as_bytes(std::span(text));
}

// The class of a ones' complement sum modulo 2^32 - 1, where 0 and 0xFFFFFFFF are both zero.
std::uint32_t residue(std::uint32_t sum)
{
    return sum == negative_zero ? 0 : sum;
}

openfits::header header_of(const std::vector<std::string>& records)
{
    std::vector<openfits::card> cards;
    cards.reserve(records.size());
    for (const std::string& text : records) {
        cards.push_back(openfits::card::parse(text));
    }
    return openfits::header(std::move(cards));
}

TEST(checksum, sums_add_the_carry_back_into_the_lowest_bit)
{
    EXPECT_EQ(add_sums(0, 0), 0U);
    EXPECT_EQ(add_sums(1, 2), 3U);
    EXPECT_EQ(add_sums(0x80000000U, 0x80000000U), 1U);
    EXPECT_EQ(add_sums(negative_zero, 1), 1U);
    EXPECT_EQ(add_sums(negative_zero, negative_zero), negative_zero);
    // A sum and its complement make negative zero, which CHECKSUM relies on (standard Appendix J.2).
    for (const std::uint32_t value : {0U, 1U, 0x12345678U, 0x80000000U, 0xFFFFFFFEU, negative_zero}) {
        EXPECT_EQ(add_sums(value, ~value), negative_zero) << value;
    }
}

TEST(checksum, sums_take_big_endian_integers)
{
    const std::vector<std::byte> bytes{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
                                       std::byte{0x05}};
    EXPECT_EQ(sum_of(std::span(bytes).first(4)), 0x01020304U);
    // A last integer that the bytes end within is completed with zeros.
    EXPECT_EQ(sum_of(bytes), 0x06020304U);
    EXPECT_EQ(sum_of({}), 0U);
    EXPECT_EQ(sum_of(std::vector<std::byte>(2880)), 0U);
    // Ones never sum to 0: negative zero stays negative zero.
    EXPECT_EQ(sum_of(std::vector<std::byte>(2880, std::byte{0xFF})), negative_zero);
}

TEST(checksum, sums_of_every_length_match_the_plain_sum)
{
    const std::vector<std::byte> bytes = random_bytes(5000, 1);
    for (std::size_t length = 0; length <= 80; ++length) {
        EXPECT_EQ(sum_of(std::span(bytes).first(length)), reference_sum(std::span(bytes).first(length))) << length;
    }
    for (const std::size_t length : {2879U, 2880U, 2881U, 4999U, 5000U}) {
        EXPECT_EQ(sum_of(std::span(bytes).first(length)), reference_sum(std::span(bytes).first(length))) << length;
    }
}

TEST(checksum, sums_fold_in_rounds_of_any_size)
{
    // The integers that carry the most into each round.
    const std::vector<std::byte> ones(1000, std::byte{0xFF});
    const std::vector<std::byte> bytes = random_bytes(1000, 2);
    for (const std::size_t round : {1U, 2U, 3U, 7U, 125U, 1000U}) {
        EXPECT_EQ(sum_of(ones, round), negative_zero) << round;
        EXPECT_EQ(sum_of(bytes, round), reference_sum(bytes)) << round;
    }
}

TEST(checksum, sums_of_pieces_continue_their_integers)
{
    const std::vector<std::byte> bytes = random_bytes(103, 3);
    const std::uint32_t whole = reference_sum(bytes);
    for (std::size_t first = 0; first <= bytes.size(); ++first) {
        for (std::size_t second = first; second <= bytes.size(); second += 5) {
            checksum_accumulator sum;
            sum.add(std::span(bytes).first(first));
            sum.add(std::span(bytes).subspan(first, second - first));
            sum.add(std::span(bytes).subspan(second));
            ASSERT_EQ(sum.sum(), whole) << first << ' ' << second;
        }
    }
    // Byte by byte, and a sum read between the pieces.
    checksum_accumulator sum;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        sum.add(std::span(bytes).subspan(i, 1));
        ASSERT_EQ(sum.sum(), reference_sum(std::span(bytes).first(i + 1))) << i;
    }
}

// Zeros added without their bytes, as a reserved unit or a file made longer writes them, take their places among the
// integers of the sum, and add nothing to it.
TEST(checksum, zeros_take_their_places_without_their_bytes)
{
    const std::vector<std::byte> head = random_bytes(11, 5);
    const std::vector<std::byte> tail = random_bytes(13, 6);
    for (std::size_t lead = 0; lead <= head.size(); ++lead) {
        for (const std::uint64_t zeros : {0U, 1U, 2U, 3U, 4U, 5U, 7U, 4096U, 4097U}) {
            std::vector<std::byte> bytes(head.begin(), head.begin() + static_cast<std::ptrdiff_t>(lead));
            bytes.resize(lead + zeros);
            bytes.insert(bytes.end(), tail.begin(), tail.end());
            checksum_accumulator sum;
            sum.add(std::span(head).first(lead));
            sum.add_zeros(zeros);
            sum.add(tail);
            ASSERT_EQ(sum.sum(), reference_sum(bytes)) << lead << ' ' << zeros;
        }
    }
}

TEST(checksum, sums_are_unchanged_by_trailing_zeros)
{
    std::vector<std::byte> bytes = random_bytes(37, 4);
    const std::uint32_t sum = sum_of(bytes);
    bytes.resize(2880);
    EXPECT_EQ(sum_of(bytes), sum);
}

// Standard Appendix J.3: an HDU of sum 868229149, hexadecimal 33C0201D.
TEST(checksum, encodes_the_example_of_the_standard)
{
    EXPECT_EQ(openfits::encode_checksum(868229149U), "hcHjjc9ghcEghc9g");
    EXPECT_EQ(openfits::decode_checksum("hcHjjc9ghcEghc9g"), 868229149U);
    // The placeholder of Appendix J.1 encodes the complement of negative zero.
    EXPECT_EQ(openfits::encode_checksum(negative_zero), "0000000000000000");
    EXPECT_EQ(openfits::decode_checksum("0000000000000000"), negative_zero);
}

// Standard Appendix J.2: the characters are alphanumerics, the 13 characters of punctuation between 9 and A and
// between Z and a never appear, and each byte is encoded alone, in four characters of its own.
TEST(checksum, encodes_every_byte_in_alphanumerics)
{
    std::set<char> seen;
    for (unsigned position = 0; position < 4; ++position) {
        for (unsigned byte = 0; byte < 256; ++byte) {
            const std::uint32_t complement = byte << (8U * position);
            const std::string text = openfits::encode_checksum(~complement);
            ASSERT_EQ(text.size(), 16U);
            for (const char c : text) {
                ASSERT_TRUE((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) << text;
                seen.insert(c);
            }
            ASSERT_EQ(openfits::decode_checksum(text), ~complement) << text;
        }
    }
    // The characters run from 0 to r, the largest being a byte of 255 (Appendix J.2, step 3): all 54 of them appear.
    std::set<char> expected;
    for (const auto& [from, to] : {std::pair{'0', '9'}, std::pair{'A', 'Z'}, std::pair{'a', 'r'}}) {
        for (char c = from; c <= to; ++c) {
            expected.insert(c);
        }
    }
    EXPECT_EQ(seen, expected);
}

TEST(checksum, decodes_what_it_encodes)
{
    splitmix64 sequence(5);
    std::vector<std::uint32_t> values{0U, 1U, 2U, 0x7FFFFFFFU, 0x80000000U, 0xFFFFFFFEU, negative_zero};
    for (int i = 0; i < 20000; ++i) {
        values.push_back(static_cast<std::uint32_t>(sequence.next() >> 32U));
    }
    for (const std::uint32_t value : values) {
        ASSERT_EQ(openfits::decode_checksum(openfits::encode_checksum(value)), value) << value;
    }
}

TEST(checksum, decodes_text_of_16_characters_only)
{
    EXPECT_FALSE(openfits::decode_checksum(""));
    EXPECT_FALSE(openfits::decode_checksum("hcHjjc9ghcEghc9"));
    EXPECT_FALSE(openfits::decode_checksum("hcHjjc9ghcEghc9gh"));
}

// Any 16 characters in the place of the 16 zeros change the sum of their record by the complement of what they decode
// to, modulo 2^32 - 1: punctuation, spaces and characters before 0 included.
TEST(checksum, decodes_any_text_as_its_change_of_the_sum)
{
    const std::string zeros = card_record("CHECKSUM", "'0000000000000000'");
    splitmix64 sequence(6);
    for (int i = 0; i < 2000; ++i) {
        std::string text(16, ' ');
        for (char& c : text) {
            // The 95 characters from the space to the tilde.
            c = static_cast<char>(0x20U + (sequence.next() % 95U));
        }
        const std::string record = card_record("CHECKSUM", "'" + text + "'");
        const std::optional<std::uint32_t> decoded = openfits::decode_checksum(text);
        ASSERT_TRUE(decoded.has_value());
        const std::uint32_t expected = add_sums(sum_of(bytes_of(zeros)), ~decoded.value_or(0));
        ASSERT_EQ(residue(sum_of(bytes_of(record))), residue(expected)) << text;
    }
}

TEST(checksum, datasum_records_a_decimal_sum)
{
    const auto datasum = [](const std::string& value) {
        return datasum_of(header_of({card_record("SIMPLE", "T"), card_record("DATASUM", value)}));
    };
    EXPECT_EQ(datasum("'2503531142'").value, 2503531142U);
    // Blanks before and after, and zeros before, are not significant (standard §4.4.2.7).
    EXPECT_EQ(datasum("'  0002503531142 '").value, 2503531142U);
    EXPECT_EQ(datasum("'0'").value, 0U);
    EXPECT_EQ(datasum("'4294967295'").value, 4294967295U);
    // An integer without quotes, as CFITSIO reads it.
    EXPECT_EQ(datasum("2503531142").value, 2503531142U);
    for (const char* value : {"''", "'          '", ""}) {
        EXPECT_FALSE(datasum(value).known) << value;
    }
    for (const char* value : {"'4294967296'", "'12.5'", "'abc'", "'-1'", "'1 2'", "-1", "4294967296", "12.0", "T"}) {
        const openfits::detail::recorded_datasum recorded = datasum(value);
        EXPECT_TRUE(recorded.known) << value;
        EXPECT_FALSE(recorded.value) << value;
    }
    EXPECT_FALSE(datasum_of(header_of({card_record("SIMPLE", "T")})).known);
    // A record of the keyword without a value indicator holds no value.
    EXPECT_FALSE(datasum_of(header_of({card_record("SIMPLE", "T"), openfits::test::record("DATASUM   '123'")})).known);
}

TEST(checksum, checksum_is_present_unless_blank)
{
    const auto present = [](const std::string& value) {
        return has_checksum(header_of({card_record("SIMPLE", "T"), card_record("CHECKSUM", value)}));
    };
    EXPECT_TRUE(present("'hcHjjc9ghcEghc9g'"));
    EXPECT_TRUE(present("'0000000000000000'"));
    // A value of another type is checked by the sum all the same.
    EXPECT_TRUE(present("12"));
    for (const char* value : {"''", "'                '", ""}) {
        EXPECT_FALSE(present(value)) << value;
    }
    EXPECT_FALSE(has_checksum(header_of({card_record("SIMPLE", "T")})));
}

TEST(checksum, sealed_headers_sum_to_negative_zero_with_their_data)
{
    const std::vector<openfits::card> structural{openfits::card("SIMPLE", true), openfits::card("BITPIX", 8),
                                                 openfits::card("NAXIS", 0)};
    openfits::header caller;
    caller.set("OBJECT", "M 31");
    const std::vector<openfits::card> closing = openfits::detail::checksum_cards("2026-11-02T10:00:00");
    for (const std::uint32_t data_sum : {0U, 1U, 2503531142U, negative_zero}) {
        std::string blocks = openfits::detail::header_blocks(structural, caller, {.closing = closing});
        const std::string before = blocks;
        const openfits::detail::byte_range changed =
            openfits::detail::seal_checksums(blocks, data_sum, "2026-11-02T10:00:00", {});
        EXPECT_EQ(add_sums(reference_sum(bytes_of(blocks)), data_sum), negative_zero);
        // The two records after the caller's cards, CHECKSUM first, as CFITSIO and astropy place them; nothing else
        // changes.
        EXPECT_EQ(changed.offset, 4 * openfits::test::record_size);
        EXPECT_EQ(changed.size, 2 * openfits::test::record_size);
        EXPECT_EQ(blocks.substr(0, changed.offset), before.substr(0, changed.offset));
        EXPECT_EQ(blocks.substr(changed.offset + changed.size), before.substr(changed.offset + changed.size));
        // CHECKSUM in the fixed format that the encoding needs: the quotes in bytes 11 and 28 (standard Appendix J.1).
        const std::string checksum = blocks.substr(changed.offset, openfits::test::record_size);
        EXPECT_EQ(checksum.substr(0, 11), "CHECKSUM= '");
        EXPECT_EQ(checksum[27], '\'');
        // It encodes the sum of the header with the placeholder, DATASUM complete, and of the data.
        std::string placeholder = blocks;
        placeholder.replace(changed.offset + 11, 16, "0000000000000000");
        EXPECT_EQ(openfits::decode_checksum(checksum.substr(11, 16)),
                  add_sums(reference_sum(bytes_of(placeholder)), data_sum));
        const openfits::card datasum = openfits::card::parse(blocks.substr(changed.offset + 80, 80));
        EXPECT_EQ(datasum.keyword(), "DATASUM");
        EXPECT_EQ(std::stoull(datasum.value().get<std::string>()), data_sum);
        EXPECT_EQ(datasum.comment(), "2026-11-02T10:00:00");
    }
}

} // namespace
