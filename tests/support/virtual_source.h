// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/error.h>
#include <openfits/io.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace openfits::test {

/// A source as large as a test needs, beyond 4 GiB without disk space or memory: its first bytes are given, such as the
/// header of a unit, and each byte after them is a function of its offset.
class virtual_source final : public input_source
{
public:
    using generator = std::function<std::byte(std::uint64_t offset)>;

    virtual_source(std::vector<std::byte> prefix, std::uint64_t size, generator bytes)
        : prefix_(std::move(prefix)), size_(size), bytes_(std::move(bytes))
    {}

    std::uint64_t size() const override
    {
        return size_;
    }

    void read(std::uint64_t offset, std::span<std::byte> destination) const override
    {
        if (offset > size_ || destination.size() > size_ - offset) {
            throw io_error(errc::end_of_data, "a read beyond the end of the virtual source");
        }
        for (std::size_t i = 0; i < destination.size(); ++i) {
            const std::uint64_t at = offset + i;
            destination[i] = at < prefix_.size() ? prefix_[at] : bytes_(at);
        }
    }

    bool supports_concurrent_reads() const override
    {
        return true;
    }

private:
    std::vector<std::byte> prefix_;
    std::uint64_t size_;
    generator bytes_;
};

} // namespace openfits::test
