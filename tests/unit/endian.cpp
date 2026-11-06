// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "core/endian.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ostream>
#include <span>
#include <string>
#include <vector>

namespace {

using openfits::detail::byte_swap;
using openfits::detail::convert_big_endian;
using openfits::detail::copy_big_endian;
using openfits::detail::has_baseline_kernels;
using openfits::detail::load_big_endian;
using openfits::detail::processor_has_avx2;
using openfits::detail::simd_kernel;
using openfits::detail::store_big_endian;
using openfits::detail::swap_byte_order;
using openfits::detail::swap_items;

// The bytes 0, 1, 2, ..., count - 1.
std::vector<std::byte> sequence(std::size_t count)
{
    std::vector<std::byte> bytes(count);
    for (std::size_t i = 0; i < count; ++i) {
        bytes[i] = static_cast<std::byte>(i);
    }
    return bytes;
}

// The bytes of sequence(count) with each item of item_size bytes reversed and the bytes after the last one kept.
std::vector<std::byte> swapped_sequence(std::size_t count, std::size_t item_size)
{
    std::vector<std::byte> bytes = sequence(count);
    const std::size_t whole = count - (count % item_size);
    for (std::size_t start = 0; start < whole; start += item_size) {
        for (std::size_t i = 0; i < item_size; ++i) {
            bytes[start + i] = static_cast<std::byte>(start + item_size - 1 - i);
        }
    }
    return bytes;
}

TEST(endian, byte_swap_reverses_the_bytes)
{
    static_assert(byte_swap(std::uint8_t{0x12}) == 0x12);
    static_assert(byte_swap(std::uint16_t{0x1234}) == 0x3412);
    static_assert(byte_swap(std::uint32_t{0x12345678}) == 0x78563412);
    static_assert(byte_swap(std::uint64_t{0x0123456789ABCDEF}) == 0xEFCDAB8967452301);
}

// FITS data are big-endian (standard §5.2, §5.3): the most significant byte comes first, whatever the host.
TEST(endian, loads_read_big_endian_bytes_on_any_host)
{
    const std::array<std::byte, 8> bytes{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
                                         std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}};
    const std::span<const std::byte, 8> all(bytes);

    EXPECT_EQ(load_big_endian<std::uint16_t>(all.first<2>()), 0x0102);
    EXPECT_EQ(load_big_endian<std::uint32_t>(all.first<4>()), 0x01020304U);
    EXPECT_EQ(load_big_endian<std::uint64_t>(all), 0x0102030405060708U);
}

TEST(endian, stores_write_big_endian_bytes_on_any_host)
{
    std::array<std::byte, 8> bytes{};

    store_big_endian(std::span<std::byte, 2>(bytes.data(), 2), std::uint16_t{0x0102});
    EXPECT_EQ(bytes[0], std::byte{0x01});
    EXPECT_EQ(bytes[1], std::byte{0x02});

    store_big_endian(std::span<std::byte, 8>(bytes), std::uint64_t{0x0102030405060708});
    EXPECT_EQ(bytes, (std::array{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}, std::byte{0x05},
                                 std::byte{0x06}, std::byte{0x07}, std::byte{0x08}}));
}

// Each item size has its own loop; the tails of zero to item_size - 1 bytes are kept as they are.
class item_sizes : public testing::TestWithParam<std::size_t>
{};

TEST_P(item_sizes, swap_reverses_each_item_and_keeps_the_bytes_after_the_last_one)
{
    const std::size_t item_size = GetParam();
    for (std::size_t tail = 0; tail < item_size; ++tail) {
        std::vector<std::byte> data = sequence((3 * item_size) + tail);
        swap_byte_order(data, item_size);
        EXPECT_EQ(data, swapped_sequence(data.size(), item_size)) << "tail of " << tail;
    }
}

// Little-endian hosts swap, big-endian hosts leave the data.
TEST_P(item_sizes, convert_swaps_only_on_a_little_endian_host)
{
    const std::size_t item_size = GetParam();
    std::vector<std::byte> data = sequence((5 * item_size) + 1);
    convert_big_endian(data, item_size);
    if constexpr (std::endian::native == std::endian::little) {
        EXPECT_EQ(data, swapped_sequence(data.size(), item_size));
    } else {
        EXPECT_EQ(data, sequence(data.size()));
    }
}

