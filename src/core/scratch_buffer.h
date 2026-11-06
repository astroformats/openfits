// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <cstddef>
#include <memory>
#include <span>

namespace openfits::detail {

/// The size of the staging chunks that the reads and writes of the library allocate for a call, 448 KiB: below the
/// blocks of 512 KiB or more that the heap of Windows takes from the system afresh at each allocation, whose page
/// faults a chunk of megabytes pays at each call, which made the read of a region of a camera frame twice as slow, and
/// the verification of an image of 1024 x 1024 pixels four times.
inline constexpr std::size_t staging_size = std::size_t{448} << 10;

/// Memory for bytes that are written completely before they are read, such as a staging chunk of a read or a write.
/// Unlike a std::vector, it is not filled with zeros first, which for a chunk of megabytes costs about as much as the
/// copy that follows.
// The array form of std::unique_ptr is how the standard library allocates bytes without initializing them; a vector
// would need a custom allocator.
// NOLINTBEGIN(modernize-avoid-c-arrays)
class scratch_buffer
{
public:
    explicit scratch_buffer(std::size_t size) : data_(std::make_unique_for_overwrite<std::byte[]>(size)), size_(size) {}

    [[nodiscard]] std::span<std::byte> bytes() noexcept
    {
        return {data_.get(), size_};
    }

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept
    {
        return {data_.get(), size_};
    }

private:
    std::unique_ptr<std::byte[]> data_;
    std::size_t size_;
};
// NOLINTEND(modernize-avoid-c-arrays)

} // namespace openfits::detail
