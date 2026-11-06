// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "compress/shuffle.h"

#include <openfits/error.h>

#include "core/simd.h"

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

// The shuffle of values whole values of value_size bytes, 2 or more, in the portable code.
void shuffle_scalar(const std::byte* input, std::byte* output, std::size_t values, std::size_t value_size) noexcept
{
    if (value_size == 2) {
        shuffle_2(input, output, values);
    } else if (value_size == 4) {
        shuffle_4(input, output, values);
    } else {
        for (std::size_t j = 0; j < value_size; ++j) {
            std::byte* const run = output + (j * values);
            for (std::size_t i = 0; i < values; ++i) {
                run[i] = input[(i * value_size) + j];
            }
        }
    }
}

void unshuffle_scalar(const std::byte* input, std::byte* output, std::size_t values, std::size_t value_size) noexcept
{
    if (value_size == 2) {
        unshuffle_2(input, output, values);
    } else if (value_size == 4) {
        unshuffle_4(input, output, values);
    } else {
        for (std::size_t j = 0; j < value_size; ++j) {
            const std::byte* const run = input + (j * values);
            for (std::size_t i = 0; i < values; ++i) {
                output[(i * value_size) + j] = run[i];
            }
        }
    }
}

#if defined(OPENFITS_SIMD_SSE2)

// The kernels move 16 values at a time, as many as a register holds bytes.
constexpr std::size_t lanes = 16;

__m128i load(const std::byte* address) noexcept
{
    return _mm_loadu_si128(reinterpret_cast<const __m128i*>(address));
}

void store(std::byte* address, __m128i value) noexcept
{
    _mm_storeu_si128(reinterpret_cast<__m128i*>(address), value);
}

// The first byte of each of the 16 values of Size bytes at values, packed into one register: the bytes above it are
// cleared in each value, and the values are packed with saturation, which leaves values below 256 unchanged.
template <std::size_t Size> __m128i first_bytes(const std::byte* values) noexcept
{
    if constexpr (Size == 2) {
        const __m128i mask = _mm_set1_epi16(0xFF);
        return _mm_packus_epi16(_mm_and_si128(load(values), mask), _mm_and_si128(load(values + 16), mask));
    } else if constexpr (Size == 4) {
        const __m128i mask = _mm_set1_epi32(0xFF);
        const auto pack = [&](std::size_t at) {
            return _mm_packs_epi32(_mm_and_si128(load(values + at), mask), _mm_and_si128(load(values + at + 16), mask));
        };
        return _mm_packus_epi16(pack(0), pack(32));
    } else {
        static_assert(Size == 8);
        const __m128i mask = _mm_set1_epi64x(0xFF);
        // The two values of a register, as its two lower 32-bit lanes.
        const auto pair = [&](std::size_t at) {
            return _mm_shuffle_epi32(_mm_and_si128(load(values + at), mask), _MM_SHUFFLE(2, 0, 2, 0));
        };
        const auto pack = [&](std::size_t at) {
            return _mm_packs_epi32(_mm_unpacklo_epi64(pair(at), pair(at + 16)),
                                   _mm_unpacklo_epi64(pair(at + 32), pair(at + 48)));
        };
        return _mm_packus_epi16(pack(0), pack(64));
    }
}

// Run j of the shuffle of count values of Size bytes, from the values from byte j of the first one on, 16 values at a
// time while the loads stay within the input, which ends at end.
template <std::size_t Size>
void gather_sse2(const std::byte* from, const std::byte* end, std::byte* run, std::size_t count) noexcept
{
    std::size_t k = 0;
    for (; k + lanes <= count && (k + lanes) * Size <= static_cast<std::size_t>(end - from); k += lanes) {
        store(run + k, first_bytes<Size>(from + (k * Size)));
    }
    for (; k < count; ++k) {
        run[k] = from[k * Size];
    }
}

