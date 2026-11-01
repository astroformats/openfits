// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/error.h>
#include <openfits/io.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace openfits::test {

/// One read that a counting_source served.
struct served_read
{
    std::uint64_t offset = 0;
    std::uint64_t size = 0;

    friend bool operator==(const served_read&, const served_read&) = default;
};

/// A source over bytes in memory that records the reads it serves, so that tests assert how many reads an operation
/// costs. It gives no bytes in memory, so that the library reads it through read().
class counting_source final : public input_source
{
public:
    explicit counting_source(std::vector<std::byte> data, bool concurrent = false)
        : data_(std::move(data)), concurrent_(concurrent)
    {}

    std::uint64_t size() const override
    {
        return data_.size();
    }

    void read(std::uint64_t offset, std::span<std::byte> destination) const override
    {
        if (offset > data_.size() || destination.size() > data_.size() - offset) {
            throw io_error(errc::end_of_data, "a read beyond the end of the counting source");
        }
        std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(offset), destination.size(), destination.begin());
        const std::scoped_lock lock(mutex_);
        reads_.push_back({.offset = offset, .size = destination.size()});
    }

    bool supports_concurrent_reads() const override
    {
        return concurrent_;
    }

    /// The reads served so far, in their order.
    [[nodiscard]] std::vector<served_read> reads() const
    {
        const std::scoped_lock lock(mutex_);
        return reads_;
    }

    /// The bytes read so far.
    [[nodiscard]] std::uint64_t bytes_read() const
    {
        const std::scoped_lock lock(mutex_);
        std::uint64_t total = 0;
        for (const served_read& served : reads_) {
            total += served.size;
        }
        return total;
    }

private:
    std::vector<std::byte> data_;
    bool concurrent_;
    mutable std::mutex mutex_;
    mutable std::vector<served_read> reads_;
};

} // namespace openfits::test
