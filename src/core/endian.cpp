// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "core/endian.h"

#include "core/simd.h"

#include <cstdint>

namespace openfits::detail {

namespace {

// The items from first on of from, swapped into to, which may be the same memory: each item is read whole before it is
// written.
template <std::unsigned_integral T>
void swap_scalar(std::span<const std::byte> from, std::span<std::byte> to, std::size_t first,
                 std::uint64_t mask) noexcept
{
    T flips = 0;
    if constexpr (sizeof(T) == sizeof(std::uint64_t)) {
        flips = mask;
    } else {
        flips = static_cast<T>(mask);
    }
    const std::size_t count = from.size() / sizeof(T);
    for (std::size_t i = first; i < count; ++i) {
        T value = 0;
        std::memcpy(&value, from.data() + (i * sizeof(T)), sizeof(T));
        // Types narrower than unsigned are promoted to int, which the cast takes back.
        if constexpr (sizeof(T) < sizeof(unsigned)) {
            value = static_cast<T>(byte_swap(value) ^ flips);
        } else {
            value = byte_swap(value) ^ flips;
        }
        std::memcpy(to.data() + (i * sizeof(T)), &value, sizeof(T));
    }
}

#if defined(OPENFITS_SIMD_SSE2)

__m128i load(const std::byte* address) noexcept
{
    return _mm_loadu_si128(reinterpret_cast<const __m128i*>(address));
}

void store(std::byte* address, __m128i value) noexcept
{
    _mm_storeu_si128(reinterpret_cast<__m128i*>(address), value);
}

// The items of Size bytes of a register with their bytes reversed: the 16-bit words of each item of 4 bytes in reverse
// order, then the two bytes of each word.
template <std::size_t Size> __m128i reversed(__m128i value) noexcept
{
    if constexpr (Size == 4) {
        value = _mm_shufflehi_epi16(_mm_shufflelo_epi16(value, _MM_SHUFFLE(2, 3, 0, 1)), _MM_SHUFFLE(2, 3, 0, 1));
    }
    return _mm_or_si128(_mm_slli_epi16(value, 8), _mm_srli_epi16(value, 8));
}

// mask in every item of Size bytes of a register.
template <std::size_t Size> __m128i repeated(std::uint64_t mask) noexcept
{
    if constexpr (Size == 2) {
        return _mm_set1_epi16(static_cast<std::int16_t>(static_cast<std::uint16_t>(mask)));
    } else {
        return _mm_set1_epi32(static_cast<std::int32_t>(static_cast<std::uint32_t>(mask)));
    }
}

// swap_scalar() of the items of Size bytes, 2 or 4, of the whole registers that from holds, four registers at a time,
// all read before any is written. Returns the items swapped. Items of 8 bytes take the scalar code: SSE2 reverses them
// in five instructions a register, at 1.4 times the speed of the scalar code, where the kernels of 2 and 4 bytes must
// be 1.5 times faster to be kept.
template <std::size_t Size>
std::size_t swap_sse2(std::span<const std::byte> from, std::span<std::byte> to, std::uint64_t mask) noexcept
{
    const __m128i flips = repeated<Size>(mask);
    const std::size_t bytes = from.size();
    const std::byte* in = from.data();
    std::byte* out = to.data();
    std::size_t done = 0;
    for (; done + 64 <= bytes; done += 64) {
        const __m128i first = load(in + done);
        const __m128i second = load(in + done + 16);
        const __m128i third = load(in + done + 32);
        const __m128i fourth = load(in + done + 48);
        store(out + done, _mm_xor_si128(reversed<Size>(first), flips));
        store(out + done + 16, _mm_xor_si128(reversed<Size>(second), flips));
        store(out + done + 32, _mm_xor_si128(reversed<Size>(third), flips));
        store(out + done + 48, _mm_xor_si128(reversed<Size>(fourth), flips));
    }
    for (; done + 16 <= bytes; done += 16) {
        store(out + done, _mm_xor_si128(reversed<Size>(load(in + done)), flips));
    }
    return done / Size;
}

// The shuffle that reverses the bytes of each item of Size bytes of a register: a permutation within each of its two
// lanes of 16 bytes, which is all that the shuffle of AVX2 permutes across.
template <std::size_t Size> OPENFITS_TARGET_AVX2 __m256i reversal() noexcept
{
    if constexpr (Size == 2) {
        return _mm256_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14, 1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11,
                                10, 13, 12, 15, 14);
    } else if constexpr (Size == 4) {
        return _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9,
                                8, 15, 14, 13, 12);
    } else {
        return _mm256_setr_epi8(7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 15, 14,
                                13, 12, 11, 10, 9, 8);
    }
}

template <std::size_t Size> OPENFITS_TARGET_AVX2 __m256i repeated_wide(std::uint64_t mask) noexcept
{
    if constexpr (Size == 2) {
        return _mm256_set1_epi16(static_cast<std::int16_t>(static_cast<std::uint16_t>(mask)));
    } else if constexpr (Size == 4) {
        return _mm256_set1_epi32(static_cast<std::int32_t>(static_cast<std::uint32_t>(mask)));
    } else {
        return _mm256_set1_epi64x(static_cast<std::int64_t>(mask));
    }
}