// Interleaves 16 values from the runs, whose first bytes are at run, run + values, run + 2 x values and so on, into the
// 16 values at out: unpacking pairs of runs gives values of 2 bytes, pairs of those values of 4 bytes, and pairs of
// those values of 8 bytes. The low halves of the unpacked registers hold values 0 to 7, the high halves values 8 to 15.
template <std::size_t Size> void interleave(const std::byte* run, std::size_t values, std::byte* out) noexcept
{
    const auto load_run = [run, values](std::size_t j) { return load(run + (j * values)); };
    if constexpr (Size == 2) {
        const __m128i bytes0 = load_run(0);
        const __m128i bytes1 = load_run(1);
        store(out, _mm_unpacklo_epi8(bytes0, bytes1));
        store(out + 16, _mm_unpackhi_epi8(bytes0, bytes1));
    } else if constexpr (Size == 4) {
        const __m128i bytes0 = load_run(0);
        const __m128i bytes1 = load_run(1);
        const __m128i bytes2 = load_run(2);
        const __m128i bytes3 = load_run(3);
        const __m128i low01 = _mm_unpacklo_epi8(bytes0, bytes1);
        const __m128i high01 = _mm_unpackhi_epi8(bytes0, bytes1);
        const __m128i low23 = _mm_unpacklo_epi8(bytes2, bytes3);
        const __m128i high23 = _mm_unpackhi_epi8(bytes2, bytes3);
        store(out, _mm_unpacklo_epi16(low01, low23));
        store(out + 16, _mm_unpackhi_epi16(low01, low23));
        store(out + 32, _mm_unpacklo_epi16(high01, high23));
        store(out + 48, _mm_unpackhi_epi16(high01, high23));
    } else {
        static_assert(Size == 8);
        const __m128i bytes0 = load_run(0);
        const __m128i bytes1 = load_run(1);
        const __m128i bytes2 = load_run(2);
        const __m128i bytes3 = load_run(3);
        const __m128i bytes4 = load_run(4);
        const __m128i bytes5 = load_run(5);
        const __m128i bytes6 = load_run(6);
        const __m128i bytes7 = load_run(7);
        // The 8 values of a half from its pairs of bytes: bytes 0 to 3 of values 0 to 3 and of values 4 to 7, then
        // bytes 4 to 7 of the same values.
        const auto store_half = [](__m128i bytes01, __m128i bytes23, __m128i bytes45, __m128i bytes67,
                                   std::byte* half) {
            const __m128i start0 = _mm_unpacklo_epi16(bytes01, bytes23);
            const __m128i start4 = _mm_unpackhi_epi16(bytes01, bytes23);
            const __m128i end0 = _mm_unpacklo_epi16(bytes45, bytes67);
            const __m128i end4 = _mm_unpackhi_epi16(bytes45, bytes67);
            store(half, _mm_unpacklo_epi32(start0, end0));
            store(half + 16, _mm_unpackhi_epi32(start0, end0));
            store(half + 32, _mm_unpacklo_epi32(start4, end4));
            store(half + 48, _mm_unpackhi_epi32(start4, end4));
        };
        store_half(_mm_unpacklo_epi8(bytes0, bytes1), _mm_unpacklo_epi8(bytes2, bytes3),
                   _mm_unpacklo_epi8(bytes4, bytes5), _mm_unpacklo_epi8(bytes6, bytes7), out);
        store_half(_mm_unpackhi_epi8(bytes0, bytes1), _mm_unpackhi_epi8(bytes2, bytes3),
                   _mm_unpackhi_epi8(bytes4, bytes5), _mm_unpackhi_epi8(bytes6, bytes7), out + 64);
    }
}

