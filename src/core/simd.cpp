// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "core/simd.h"

#include <array>

#if defined(OPENFITS_SIMD_SSE2) && defined(_MSC_VER)
#include <intrin.h>
#endif

namespace openfits::detail {

namespace {

#if defined(OPENFITS_SIMD_SSE2)

// CPUID leaf 1 reports OSXSAVE (ECX bit 27) and AVX (bit 28), XCR0 the state of the XMM and YMM registers that the
// system saves (bits 1 and 2), and CPUID leaf 7 AVX2 (EBX bit 5).
#if defined(__clang__) && defined(_MSC_VER)
__attribute__((target("xsave")))
#endif
bool query_avx2() noexcept
{
#if defined(_MSC_VER)
    std::array<int, 4> registers{};
    __cpuid(registers.data(), 0);
    if (registers[0] < 7) {
        return false;
    }
    __cpuid(registers.data(), 1);
    constexpr int osxsave_and_avx = (1 << 27) | (1 << 28);
    if ((registers[2] & osxsave_and_avx) != osxsave_and_avx || (_xgetbv(0) & 6U) != 6U) {
        return false;
    }
    __cpuidex(registers.data(), 7, 0);
    return (registers[1] & (1 << 5)) != 0;
#else
    // The check of the compiler's runtime, which looks at XCR0 too, and runs before the constructors of the program; it
    // is made again here for a call from one of them.
    __builtin_cpu_init();
    // An int with GCC, a bool with Clang.
    return static_cast<bool>(__builtin_cpu_supports("avx2"));
#endif
}

#endif

} // namespace

bool processor_has_avx2() noexcept
{
#if defined(OPENFITS_SIMD_SSE2)
    static const bool available = query_avx2();
    return available;
#else
    return false;
#endif
}

} // namespace openfits::detail
