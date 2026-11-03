// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "compress/rice.h"

#include <openfits/error.h>

#include "support/sequence.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace {

using openfits::errc;
using openfits::integrity_error;
using openfits::detail::rice_decode;
using openfits::detail::rice_encode;
using openfits::test::splitmix64;
using openfits::test::throws;

template <typename U> std::vector<std::byte> encode(const std::vector<U>& values, std::size_t block_size = 32)
{
    std::vector<std::byte> stream;
    rice_encode(std::span<const U>(values), block_size, stream);
    return stream;
}

template <typename U>
std::vector<U> decode(std::span<const std::byte> stream, std::size_t count, std::size_t block_size = 32)
{
    std::vector<U> values(count);
    rice_decode(stream, block_size, std::span<U>(values));
    return values;
}

std::vector<std::byte> bytes_of(std::initializer_list<int> values)
{
    std::vector<std::byte> bytes;
    for (const int value : values) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

// Values of every kind of block: runs, noise of every width, ramps and jumps across the range of the type.
template <typename U> std::vector<U> sample_values(std::size_t count, std::uint64_t seed)
{
    splitmix64 sequence(seed);
    std::vector<U> values(count);
    U value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t kind = (i / 40) % 5;
        const std::uint64_t noise = sequence.next();
        if (kind == 0) {
            // A constant run.
        } else if (kind == 1) {
            value = static_cast<U>(value + (noise % 7) - 3);
        } else if (kind == 2) {
            value = static_cast<U>(value + (noise % 300));
        } else if (kind == 3) {
            value = static_cast<U>(noise);
        } else {
            value = static_cast<U>(i % 2 == 0 ? std::numeric_limits<U>::max() : 0);
        }
        values[i] = value;
    }
    return values;
}

TEST(rice, writes_the_stream_of_cfitsio)
{
    // 1000, 1001, 1003 in 16 bits: the first value, then one block of the mapped differences 0, 2 and 4, whose sum of 6
    // gives the split level 0 (code 1), each value in unary, the last byte filled with zero bits.
    EXPECT_EQ(encode<std::uint16_t>({1000, 1001, 1003}), bytes_of({0x03, 0xE8, 0x19, 0x08}));
    // A block of no information is its code alone, 0.
    EXPECT_EQ(encode<std::uint8_t>({7, 7, 7, 7}), bytes_of({0x07, 0x00}));
    // A block of noise across the range is stored as it is: code 26 of 5 bits, then each mapped difference in 32 bits.
    const std::vector<std::byte> raw = encode<std::uint32_t>({0, 0x80000000U});
    EXPECT_EQ(raw, bytes_of({0x00, 0x00, 0x00, 0x00, 0xD0, 0x00, 0x00, 0x00, 0x07, 0xFF, 0xFF, 0xFF, 0xF8}));
}

TEST(rice, round_trips_every_width_and_block_size)
{
    for (const std::size_t block : {std::size_t{16}, std::size_t{32}}) {
        for (const std::size_t count :
             {std::size_t{1}, std::size_t{2}, std::size_t{31}, std::size_t{32}, std::size_t{33}, std::size_t{1000}}) {
            const auto values8 = sample_values<std::uint8_t>(count, count);
            EXPECT_EQ(decode<std::uint8_t>(encode(values8, block), count, block), values8) << block << " " << count;
            const auto values16 = sample_values<std::uint16_t>(count, count + 1);
            EXPECT_EQ(decode<std::uint16_t>(encode(values16, block), count, block), values16) << block << " " << count;
            const auto values32 = sample_values<std::uint32_t>(count, count + 2);
            EXPECT_EQ(decode<std::uint32_t>(encode(values32, block), count, block), values32) << block << " " << count;
            const auto values64 = sample_values<std::uint64_t>(count, count + 3);
            EXPECT_EQ(decode<std::uint64_t>(encode(values64, block), count, block), values64) << block << " " << count;
        }
    }
}

TEST(rice, compresses_small_differences)
{
    std::vector<std::uint32_t> values(10'000);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = 100'000 + static_cast<std::uint32_t>(i % 5);
    }
    const std::vector<std::byte> stream = encode(values);
    EXPECT_LT(stream.size(), values.size());
    EXPECT_EQ(decode<std::uint32_t>(stream, values.size()), values);
}

TEST(rice, ignores_the_bytes_after_the_last_block)
{
    const auto values = sample_values<std::uint16_t>(100, 9);
    std::vector<std::byte> stream = encode(values);
    stream.insert(stream.end(), 5, std::byte{0xFF});
    EXPECT_EQ(decode<std::uint16_t>(stream, values.size()), values);
}

TEST(rice, refuses_a_stream_that_ends_early)
{
    const auto values = sample_values<std::uint32_t>(500, 10);
    const std::vector<std::byte> stream = encode(values);
    for (std::size_t length = 0; length < stream.size(); length += 1 + (length / 8)) {
        EXPECT_TRUE(throws<integrity_error>(errc::corrupt_compressed_data, [&] {
            (void)decode<std::uint32_t>(std::span(stream).first(length), values.size());
        })) << length;
    }
}

TEST(rice, refuses_a_stream_too_short_for_its_values_before_decoding)
{
    // Four bytes cannot hold the first value of 32 bits and the codes of 100 blocks.
    EXPECT_EQ(openfits::detail::rice_minimum_size(3200, 4, 32), 67U);
    EXPECT_TRUE(throws<integrity_error>(errc::corrupt_compressed_data,
                                        [] { (void)decode<std::uint32_t>(bytes_of({0, 0, 0, 0, 0, 0, 0}), 3200); }));
}

TEST(rice, refuses_codes_of_no_block)
{
    // Codes 27 to 31 of the 32-bit coder are no block: CFITSIO decodes them as splits beyond its own limit.
    EXPECT_TRUE(throws<integrity_error>(errc::corrupt_compressed_data, [] {
        (void)decode<std::uint32_t>(bytes_of({0, 0, 0, 0, 0xD8, 0, 0, 0, 0, 0, 0, 0}), 2);
    }));
    // A unary part of 269 zeros, which makes the value longer than 8 bits.
    std::vector<std::byte> long_unary = bytes_of({0, 0x20});
    long_unary.insert(long_unary.end(), 33, std::byte{0});
    long_unary.push_back(std::byte{0xFF});
    EXPECT_TRUE(
        throws<integrity_error>(errc::corrupt_compressed_data, [&] { (void)decode<std::uint8_t>(long_unary, 2); }));
}

} // namespace
