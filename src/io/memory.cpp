// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>
#include <openfits/io.h>

#include "core/checked_math.h"
#include "io/range_check.h"

#include <algorithm>
#include <new>
#include <string>
#include <utility>

namespace openfits {

memory_source::memory_source(std::span<const std::byte> data) noexcept : data_(data) {}

memory_source::memory_source(std::vector<std::byte> data) noexcept : owned_(std::move(data)), data_(owned_) {}

std::uint64_t memory_source::size() const
{
    return data_.size();
}

void memory_source::read(std::uint64_t offset, std::span<std::byte> destination) const
{
    detail::check_read_range(data_.size(), offset, destination.size(), {});
    std::ranges::copy(data_.subspan(detail::checked_cast<std::size_t>(offset), destination.size()),
                      destination.begin());
}

bool memory_source::supports_concurrent_reads() const
{
    return true;
}

std::span<const std::byte> memory_source::bytes() const
{
    return data_;
}

void memory_sink::write(std::span<const std::byte> data)
{
    data_.insert(data_.end(), data.begin(), data.end());
}

std::uint64_t memory_sink::position() const
{
    return data_.size();
}

bool memory_sink::can_rewrite() const
{
    return true;
}

void memory_sink::rewrite(std::uint64_t offset, std::span<const std::byte> data)
{
    detail::check_rewrite_range(data_.size(), offset, data.size());
    std::ranges::copy(data, data_.begin() + static_cast<std::ptrdiff_t>(offset));
}

std::span<const std::byte> memory_sink::data() const noexcept
{
    return data_;
}

std::vector<std::byte> memory_sink::release() noexcept
{
    return std::exchange(data_, {});
}

void memory_sink::reserve(std::uint64_t bytes)
{
    if (bytes > data_.max_size()) {
        throw limit_error(errc::allocation_too_large,
                          "a memory sink cannot hold " + std::to_string(bytes) + " bytes in memory");
    }
    data_.reserve(bytes);
}

memory_device::memory_device(std::span<std::byte> data) noexcept : data_(data) {}

memory_device::memory_device(std::vector<std::byte>&& data) noexcept
    : owned_(std::move(data)), data_(owned_), owns_(true)
{}

std::uint64_t memory_device::size() const
{
    return data_.size();
}

void memory_device::read(std::uint64_t offset, std::span<std::byte> destination) const
{
    detail::check_read_range(data_.size(), offset, destination.size(), {});
    std::ranges::copy(data_.subspan(detail::checked_cast<std::size_t>(offset), destination.size()),
                      destination.begin());
}

void memory_device::write(std::uint64_t offset, std::span<const std::byte> data)
{
    detail::check_write_range(data_.size(), offset, data.size(), {});
    std::ranges::copy(data, data_.begin() + static_cast<std::ptrdiff_t>(offset));
}

bool memory_device::can_resize() const
{
    return owns_;
}

void memory_device::resize(std::uint64_t size)
{
    if (!owns_) {
        throw usage_error(errc::invalid_argument, "a memory device that borrows its bytes cannot change its size");
    }
    // Beyond what a vector can hold, no memory can hold it either.
    if (size > owned_.max_size()) {
        throw std::bad_alloc();
    }
    owned_.resize(size);
    data_ = owned_;
}

bool memory_device::supports_concurrent_reads() const
{
    return true;
}

bool memory_device::supports_concurrent_writes() const
{
    return true;
}

std::span<const std::byte> memory_device::data() const noexcept
{
    return data_;
}

std::vector<std::byte> memory_device::release() noexcept
{
    if (!owns_) {
        return {};
    }
    data_ = {};
    return std::exchange(owned_, {});
}

} // namespace openfits
