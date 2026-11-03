// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "compress/plio.h"

#include <openfits/error.h>

#include "support/sequence.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

using openfits::errc;
using openfits::integrity_error;
using openfits::usage_error;
using openfits::detail::plio_decode;
using openfits::detail::plio_encode;
using openfits::test::splitmix64;
using openfits::test::throws;

std::vector<std::int16_t> encode(const std::vector<std::int32_t>& values)
{
    std::vector<std::int16_t> words;
    plio_encode(values, words);
    return words;
}

std::vector<std::int32_t> decode(const std::vector<std::int16_t>& words, std::size_t count)
{
    std::vector<std::int32_t> values(count, -1);
    plio_decode(words, values);
    return values;
}

std::vector<std::int16_t> words_of(const std::vector<std::uint16_t>& hex)
{
    std::vector<std::int16_t> words;
    words.reserve(hex.size());
    for (const std::uint16_t value : hex) {
        words.push_back(static_cast<std::int16_t>(value));
    }
    return words;
}

std::vector<std::int32_t> repeated(std::int32_t value, std::size_t count)
{
    return std::vector<std::int32_t>(count, value);
}

std::vector<std::int32_t> joined(std::initializer_list<std::vector<std::int32_t>> parts)
{
    std::vector<std::int32_t> values;
    for (const std::vector<std::int32_t>& part : parts) {
        values.insert(values.end(), part.begin(), part.end());
    }
    return values;
}

// The lists that fpack 1.7.0 of CFITSIO 4.7.0 writes for these tiles, word for word.
struct listed_tile
{
    std::vector<std::int32_t> values{};
    std::vector<std::uint16_t> words{};
};

TEST(plio, encodes_as_cfitsio_writes)
{
    const std::vector<listed_tile> tiles{
        {.values = {0, 0, 5, 5, 0, 0, 0, 10, 0, 0},
         .words = {0x0000, 0x0007, 0xFF9C, 0x000D, 0x0000, 0x0000, 0x0000, 0x2004, 0x0002, 0x4002, 0x2005, 0x5004,
                   0x0002}},
        {.values = {1, 10000, 1, 10000},
         .words = {0x0000, 0x0007, 0xFF9C, 0x0011, 0x0000, 0x0000, 0x0000, 0x4001, 0x1710, 0x0002, 0x4001, 0x1001,
                   0x0000, 0x4001, 0x1710, 0x0002, 0x4001}},
        {.values = {100, 80, 60, 40, 20},
         .words = {0x0000, 0x0007, 0xFF9C, 0x000C, 0x0000, 0x0000, 0x0000, 0x6063, 0x7014, 0x7014, 0x7014, 0x7014}},
        {.values = repeated(3, 5000),
         .words = {0x0000, 0x0007, 0xFF9C, 0x000A, 0x0000, 0x0000, 0x0000, 0x2002, 0x4FFF, 0x4389}},
        {.values = {1, 1, 2, 0, 9000, 9000, 0, 0, 1},
         .words = {0x0000, 0x0007, 0xFF9C, 0x0010, 0x0000, 0x0000, 0x0000, 0x4002, 0x6001, 0x1328, 0x0002, 0x0001,
                   0x4002, 0x1001, 0x0000, 0x5003}},
        {.values = {16777215, 4096, 4095, 1},
         .words = {0x0000, 0x0007, 0xFF9C, 0x000F, 0x0000, 0x0000, 0x0000, 0x1FFF, 0x0FFF, 0x4001, 0x1000, 0x0001,
                   0x4001, 0x7001, 0x7FFE}},
        {.values = repeated(0, 10), .words = {0x0000, 0x0007, 0xFF9C, 0x0008, 0x0000, 0x0000, 0x0000, 0x000A}},
    };
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        EXPECT_EQ(encode(tiles[i].values), words_of(tiles[i].words)) << i;
        EXPECT_EQ(decode(words_of(tiles[i].words), tiles[i].values.size()), tiles[i].values) << i;
    }
}

TEST(plio, writes_a_pixel_after_zeros_of_a_multiple_of_4095_in_a_word_of_its_own)
{
    // CFITSIO turns the last ZN 4095 into 0x6000, IS 0, which loses the 4095 zeros: 0x5001 after it keeps them.
    const std::vector<std::int32_t> values = joined({repeated(0, 4095), {7}, repeated(0, 3)});
    const std::vector<std::int16_t> words = encode(values);
    EXPECT_EQ(words,
              words_of({0x0000, 0x0007, 0xFF9C, 0x000B, 0x0000, 0x0000, 0x0000, 0x2006, 0x0FFF, 0x5001, 0x0003}));
    EXPECT_EQ(decode(words, values.size()), values);
    const std::vector<std::int32_t> longer = joined({repeated(0, 8190), {9}});
    EXPECT_EQ(decode(encode(longer), longer.size()), longer);
}