// Both runs of values of 2 bytes from one load of each 16 values: the bytes of a value are the low and the high byte
// of a 16-bit lane, which a mask and a shift give, and the 16 values of two registers pack into one.
void shuffle_pairs_sse2(const std::byte* input, std::byte* output, std::size_t values) noexcept
{
    const __m128i mask = _mm_set1_epi16(0xFF);
    std::size_t k = 0;
    for (; k + lanes <= values; k += lanes) {
        const __m128i first = load(input + (2 * k));
        const __m128i second = load(input + (2 * k) + 16);
        store(output + k, _mm_packus_epi16(_mm_and_si128(first, mask), _mm_and_si128(second, mask)));
        store(output + values + k, _mm_packus_epi16(_mm_srli_epi16(first, 8), _mm_srli_epi16(second, 8)));
    }
    for (; k < values; ++k) {
        output[k] = input[2 * k];
        output[values + k] = input[(2 * k) + 1];
    }
}

template <std::size_t Size> void shuffle_sse2(const std::byte* input, std::byte* output, std::size_t values) noexcept
{
    if constexpr (Size == 2) {
        shuffle_pairs_sse2(input, output, values);
    } else {
        const std::byte* const end = input + (values * Size);
        for (std::size_t j = 0; j < Size; ++j) {
            gather_sse2<Size>(input + j, end, output + (j * values), values);
        }
    }
}

template <std::size_t Size> void unshuffle_sse2(const std::byte* input, std::byte* output, std::size_t values) noexcept
{
    std::size_t i = 0;
    for (; i + lanes <= values; i += lanes) {
        interleave<Size>(input + i, values, output + (i * Size));
    }
    for (std::size_t j = 0; j < Size; ++j) {
        for (std::size_t k = i; k < values; ++k) {
            output[(k * Size) + j] = input[(j * values) + k];
        }
    }
}

#endif

// The SIMD kernel of values of value_size bytes, when the build has one: false otherwise.
bool shuffle_simd(const std::byte* input, std::byte* output, std::size_t values, std::size_t value_size,
                  bool reverse) noexcept
{
#if defined(OPENFITS_SIMD_SSE2)
    switch (value_size) {
    case 2:
        reverse ? unshuffle_sse2<2>(input, output, values) : shuffle_sse2<2>(input, output, values);
        return true;
    case 4:
        reverse ? unshuffle_sse2<4>(input, output, values) : shuffle_sse2<4>(input, output, values);
        return true;
    case 8:
        reverse ? unshuffle_sse2<8>(input, output, values) : shuffle_sse2<8>(input, output, values);
        return true;
    default:
        return false;
    }
#else
    (void)input;
    (void)output;
    (void)values;
    (void)value_size;
    (void)reverse;
    return false;
#endif
}

} // namespace

// Byte j of value i is at i * value_size + j in the order of the values, and at j * values + i in the shuffled one.

void shuffle_bytes(std::span<const std::byte> input, std::span<std::byte> output, std::size_t value_size,
                   simd_kernel kernel)
{
    check_sizes(input, output);
    const std::size_t values = value_size == 0 ? 0 : input.size() / value_size;
    if (value_size <= 1 || values == 0) {
        std::ranges::copy(input, output.begin());
        return;
    }
    if (kernel == simd_kernel::scalar || !shuffle_simd(input.data(), output.data(), values, value_size, false)) {
        shuffle_scalar(input.data(), output.data(), values, value_size);
    }
    std::ranges::copy(input.subspan(values * value_size),
                      output.begin() + static_cast<std::ptrdiff_t>(values * value_size));
}

void unshuffle_bytes(std::span<const std::byte> input, std::span<std::byte> output, std::size_t value_size,
                     simd_kernel kernel)
{
    check_sizes(input, output);
    const std::size_t values = value_size == 0 ? 0 : input.size() / value_size;
    if (value_size <= 1 || values == 0) {
        std::ranges::copy(input, output.begin());
        return;
    }
    if (kernel == simd_kernel::scalar || !shuffle_simd(input.data(), output.data(), values, value_size, true)) {
        unshuffle_scalar(input.data(), output.data(), values, value_size);
    }
    std::ranges::copy(input.subspan(values * value_size),
                      output.begin() + static_cast<std::ptrdiff_t>(values * value_size));
}

} // namespace openfits::detail