TEST_P(item_sizes, copy_converts_without_modifying_the_source)
{
    const std::size_t item_size = GetParam();
    for (std::size_t tail = 0; tail < item_size; ++tail) {
        const std::vector<std::byte> source = sequence((4 * item_size) + tail);
        std::vector<std::byte> target(source.size());
        copy_big_endian(source, target, item_size);

        std::vector<std::byte> expected = source;
        convert_big_endian(expected, item_size);
        EXPECT_EQ(target, expected) << "tail of " << tail;
        EXPECT_EQ(source, sequence(source.size()));
    }
}

INSTANTIATE_TEST_SUITE_P(endian, item_sizes, testing::Values(2, 4, 8));

// The SIMD kernels give the bytes of the scalar code for every item size, with and without a mask, from data of every
// length up to some registers of each kind, so that each kernel ends with every tail its registers leave: whole
// registers of 16 and 32 bytes, four of them a turn, then the items left, then the bytes after the last item.
struct kernel_case
{
    simd_kernel kernel = simd_kernel::baseline;
    const char* name = "";
};

// The name of the kernel, which CTest names the test after.
// NOLINTNEXTLINE(readability-identifier-naming)
void PrintTo(const kernel_case& value, std::ostream* output)
{
    *output << value.name;
}

class swap_kernels : public testing::TestWithParam<kernel_case>
{};

TEST_P(swap_kernels, give_the_bytes_of_the_scalar_code_for_every_length)
{
    const simd_kernel kernel = GetParam().kernel;
    if (!has_baseline_kernels() || (kernel == simd_kernel::best && !processor_has_avx2())) {
        GTEST_SKIP() << "the kernel falls back to another one on this processor or in this build";
    }
    for (const std::size_t item_size : {2U, 4U, 8U}) {
        // The sign bit of a value converted from big-endian to native order and back, as conversions flip it.
        const std::uint64_t sign = std::uint64_t{1} << ((8 * item_size) - 1);
        for (const std::uint64_t mask : {std::uint64_t{0}, sign, byte_swap(sign) >> (64 - (8 * item_size))}) {
            for (std::size_t length = 0; length <= 300; ++length) {
                const std::vector<std::byte> source = sequence(length);
                std::vector<std::byte> expected(length);
                swap_items(source, expected, item_size, mask, simd_kernel::scalar);
                std::vector<std::byte> copied(length);
                swap_items(source, copied, item_size, mask, kernel);
                EXPECT_EQ(copied, expected) << item_size << " bytes, mask " << mask << ", length " << length;
                std::vector<std::byte> in_place = source;
                swap_items(in_place, in_place, item_size, mask, kernel);
                EXPECT_EQ(in_place, expected) << item_size << " bytes, mask " << mask << ", length " << length;
            }
        }
    }
}

// The scalar code reverses each item and then flips the bits of the mask, as an integer of the host.
TEST(endian, swap_items_flips_the_bits_of_the_mask_after_the_swap)
{
    const std::vector<std::byte> source = sequence(9);
    std::vector<std::byte> target(9);
    swap_items(source, target, 4, 0x80000000U, simd_kernel::scalar);

    std::vector<std::byte> expected = swapped_sequence(9, 4);
    for (const std::size_t item : {0U, 4U}) {
        std::uint32_t value = 0;
        std::memcpy(&value, expected.data() + item, 4);
        value ^= 0x80000000U;
        std::memcpy(expected.data() + item, &value, 4);
    }
    EXPECT_EQ(target, expected);
}

std::string kernel_name(const testing::TestParamInfo<kernel_case>& instance)
{
    return instance.param.name;
}

INSTANTIATE_TEST_SUITE_P(endian, swap_kernels,
                         testing::Values(kernel_case{.kernel = simd_kernel::baseline, .name = "baseline"},
                                         kernel_case{.kernel = simd_kernel::best, .name = "best"}),
                         kernel_name);

// One-byte items, the bytes of BITPIX 8, have no byte order; neither has an item size the format does not use.
TEST(endian, other_item_sizes_and_empty_data_are_left_alone)
{
    for (const std::size_t item_size : {0U, 1U, 3U, 16U}) {
        std::vector<std::byte> data = sequence(17);
        swap_byte_order(data, item_size);
        convert_big_endian(data, item_size);
        EXPECT_EQ(data, sequence(17)) << "items of " << item_size;

        std::vector<std::byte> target(17);
        copy_big_endian(sequence(17), target, item_size);
        EXPECT_EQ(target, sequence(17)) << "items of " << item_size;
    }

    std::vector<std::byte> empty;
    swap_byte_order(empty, 4);
    convert_big_endian(empty, 8);
    copy_big_endian({}, empty, 2);
    EXPECT_TRUE(empty.empty());
}

} // namespace