// swap_sse2() with registers of 32 bytes, one shuffle each, for items of 2, 4 and 8 bytes.
template <std::size_t Size>
OPENFITS_TARGET_AVX2 std::size_t swap_avx2(std::span<const std::byte> from, std::span<std::byte> to,
                                           std::uint64_t mask) noexcept
{
    const __m256i pattern = reversal<Size>();
    const __m256i flips = repeated_wide<Size>(mask);
    const std::size_t bytes = from.size();
    // Registers at in and out, without lambdas, whose call operators would not have the target of this function.
    const auto* in = reinterpret_cast<const __m256i*>(from.data());
    auto* out = reinterpret_cast<__m256i*>(to.data());
    const std::size_t registers = bytes / 32;
    std::size_t done = 0;
    for (; done + 4 <= registers; done += 4) {
        const __m256i first = _mm256_loadu_si256(in + done);
        const __m256i second = _mm256_loadu_si256(in + done + 1);
        const __m256i third = _mm256_loadu_si256(in + done + 2);
        const __m256i fourth = _mm256_loadu_si256(in + done + 3);
        _mm256_storeu_si256(out + done, _mm256_xor_si256(_mm256_shuffle_epi8(first, pattern), flips));
        _mm256_storeu_si256(out + done + 1, _mm256_xor_si256(_mm256_shuffle_epi8(second, pattern), flips));
        _mm256_storeu_si256(out + done + 2, _mm256_xor_si256(_mm256_shuffle_epi8(third, pattern), flips));
        _mm256_storeu_si256(out + done + 3, _mm256_xor_si256(_mm256_shuffle_epi8(fourth, pattern), flips));
    }
    for (; done < registers; ++done) {
        _mm256_storeu_si256(out + done,
                            _mm256_xor_si256(_mm256_shuffle_epi8(_mm256_loadu_si256(in + done), pattern), flips));
    }
    // The code after it may use the registers of SSE, which pay a transition while the upper halves are in use.
    _mm256_zeroupper();
    return done * 32 / Size;
}

#endif

template <std::unsigned_integral T>
void swap_of_size(std::span<const std::byte> from, std::span<std::byte> to, std::uint64_t mask,
                  simd_kernel kernel) noexcept
{
    std::size_t first = 0;
#if defined(OPENFITS_SIMD_SSE2)
    if (kernel == simd_kernel::best && processor_has_avx2()) {
        first = swap_avx2<sizeof(T)>(from, to, mask);
    } else if constexpr (sizeof(T) < 8) {
        if (kernel != simd_kernel::scalar) {
            first = swap_sse2<sizeof(T)>(from, to, mask);
        }
    }
#else
    (void)kernel;
#endif
    swap_scalar<T>(from, to, first, mask);
}

// Swaps the items of from into to, which may be the same memory. Returns false for an item size that has no byte order
// to swap.
bool swap_into(std::span<const std::byte> from, std::span<std::byte> to, std::size_t item_size, std::uint64_t mask,
               simd_kernel kernel) noexcept
{
    switch (item_size) {
    case 2:
        swap_of_size<std::uint16_t>(from, to, mask, kernel);
        return true;
    case 4:
        swap_of_size<std::uint32_t>(from, to, mask, kernel);
        return true;
    case 8:
        swap_of_size<std::uint64_t>(from, to, mask, kernel);
        return true;
    default:
        return false;
    }
}

// The bytes of from from offset on, copied into to unless both are the same memory.
void copy_rest(std::span<const std::byte> from, std::span<std::byte> to, std::size_t offset) noexcept
{
    if (from.data() != to.data() && offset < from.size()) {
        std::memcpy(to.data() + offset, from.data() + offset, from.size() - offset);
    }
}

} // namespace

void swap_items(std::span<const std::byte> from, std::span<std::byte> to, std::size_t item_size, std::uint64_t mask,
                simd_kernel kernel) noexcept
{
    std::size_t swapped = 0;
    if (swap_into(from, to, item_size, mask, kernel)) {
        swapped = from.size() - (from.size() % item_size);
    }
    copy_rest(from, to, swapped);
}

void swap_byte_order(std::span<std::byte> data, std::size_t item_size, simd_kernel kernel) noexcept
{
    (void)swap_into(data, data, item_size, 0, kernel);
}

void convert_big_endian(std::span<std::byte> data, std::size_t item_size, simd_kernel kernel) noexcept
{
    if constexpr (std::endian::native == std::endian::little) {
        (void)swap_into(data, data, item_size, 0, kernel);
    }
}

void copy_big_endian(std::span<const std::byte> from, std::span<std::byte> to, std::size_t item_size,
                     simd_kernel kernel) noexcept
{
    if (from.empty()) {
        return;
    }
    std::size_t copied = 0;
    if constexpr (std::endian::native == std::endian::little) {
        if (swap_into(from, to, item_size, 0, kernel)) {
            copied = from.size() - (from.size() % item_size);
        }
    }
    // Everything on a big-endian host, and the bytes after the last complete item on a little-endian one.
    copy_rest(from, to, copied);
}

} // namespace openfits::detail
