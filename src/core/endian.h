// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

#if defined(_MSC_VER) && !defined(__clang__)
#include <stdlib.h>
#endif

#include "core/simd.h"

// Byte order conversions. FITS data are big-endian (standard §5.2, §5.3): a little-endian host swaps the bytes of every
// item it reads or writes, and a big-endian host copies them. Nothing here assumes the byte order of the host.

namespace openfits::detail {

static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big,
              "mixed-endian hosts are not supported");

/// byte_swap() in portable code, for constant evaluation and other compilers.
template <std::unsigned_integral T> [[nodiscard]] constexpr T portable_byte_swap(T value) noexcept
{
    // In 64 bits, so that integer promotion does not change the type, and no cast is needed when T is 64 bits wide.
    std::uint64_t swapped = 0;
    std::uint64_t remaining = value;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        swapped = (swapped << 8U) | (remaining & 0xFFU);
        remaining >>= 8U;
    }
    if constexpr (sizeof(T) == sizeof(std::uint64_t)) {
        return swapped;
    } else {
        return static_cast<T>(swapped);
    }
}

/// value with its bytes in reverse order (std::byteswap is C++23). Outside constant evaluation, the compiler's own
/// swap, which its vectorizer knows: MSVC leaves every loop over the portable form scalar.
template <std::unsigned_integral T> [[nodiscard]] constexpr T byte_swap(T value) noexcept
{
    if constexpr (sizeof(T) == 1) {
        return value;
    } else {
        if (!std::is_constant_evaluated()) {
#if defined(__GNUC__) || defined(__clang__)
            if constexpr (sizeof(T) == 2) {
                return __builtin_bswap16(value);
            } else if constexpr (sizeof(T) == 4) {
                return __builtin_bswap32(value);
            } else if constexpr (sizeof(T) == 8) {
                return __builtin_bswap64(value);
            }
#elif defined(_MSC_VER)
            if constexpr (sizeof(T) == 2) {
                return _byteswap_ushort(value);
            } else if constexpr (sizeof(T) == 4) {
                return static_cast<T>(_byteswap_ulong(value));
            } else if constexpr (sizeof(T) == 8) {
                return _byteswap_uint64(value);
            }
#endif
        }
        return portable_byte_swap(value);
    }
}

/// Loads an unsigned integer stored in big-endian byte order.
template <std::unsigned_integral T>
[[nodiscard]] T load_big_endian(std::span<const std::byte, sizeof(T)> bytes) noexcept
{
    T value = 0;
    std::memcpy(&value, bytes.data(), sizeof(T));
    if constexpr (std::endian::native == std::endian::little) {
        value = byte_swap(value);
    }
    return value;
}

/// Stores an unsigned integer in big-endian byte order.
template <std::unsigned_integral T> void store_big_endian(std::span<std::byte, sizeof(T)> bytes, T value) noexcept
{
    if constexpr (std::endian::native == std::endian::little) {
        value = byte_swap(value);
    }
    std::memcpy(bytes.data(), &value, sizeof(T));
}

/// Writes the items of item_size bytes, 2, 4 or 8, of from into to, which has its size, with their bytes reversed and
/// then the bits of mask flipped, as unsigned integers of that size in the byte order of the host: to[i] =
/// byte_swap(from[i]) ^ mask, the sign bit of a value converted between big-endian and native order given in mask
/// for the offset of standard §5.2.5. from and to are the same memory or do not overlap. The bytes after the last
/// complete item are copied as they are, and so is everything for items of other sizes. The kernels: SSE2 for items of
/// 2 and 4 bytes, where it is faster than the scalar code, and AVX2 for items of 2, 4 and 8 bytes.
void swap_items(std::span<const std::byte> from, std::span<std::byte> to, std::size_t item_size, std::uint64_t mask = 0,
                simd_kernel kernel = simd_kernel::best) noexcept;

/// Reverses the byte order of every item of item_size bytes, 2, 4 or 8, in place. Bytes after the last complete item
/// are left unchanged; items of other sizes are left unchanged.
void swap_byte_order(std::span<std::byte> data, std::size_t item_size, simd_kernel kernel = simd_kernel::best) noexcept;

/// Converts items of item_size bytes, 2, 4 or 8, between big-endian and native byte order, in place: swaps them on a
/// little-endian host and leaves them on a big-endian one. The conversion is its own inverse.
void convert_big_endian(std::span<std::byte> data, std::size_t item_size,
                        simd_kernel kernel = simd_kernel::best) noexcept;

/// Copies items of item_size bytes from one byte order to the other, between big-endian and native: from and to have
/// the same size and do not overlap. Items of 2, 4 and 8 bytes are converted as convert_big_endian() converts them, and
/// everything else is copied as it is. The writer converts the caller's data this way, without modifying them.
void copy_big_endian(std::span<const std::byte> from, std::span<std::byte> to, std::size_t item_size,
                     simd_kernel kernel = simd_kernel::best) noexcept;

} // namespace openfits::detail
