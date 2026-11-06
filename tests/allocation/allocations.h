// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <cstdint>
#include <new>
#include <utility>

// The memory that code asks for, counted, and allocations made to fail, by the operator new of
// openfits_allocation_tests (allocations.cpp), which replaces that of the standard library in every test of that
// executable. Code that does not call it is neither counted nor failed: zlib, which calls malloc(), the library itself
// in a shared build on Windows, where it is a DLL with an operator new of its own, and on MinGW the members of the
// standard library that its DLL holds, such as those of std::string in libc++.dll. A count follows the blocks that this
// operator new allocated since it started, and a block that other code allocated is not taken from it when the
// executable frees it. On Linux and macOS this operator new replaces that of the shared libraries too, whose
// allocations it counts and fails: those of oneTBB, which starts its threads at the first parallel loop of a process.
// The internal code that the tests call directly is compiled into the test executable in every build.

namespace openfits::test {

/// What operator new did while it counted.
struct allocation_count
{
    /// The bytes that it was asked for.
    std::uint64_t requested = 0;
    /// The most bytes held at once by the blocks allocated since the count started, less those freed, as the C library
    /// measures its blocks; 0 where it cannot.
    std::int64_t peak = 0;
    /// The bytes held at the end of the count by the blocks allocated since it started, less those freed: what the
    /// code kept.
    std::int64_t kept = 0;
};

/// Makes operator new count from zero.
void start_counting_allocations() noexcept;

/// Stops the count, and returns it. A count that held more blocks at once than it follows, 786,432, gives the largest
/// values as peak and kept, which every bound on them refuses.
[[nodiscard]] allocation_count stop_counting_allocations() noexcept;

/// Makes the call of operator new that follows `index` others, from now on, throw std::bad_alloc, once.
void fail_allocation(std::uint64_t index) noexcept;

/// Stops failing, and returns whether an allocation failed since fail_allocation().
[[nodiscard]] bool stop_failing_allocations() noexcept;

/// Whether the allocations of the library reach this operator new, which they do not in a shared build on Windows.
[[nodiscard]] bool counts_the_allocations_of_the_library();

/// Whether the count measures the blocks that it holds (allocation_count::peak and kept).
[[nodiscard]] bool measures_blocks() noexcept;

/// Whether the allocations of the library can be made to fail: they reach this operator new, which they do not in a
/// shared build on Windows, and fail without ending the program, which they do not with the debug standard library of
/// MSVC, whose noexcept moves of containers allocate when iterators are checked.
[[nodiscard]] bool can_fail_allocations();

/// What operator new does while function runs, on every thread, which in the tests that use it is the calling thread
/// alone.
template <typename Function> [[nodiscard]] allocation_count allocations_of(Function&& function)
{
    start_counting_allocations();
    try {
        std::forward<Function>(function)();
    } catch (...) {
        (void)stop_counting_allocations();
        throw;
    }
    return stop_counting_allocations();
}

/// Calls function with its first allocation failing, then with its second failing, and so on, and calls check after
/// each failure, until a call makes fewer allocations than the one that would fail. Returns the number of allocations
/// made to fail.
template <typename Function, typename Check>
std::uint64_t fail_each_allocation(const Function& function, const Check& check)
{
    for (std::uint64_t index = 0;; ++index) {
        fail_allocation(index);
        try {
            function();
        } catch (const std::bad_alloc&) {
            if (!stop_failing_allocations()) {
                throw;
            }
            check();
            continue;
        } catch (...) {
            (void)stop_failing_allocations();
            throw;
        }
        if (!stop_failing_allocations()) {
            return index;
        }
        // The function went on after its allocation failed, which it may.
        check();
    }
}

} // namespace openfits::test
