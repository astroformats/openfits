// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>

#include "compress/gzip.h"
#include "compress/shuffle.h"
#include "support/sequence.h"
#include "support/throws.h"

#include <gtest/gtest.h>
#include <zlib.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

using openfits::errc;
using openfits::integrity_error;
using openfits::detail::gzip_decompress;
using openfits::detail::gzip_encoder;
using openfits::test::splitmix64;
using openfits::test::throws;

std::vector<std::byte> bytes_of(std::string_view text)
{
    std::vector<std::byte> bytes;
    for (const char c : text) {
        bytes.push_back(static_cast<std::byte>(c));
    }
    return bytes;
}

// Bytes that compress: runs of a few values, as the high bytes of the pixels of an image are.
std::vector<std::byte> sample_bytes(std::size_t count, std::uint64_t seed)
{
    splitmix64 sequence(seed);
    std::vector<std::byte> bytes(count);
    for (std::size_t i = 0; i < count; ++i) {
        bytes[i] = static_cast<std::byte>((i / 7) % 5 + (sequence.next() % 3));
    }
    return bytes;
}

std::vector<std::byte> compress(std::span<const std::byte> input, int level = -1)
{
    std::vector<std::byte> output;
    gzip_encoder(level).compress(input, output);
    return output;
}

TEST(shuffle, orders_the_bytes_of_values_by_significance)
{
    // The example of standard §10.4.2: five 16-bit values A1 A2 ... E1 E2 become A1 B1 C1 D1 E1 A2 B2 C2 D2 E2.
    const std::vector<std::byte> values = bytes_of("aAbBcCdDeE");
    std::vector<std::byte> shuffled(values.size());
    openfits::detail::shuffle_bytes(values, shuffled, 2);
    EXPECT_EQ(shuffled, bytes_of("abcdeABCDE"));
    std::vector<std::byte> restored(values.size());
    openfits::detail::unshuffle_bytes(shuffled, restored, 2);
    EXPECT_EQ(restored, values);
}

TEST(shuffle, keeps_the_bytes_after_the_last_whole_value)
{
    const std::vector<std::byte> values = bytes_of("1234567890x");
    std::vector<std::byte> shuffled(values.size());
    openfits::detail::shuffle_bytes(values, shuffled, 4);
    // Values 1234 and 5678: the first bytes 1 5, the second 2 6, then 3 7 and 4 8; 90x unchanged.
    EXPECT_EQ(shuffled, bytes_of("1526374890x"));
    for (const std::size_t size : {0U, 1U, 20U}) {
        openfits::detail::shuffle_bytes(values, shuffled, size);
        EXPECT_EQ(shuffled, values) << size;
    }
}

TEST(shuffle, unshuffles_every_value_size_and_length)
{
    for (std::size_t size = 1; size <= 8; ++size) {
        for (std::size_t length = 0; length <= 3 * size + 2; ++length) {
            const std::vector<std::byte> values = sample_bytes(length, (size * 100) + length);
            std::vector<std::byte> shuffled(length);
            std::vector<std::byte> restored(length);
            openfits::detail::shuffle_bytes(values, shuffled, size);
            openfits::detail::unshuffle_bytes(shuffled, restored, size);
            ASSERT_EQ(restored, values) << size << " " << length;
        }
    }
}

// The SIMD kernels move 16 values a register, and the gather of the shuffle stops where a load would leave the input:
// lengths up to some registers of values of each size end with every tail, and both directions give the bytes of the
// scalar code.
TEST(shuffle, the_simd_kernels_give_the_bytes_of_the_scalar_code)
{
    if (!openfits::detail::has_baseline_kernels()) {
        GTEST_SKIP() << "the build has no SIMD kernels";
    }
    using openfits::detail::simd_kernel;
    for (const std::size_t size : {2U, 4U, 8U}) {
        for (std::size_t length = 0; length <= (40 * size) + 7; ++length) {
            const std::vector<std::byte> values = sample_bytes(length, (size * 1000) + length);
            std::vector<std::byte> expected(length);
            std::vector<std::byte> shuffled(length);
            openfits::detail::shuffle_bytes(values, expected, size, simd_kernel::scalar);
            openfits::detail::shuffle_bytes(values, shuffled, size, simd_kernel::best);
            ASSERT_EQ(shuffled, expected) << size << " " << length;
            std::vector<std::byte> restored(length);
            openfits::detail::unshuffle_bytes(values, expected, size, simd_kernel::scalar);
            openfits::detail::unshuffle_bytes(values, restored, size, simd_kernel::best);
            ASSERT_EQ(restored, expected) << size << " " << length;
        }
    }
}