TEST(plio, reads_the_word_of_cfitsio_after_zeros_of_4095_as_cfitsio_reads_it)
{
    // fpack's list of 4095 zeros, 7 and 7904 zeros, which funpack restores as 7 and 11,999 zeros.
    const std::vector<std::int16_t> words =
        words_of({0x0000, 0x0007, 0xFF9C, 0x000B, 0x0000, 0x0000, 0x0000, 0x2006, 0x6000, 0x0FFF, 0x0EE1});
    const std::vector<std::int32_t> expected = joined({{7}, repeated(0, 11'999)});
    EXPECT_EQ(decode(words, 12'000), expected);
}

TEST(plio, round_trips_runs_of_every_length_and_value)
{
    splitmix64 sequence(42);
    for (int tile = 0; tile < 200; ++tile) {
        std::vector<std::int32_t> values;
        const std::size_t runs = 1 + (sequence.next() % 40);
        for (std::size_t r = 0; r < runs; ++r) {
            const std::uint64_t kind = sequence.next() % 4;
            std::size_t length = 1 + (sequence.next() % 30);
            if (sequence.next() % 10 == 0) {
                length = 4094 + (sequence.next() % 8200);
            }
            std::int32_t value = 0;
            if (kind == 1) {
                value = static_cast<std::int32_t>(1 + (sequence.next() % 10));
            } else if (kind == 2) {
                value = static_cast<std::int32_t>(sequence.next() % (std::uint64_t{1} << 24));
            } else if (kind == 3) {
                value = static_cast<std::int32_t>(sequence.next() % 9000);
            }
            values.insert(values.end(), length, value);
        }
        const std::vector<std::int16_t> words = encode(values);
        ASSERT_LE(words.size(), openfits::detail::plio_max_words(values.size()));
        ASSERT_EQ(decode(words, values.size()), values) << tile;
    }
}

TEST(plio, stays_within_three_words_a_pixel)
{
    // Single pixels whose values jump by more than 4095 take SH and HN 1, the worst case.
    std::vector<std::int32_t> values;
    values.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        values.push_back(i % 2 == 0 ? 5000 : 10000);
    }
    const std::vector<std::int16_t> words = encode(values);
    EXPECT_EQ(words.size(), openfits::detail::plio_max_words(values.size()));
    EXPECT_EQ(decode(words, values.size()), values);
    // A list longer than 32,767 words holds its length in two words.
    EXPECT_EQ(words[3], static_cast<std::int16_t>(3007 % 32768));
    std::vector<std::int32_t> many(20'000);
    for (std::size_t i = 0; i < many.size(); ++i) {
        many[i] = i % 2 == 0 ? 5000 : 10000;
    }
    const std::vector<std::int16_t> long_list = encode(many);
    EXPECT_EQ(long_list[4], 1);
    EXPECT_EQ(decode(long_list, many.size()), many);
}

TEST(plio, refuses_values_beyond_24_bits)
{
    std::vector<std::int16_t> words;
    EXPECT_TRUE(
        throws<usage_error>(errc::invalid_argument, [&] { plio_encode(std::vector<std::int32_t>{-1}, words); }));
    EXPECT_TRUE(
        throws<usage_error>(errc::invalid_argument, [&] { plio_encode(std::vector<std::int32_t>{1 << 24}, words); }));
    EXPECT_NO_THROW(plio_encode(std::vector<std::int32_t>{(1 << 24) - 1}, words));
}

TEST(plio, reads_the_header_of_three_words)
{
    // The old header gives the length in word 2, the instructions after it.
    EXPECT_EQ(decode(words_of({0x0000, 0x0000, 0x0005, 0x2004, 0x4003}), 5),
              (std::vector<std::int32_t>{5, 5, 5, 0, 0}));
}

TEST(plio, fills_the_pixels_that_a_list_leaves_out_with_zeros)
{
    EXPECT_EQ(decode(words_of({0x0000, 0x0007, 0xFF9C, 0x0008, 0x0000, 0x0000, 0x0000, 0x4002}), 5),
              (std::vector<std::int32_t>{1, 1, 0, 0, 0}));
    // A run beyond the tile is cut.
    EXPECT_EQ(decode(words_of({0x0000, 0x0007, 0xFF9C, 0x0008, 0x0000, 0x0000, 0x0000, 0x4FFF}), 3),
              (std::vector<std::int32_t>{1, 1, 1}));
}

TEST(plio, refuses_lists_that_are_malformed_or_end_early)
{
    const auto corrupt = [](const std::vector<std::uint16_t>& words, std::size_t count) {
        return throws<integrity_error>(errc::corrupt_compressed_data, [&] { (void)decode(words_of(words), count); });
    };
    EXPECT_TRUE(corrupt({}, 1));
    EXPECT_TRUE(corrupt({0x0000, 0x0007}, 1));
    EXPECT_TRUE(corrupt({0x0000, 0x0007, 0xFF9C, 0x0008}, 1));
    // A length of no words, and a first instruction before the list.
    EXPECT_TRUE(corrupt({0x0000, 0x0007, 0xFF9C, 0x0000, 0x0000, 0x0000, 0x0000}, 1));
    EXPECT_TRUE(corrupt({0x0000, 0xFFFF, 0xFF9C, 0x0008, 0x0000, 0x0000, 0x0000, 0x4001}, 1));
    // A length beyond the words there are, and SH without its second word.
    EXPECT_TRUE(corrupt({0x0000, 0x0007, 0xFF9C, 0x0009, 0x0000, 0x0000, 0x0000, 0x0001}, 10));
    EXPECT_TRUE(corrupt({0x0000, 0x0007, 0xFF9C, 0x0008, 0x0000, 0x0000, 0x0000, 0x1001}, 1));
    // A word of no instruction.
    EXPECT_TRUE(corrupt({0x0000, 0x0007, 0xFF9C, 0x0008, 0x0000, 0x0000, 0x0000, 0x8001}, 1));
    // A high value beyond 32 bits: 2^27 from SH, then 4095 more half a million times.
    std::vector<std::uint16_t> rising{0x0000, 0x0007, 0xFF9C, 0x0000, 0x0000, 0x0000, 0x0000, 0x1000, 0x7FFF};
    rising.insert(rising.end(), 520'000, 0x2FFF);
    rising[3] = static_cast<std::uint16_t>(rising.size() % 32768);
    rising[4] = static_cast<std::uint16_t>(rising.size() / 32768);
    EXPECT_TRUE(corrupt(rising, 1));
}

} // namespace
