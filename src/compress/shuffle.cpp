// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "compress/shuffle.h"

#include <openfits/error.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>

namespace openfits::detail {

namespace {

void check_sizes(std::span<const std::byte> input, std::span<std::byte> output)
{
    if (input.size() != output.size()) {
        throw usage_error(errc::invalid_argument, "byte shuffling needs an output of the size of its input");
    }
}

// The values of a block of the shuffles of 2 and 4 bytes.
constexpr std::size_t block_values = 512;

// The shift of byte j, counted from the most significant, of a value of Bytes bytes in an integer that memory holds in
// the byte order of the host.
template <std::size_t Bytes> constexpr unsigned shift_of(std::size_t j) noexcept
{
    return static_cast<unsigned>(8 * (std::endian::native == std::endian::little ? j : Bytes - 1 - j));
}

// The shuffles of values of 2 and 4 bytes, a value at a time, each value an integer in the byte order of the host, its
// bytes in runs of their own, through blocks on the stack: compilers vectorize such loops, which MSVC does neither
// over the bytes of each run, nor between the input and the output, which it cannot tell apart, nor over runs of one
// array.
using byte_run = std::array<std::uint8_t, block_values>;

void shuffle_2(const std::byte* input, std::byte* output, std::size_t values) noexcept
{
    // Written before they are read: zeros would cost a pass over them at each call.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-member-init)
    std::array<std::uint16_t, block_values> block;
    byte_run first_bytes;
    byte_run second_bytes;
    // NOLINTEND(cppcoreguidelines-pro-type-member-init)
    for (std::size_t first = 0; first < values; first += block_values) {
        const std::size_t count = std::min(block_values, values - first);
        std::memcpy(block.data(), input + (first * 2), count * 2);
        for (std::size_t i = 0; i < count; ++i) {
            first_bytes[i] = static_cast<std::uint8_t>((std::uint32_t{block[i]} >> shift_of<2>(0)) & 0xFFU);
            second_bytes[i] = static_cast<std::uint8_t>((std::uint32_t{block[i]} >> shift_of<2>(1)) & 0xFFU);
        }
        std::memcpy(output + first, first_bytes.data(), count);
        std::memcpy(output + values + first, second_bytes.data(), count);
    }
}

void unshuffle_2(const std::byte* input, std::byte* output, std::size_t values) noexcept
{
    // Written before they are read: zeros would cost a pass over them at each call.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-member-init)
    std::array<std::uint16_t, block_values> block;
    byte_run first_bytes;
    byte_run second_bytes;
    // NOLINTEND(cppcoreguidelines-pro-type-member-init)
    for (std::size_t first = 0; first < values; first += block_values) {
        const std::size_t count = std::min(block_values, values - first);
        std::memcpy(first_bytes.data(), input + first, count);
        std::memcpy(second_bytes.data(), input + values + first, count);
        for (std::size_t i = 0; i < count; ++i) {
            block[i] = static_cast<std::uint16_t>((std::uint32_t{first_bytes[i]} << shift_of<2>(0)) |
                                                  (std::uint32_t{second_bytes[i]} << shift_of<2>(1)));
        }
        std::memcpy(output + (first * 2), block.data(), count * 2);
    }
}

void shuffle_4(const std::byte* input, std::byte* output, std::size_t values) noexcept
{
    // Written before they are read: zeros would cost a pass over them at each call.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-member-init)
    std::array<std::uint32_t, block_values> block;
    byte_run byte0;
    byte_run byte1;
    byte_run byte2;
    byte_run byte3;
    // NOLINTEND(cppcoreguidelines-pro-type-member-init)
    for (std::size_t first = 0; first < values; first += block_values) {
        const std::size_t count = std::min(block_values, values - first);
        std::memcpy(block.data(), input + (first * 4), count * 4);
        for (std::size_t i = 0; i < count; ++i) {
            byte0[i] = static_cast<std::uint8_t>((block[i] >> shift_of<4>(0)) & 0xFFU);
            byte1[i] = static_cast<std::uint8_t>((block[i] >> shift_of<4>(1)) & 0xFFU);
            byte2[i] = static_cast<std::uint8_t>((block[i] >> shift_of<4>(2)) & 0xFFU);
            byte3[i] = static_cast<std::uint8_t>((block[i] >> shift_of<4>(3)) & 0xFFU);
        }
        std::memcpy(output + first, byte0.data(), count);
        std::memcpy(output + values + first, byte1.data(), count);
        std::memcpy(output + (2 * values) + first, byte2.data(), count);
        std::memcpy(output + (3 * values) + first, byte3.data(), count);
    }
}

void unshuffle_4(const std::byte* input, std::byte* output, std::size_t values) noexcept
{
    // Written before they are read: zeros would cost a pass over them at each call.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-member-init)
    std::array<std::uint32_t, block_values> block;
    byte_run byte0;
    byte_run byte1;
    byte_run byte2;
    byte_run byte3;
    // NOLINTEND(cppcoreguidelines-pro-type-member-init)
    for (std::size_t first = 0; first < values; first += block_values) {
        const std::size_t count = std::min(block_values, values - first);
        std::memcpy(byte0.data(), input + first, count);
        std::memcpy(byte1.data(), input + values + first, count);
        std::memcpy(byte2.data(), input + (2 * values) + first, count);
        std::memcpy(byte3.data(), input + (3 * values) + first, count);
        for (std::size_t i = 0; i < count; ++i) {
            block[i] = (std::uint32_t{byte0[i]} << shift_of<4>(0)) | (std::uint32_t{byte1[i]} << shift_of<4>(1)) |
                       (std::uint32_t{byte2[i]} << shift_of<4>(2)) | (std::uint32_t{byte3[i]} << shift_of<4>(3));
        }
        std::memcpy(output + (first * 4), block.data(), count * 4);
    }
}

} // namespace

// Byte j of value i is at i * value_size + j in the order of the values, and at j * values + i in the shuffled one.

void shuffle_bytes(std::span<const std::byte> input, std::span<std::byte> output, std::size_t value_size)
{
    check_sizes(input, output);
    const std::size_t values = value_size == 0 ? 0 : input.size() / value_size;
    if (value_size <= 1 || values == 0) {
        std::ranges::copy(input, output.begin());
        return;
    }
    if (value_size == 2) {
        shuffle_2(input.data(), output.data(), values);
    } else if (value_size == 4) {
        shuffle_4(input.data(), output.data(), values);
    } else {
        for (std::size_t j = 0; j < value_size; ++j) {
            std::byte* const run = output.data() + (j * values);
            for (std::size_t i = 0; i < values; ++i) {
                run[i] = input[(i * value_size) + j];
            }
        }
    }
    std::ranges::copy(input.subspan(values * value_size),
                      output.begin() + static_cast<std::ptrdiff_t>(values * value_size));
}

void unshuffle_bytes(std::span<const std::byte> input, std::span<std::byte> output, std::size_t value_size)
{
    check_sizes(input, output);
    const std::size_t values = value_size == 0 ? 0 : input.size() / value_size;
    if (value_size <= 1 || values == 0) {
        std::ranges::copy(input, output.begin());
        return;
    }
    if (value_size == 2) {
        unshuffle_2(input.data(), output.data(), values);
    } else if (value_size == 4) {
        unshuffle_4(input.data(), output.data(), values);
    } else {
        for (std::size_t j = 0; j < value_size; ++j) {
            const std::byte* const run = input.data() + (j * values);
            for (std::size_t i = 0; i < values; ++i) {
                output[(i * value_size) + j] = run[i];
            }
        }
    }
    std::ranges::copy(input.subspan(values * value_size),
                      output.begin() + static_cast<std::ptrdiff_t>(values * value_size));
}

} // namespace openfits::detail