TEST(gzip_tiles, round_trips_at_every_level_and_size)
{
    for (const std::size_t size : {0U, 1U, 100U, 70'000U}) {
        const std::vector<std::byte> input = sample_bytes(size, size);
        for (const int level : {-1, 1, 6, 9}) {
            const std::vector<std::byte> stream = compress(input, level);
            std::vector<std::byte> output(size);
            gzip_decompress(stream, output);
            ASSERT_EQ(output, input) << size << " " << level;
        }
    }
}

TEST(gzip_tiles, writes_the_same_gzip_header_on_every_host)
{
    // RFC 1952: the magic bytes, deflate, no flags, no time, and the operating system unknown.
    const std::vector<std::byte> stream = compress(sample_bytes(1000, 1));
    ASSERT_GT(stream.size(), 10U);
    EXPECT_EQ(stream[0], std::byte{0x1F});
    EXPECT_EQ(stream[1], std::byte{0x8B});
    EXPECT_EQ(stream[2], std::byte{8});
    EXPECT_EQ(stream[3], std::byte{0});
    for (std::size_t i = 4; i < 8; ++i) {
        EXPECT_EQ(stream[i], std::byte{0}) << i;
    }
    EXPECT_EQ(stream[9], std::byte{255});
}

TEST(gzip_tiles, an_encoder_compresses_inputs_one_after_another)
{
    gzip_encoder encoder(6);
    const std::vector<std::byte> first = sample_bytes(5000, 2);
    const std::vector<std::byte> second = sample_bytes(3000, 3);
    std::vector<std::byte> one;
    std::vector<std::byte> two;
    encoder.compress(first, one);
    encoder.compress(second, two);
    EXPECT_EQ(one, compress(first, 6));
    EXPECT_EQ(two, compress(second, 6));
}

TEST(gzip_tiles, decompresses_in_pieces)
{
    const std::vector<std::byte> input = sample_bytes(10'000, 4);
    std::vector<std::byte> stream;
    gzip_encoder(6, 7).compress(input, stream);
    EXPECT_EQ(stream, compress(input, 6));
    std::vector<std::byte> output(input.size());
    gzip_decompress(stream, output, 13);
    EXPECT_EQ(output, input);
}

TEST(gzip_tiles, reads_a_zlib_stream_too)
{
    // The length in zlib's type, which has 32 bits on Windows and is std::size_t where long has 64.
    constexpr uLong length = 2000;
    const std::vector<std::byte> input = sample_bytes(length, 5);
    uLongf size = compressBound(length);
    std::vector<std::byte> stream(size);
    ASSERT_EQ(compress2(reinterpret_cast<Bytef*>(stream.data()), &size, reinterpret_cast<const Bytef*>(input.data()),
                        length, 6),
              Z_OK);
    stream.resize(size);
    std::vector<std::byte> output(input.size());
    gzip_decompress(stream, output);
    EXPECT_EQ(output, input);
}

TEST(gzip_tiles, ignores_the_bytes_after_the_stream)
{
    const std::vector<std::byte> input = sample_bytes(500, 6);
    std::vector<std::byte> stream = compress(input);
    stream.insert(stream.end(), 9, std::byte{0});
    std::vector<std::byte> output(input.size());
    gzip_decompress(stream, output);
    EXPECT_EQ(output, input);
}

TEST(gzip_tiles, refuses_a_stream_of_another_size)
{
    const std::vector<std::byte> input = sample_bytes(500, 7);
    const std::vector<std::byte> stream = compress(input);
    std::vector<std::byte> shorter(input.size() - 1);
    EXPECT_TRUE(throws<integrity_error>(errc::corrupt_compressed_data, [&] { gzip_decompress(stream, shorter); }));
    std::vector<std::byte> longer(input.size() + 1);
    EXPECT_TRUE(throws<integrity_error>(errc::corrupt_compressed_data, [&] { gzip_decompress(stream, longer); }));
}

TEST(gzip_tiles, refuses_a_truncated_or_corrupt_stream)
{
    const std::vector<std::byte> input = sample_bytes(5000, 8);
    const std::vector<std::byte> stream = compress(input);
    std::vector<std::byte> output(input.size());
    for (const std::size_t length : {std::size_t{0}, std::size_t{5}, stream.size() / 2, stream.size() - 1}) {
        EXPECT_TRUE(throws<integrity_error>(errc::corrupt_compressed_data, [&] {
            gzip_decompress(std::span(stream).first(length), output);
        })) << length;
    }
    std::vector<std::byte> corrupt = stream;
    corrupt[0] = std::byte{0};
    EXPECT_TRUE(throws<integrity_error>(errc::corrupt_compressed_data, [&] { gzip_decompress(corrupt, output); }));
    // The CRC-32 of the trailer.
    corrupt = stream;
    corrupt[corrupt.size() - 6] ^= std::byte{1};
    EXPECT_TRUE(throws<integrity_error>(errc::corrupt_compressed_data, [&] { gzip_decompress(corrupt, output); }));
}

} // namespace
