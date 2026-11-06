// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "allocation/allocations.h"

#include <openfits/card.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string>

#if defined(_WIN32)
#include <malloc.h>
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__linux__) || defined(__CYGWIN__)
#include <malloc.h>
#endif

namespace {

// Whether operator new counts, and what it has counted; whether it fails, the allocations before the one that fails,
// and whether one failed. They are constants until a test counts or fails, so that operator new can read them from
// the first static initializer on; a thread_local, read that early, crashed Clang 15 on MinGW.
std::atomic<bool> counting{false};
std::atomic<std::uint64_t> requested{0};
std::atomic<std::int64_t> held{0};
std::atomic<std::int64_t> peak{0};
std::atomic<bool> failing{false};
std::atomic<std::uint64_t> before_failure{0};
std::atomic<bool> failed{false};

// The bytes of a block of malloc(), as the C library measures it, which may be more than were asked for; 0 where the
// library does not tell. The same measure on allocation and on release makes what is held exact to the library.
std::int64_t block_size(void* block) noexcept
{
#if defined(_WIN32)
    return static_cast<std::int64_t>(_msize(block));
#elif defined(__APPLE__)
    return static_cast<std::int64_t>(malloc_size(block));
#elif defined(__linux__) || defined(__CYGWIN__)
    return static_cast<std::int64_t>(malloc_usable_size(block));
#else
    (void)block;
    return 0;
#endif
}

void count_allocation(std::size_t size, void* block) noexcept
{
    requested.fetch_add(size, std::memory_order_relaxed);
    const std::int64_t bytes = block_size(block);
    const std::int64_t now = held.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    // The highest count so far, which another thread may raise at the same time.
    std::int64_t highest = peak.load(std::memory_order_relaxed);
    while (now > highest) {
        if (peak.compare_exchange_weak(highest, now, std::memory_order_relaxed)) {
            break;
        }
    }
}

} // namespace

namespace openfits::test {

void start_counting_allocations() noexcept
{
    requested.store(0, std::memory_order_relaxed);
    held.store(0, std::memory_order_relaxed);
    peak.store(0, std::memory_order_relaxed);
    counting.store(true, std::memory_order_seq_cst);
}

allocation_count stop_counting_allocations() noexcept
{
    counting.store(false, std::memory_order_seq_cst);
    return {.requested = requested.load(std::memory_order_relaxed),
            .peak = peak.load(std::memory_order_relaxed),
            .kept = held.load(std::memory_order_relaxed)};
}

void fail_allocation(std::uint64_t index) noexcept
{
    before_failure.store(index, std::memory_order_relaxed);
    failed.store(false, std::memory_order_relaxed);
    failing.store(true, std::memory_order_seq_cst);
}

bool stop_failing_allocations() noexcept
{
    failing.store(false, std::memory_order_seq_cst);
    return failed.load(std::memory_order_relaxed);
}

bool counts_the_allocations_of_the_library()
{
    // A field of 256 characters, which the library renders into a string that it allocates.
    const display_format format{.code = display_code::character, .width = 256};
    std::string text;
    const allocation_count count = allocations_of([&format, &text] { text = format_display(format, "a"); });
    return count.requested != 0;
}

bool can_fail_allocations()
{
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
    return false;
#else
    return counts_the_allocations_of_the_library();
#endif
}

bool measures_blocks() noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    void* block = std::malloc(64);
    const bool measured = block != nullptr && block_size(block) != 0;
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    std::free(block);
    return measured;
}

} // namespace openfits::test

// The replaceable allocation functions of the test executable. The memory comes from malloc(), as that of the standard
// libraries does, so that a block allocated on one side of a DLL, such as that of GoogleTest, and freed on the other is
// still freed by the function that matches. The array and nothrow forms are replaced too, though those of the standard
// libraries call these, since a sanitizer brings forms of its own: the nothrow operator new of AddressSanitizer, which
// GoogleTest calls as it runs a suite, gave memory of its allocator to the free() of these. They carry none of the SAL
// annotations of the declarations of MSVC's runtime, which its analyzer compares (C28251), and do as those say.
#if defined(_MSC_VER)
#pragma warning(disable : 28251)
#endif

void* operator new(std::size_t size)
{
    if (failing.load(std::memory_order_relaxed)) {
        if (before_failure.load(std::memory_order_relaxed) == 0) {
            failing.store(false, std::memory_order_relaxed);
            failed.store(true, std::memory_order_relaxed);
            throw std::bad_alloc();
        }
        before_failure.fetch_sub(1, std::memory_order_relaxed);
    }
    // An allocation function gets its memory from malloc().
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    if (void* block = std::malloc(size == 0 ? 1 : size)) {
        if (counting.load(std::memory_order_relaxed)) {
            count_allocation(size, block);
        }
        return block;
    }
    throw std::bad_alloc();
}

void operator delete(void* block) noexcept
{
    if (block != nullptr && counting.load(std::memory_order_relaxed)) {
        held.fetch_sub(block_size(block), std::memory_order_relaxed);
    }
    // The memory of operator new comes from malloc().
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    std::free(block);
}

void operator delete(void* block, std::size_t /*size*/) noexcept
{
    ::operator delete(block);
}

void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept
{
    try {
        return ::operator new(size);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept
{
    return ::operator new(size, tag);
}

void operator delete(void* block, const std::nothrow_t& /*tag*/) noexcept
{
    ::operator delete(block);
}

void operator delete[](void* block) noexcept
{
    ::operator delete(block);
}

void operator delete[](void* block, std::size_t /*size*/) noexcept
{
    ::operator delete(block);
}

void operator delete[](void* block, const std::nothrow_t& /*tag*/) noexcept
{
    ::operator delete(block);
}
