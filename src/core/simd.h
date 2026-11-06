// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

// The instruction sets of the SIMD kernels. SSE2 is part of every x86-64 processor, so the kernels of SSE2 need no
// dispatch, and are built when OPENFITS_ENABLE_SIMD is on (OPENFITS_SIMD_SSE2). AVX2 is not: its kernels are compiled
// for it alone, with OPENFITS_TARGET_AVX2 on each of their functions, the target attribute of GCC and Clang, whose
// other functions keep the instruction set of the build (MSVC needs none for intrinsics), and run where
// processor_has_avx2() says so. ARM64EC defines _M_X64 too, but emulates SSE.

#if defined(OPENFITS_ENABLE_SIMD) && (defined(__SSE2__) || defined(_M_X64)) && !defined(_M_ARM64EC)
#define OPENFITS_SIMD_SSE2
#include <immintrin.h>
#if defined(__GNUC__) || defined(__clang__)
#define OPENFITS_TARGET_AVX2 __attribute__((target("avx2")))
#else
#define OPENFITS_TARGET_AVX2
#endif
#endif

#include <cstdint>

namespace openfits::detail {

/// The code that a kernel with SIMD forms runs. Each gives the same results; the tests and the benchmarks compare them,
/// and the library runs the best.
enum class simd_kernel : std::uint8_t
{
    /// Portable code, which compilers may vectorize.
    scalar,
    /// The instruction set that every processor of the architecture has, SSE2 on x86-64, where the kernel has a form
    /// for it; the scalar code otherwise.
    baseline,
    /// The widest instruction set of the processor that the kernel has a form for, AVX2 on x86-64; the baseline
    /// otherwise.
    best,
};

/// True when the build has the kernels of the baseline instruction set.
[[nodiscard]] constexpr bool has_baseline_kernels() noexcept
{
#if defined(OPENFITS_SIMD_SSE2)
    return true;
#else
    return false;
#endif
}

/// True when the build has the kernels of AVX2, the processor has AVX2 and the system saves its registers; found at
/// the first call.
[[nodiscard]] bool processor_has_avx2() noexcept;

} // namespace openfits::detail
