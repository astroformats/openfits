// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "allocation/allocations.h"

#include <openfits/card.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>
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

// The blocks that operator new allocated since the count started, with their bytes: a table of open addressing with
// linear probing, at most three quarters full, which operator new and operator delete hold under a spin lock while
// they count. A release counts only the blocks of the table. Code may free blocks that this operator new did not
// allocate: those allocated before the count started, and those that the standard library allocates in a DLL of its
// own, with the operator new of that DLL, and that code of the test executable frees, such as the buffers of the
// strings that the members of std::string compiled into libc++.dll build on MinGW, which a destructor inlined into the
// executable frees. Nothing that may allocate runs under the lock, which the allocation would wait for: a count that
// holds more blocks than the table is lost, and gives the largest values. The debug standard library of MSVC allocates
// a block more for each string and container.
struct tracked_block
{
    void* block = nullptr;
    std::int64_t bytes = 0;
};

constexpr std::size_t table_bits = 20;
constexpr std::size_t table_size = std::size_t{1} << table_bits;
constexpr std::size_t table_mask = table_size - 1;
constexpr std::size_t table_capacity = table_size / 4 * 3;
std::array<tracked_block, table_size> table{};
std::size_t tracked = 0;
bool lost = false;
std::atomic<bool> table_held{false};

// Holds the table, and the counts with it, while it lives.
class table_lock
{
public:
    table_lock() noexcept
    {
        while (table_held.exchange(true, std::memory_order_acquire)) {
        }
    }
    ~table_lock()
    {
        table_held.store(false, std::memory_order_release);
    }
    table_lock(const table_lock&) = delete;
    table_lock& operator=(const table_lock&) = delete;
};

// The slot where the search for a block starts: the bits of its address above the alignment of malloc(), mixed by a
// multiplication.
std::size_t home_of(const void* block) noexcept
{
    const std::size_t address = std::bit_cast<std::uintptr_t>(block) >> 4U;
    return (address * std::size_t{0x9E37'79B9'7F4A'7C15U}) >> (64U - table_bits);
}

// Puts a block in the table, and returns whether the table had room for it. A block that the table holds already was
// freed where this operator delete did not see it, by the standard library in its DLL: its bytes leave what is held.
bool track(void* block, std::int64_t bytes) noexcept
{
    std::size_t slot = home_of(block);
    while (table[slot].block != nullptr) {
        if (table[slot].block == block) {
            held.fetch_sub(table[slot].bytes, std::memory_order_relaxed);
            table[slot].bytes = bytes;
            return true;
        }
        slot = (slot + 1) & table_mask;
    }
    if (tracked == table_capacity) {
        return false;
    }
    table[slot] = {.block = block, .bytes = bytes};
    ++tracked;
    return true;
}

// Takes a block out of the table, and gives its bytes, or nothing when the table does not hold it.
std::optional<std::int64_t> untrack(const void* block) noexcept
{
    std::size_t slot = home_of(block);
    while (table[slot].block != block) {
        if (table[slot].block == nullptr) {
            return std::nullopt;
        }
        slot = (slot + 1) & table_mask;
    }
    const std::int64_t bytes = table[slot].bytes;
    // The blocks after it whose search passes its slot move back into the gap, so that every search still ends at the
    // first empty slot after the blocks it may find.
    std::size_t gap = slot;
    for (std::size_t next = (slot + 1) & table_mask; table[next].block != nullptr; next = (next + 1) & table_mask) {
        if (((next - home_of(table[next].block)) & table_mask) >= ((next - gap) & table_mask)) {
            table[gap] = table[next];
            gap = next;
        }
    }
    table[gap] = {};
    --tracked;
    return bytes;
}

void count_allocation(std::size_t size, void* block) noexcept
{
    const std::int64_t bytes = block_size(block);
    const table_lock lock;
    requested.fetch_add(size, std::memory_order_relaxed);
    if (!track(block, bytes)) {
        lost = true;
        return;
    }
    const std::int64_t now = held.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    if (now > peak.load(std::memory_order_relaxed)) {
        peak.store(now, std::memory_order_relaxed);
    }
}

void count_release(const void* block) noexcept
{
    const table_lock lock;
    if (const std::optional<std::int64_t> bytes = untrack(block)) {
        held.fetch_sub(*bytes, std::memory_order_relaxed);
    }
}

} // namespace

namespace openfits::test {

void start_counting_allocations() noexcept
{
    {
        const table_lock lock;
        // The blocks of the last count that were not freed while it counted.
        if (tracked != 0) {
            table.fill({});
            tracked = 0;
        }
        lost = false;
        requested.store(0, std::memory_order_relaxed);
        held.store(0, std::memory_order_relaxed);
        peak.store(0, std::memory_order_relaxed);
    }
    counting.store(true, std::memory_order_seq_cst);
}

allocation_count stop_counting_allocations() noexcept
{
    counting.store(false, std::memory_order_seq_cst);
    const table_lock lock;
    if (lost) {
        return {.requested = requested.load(std::memory_order_relaxed),
                .peak = std::numeric_limits<std::int64_t>::max(),
                .kept = std::numeric_limits<std::int64_t>::max()};
    }
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
        count_release(block);
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
