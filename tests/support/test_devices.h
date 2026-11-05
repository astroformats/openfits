// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/error.h>
#include <openfits/io.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

// Devices for the tests of the editor: one that counts its calls, and one as large as a test needs without the memory.

namespace openfits::test {

/// A device over bytes in memory, which it owns and resizes, that counts the reads and writes it serves, so that tests
/// assert what an edit costs.
class counting_device final : public random_access_device
{
public:
    explicit counting_device(std::vector<std::byte> data) : data_(std::move(data)) {}

    std::uint64_t size() const override
    {
        return data_.size();
    }

    void read(std::uint64_t offset, std::span<std::byte> destination) const override
    {
        if (offset > data_.size() || destination.size() > data_.size() - offset) {
            throw io_error(errc::end_of_data, "a read beyond the end of the counting device");
        }
        std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(offset), destination.size(), destination.begin());
        const std::scoped_lock lock(mutex_);
        ++reads_;
    }

    void write(std::uint64_t offset, std::span<const std::byte> data) override
    {
        if (offset > data_.size() || data.size() > data_.size() - offset) {
            throw usage_error(errc::invalid_argument, "a write beyond the end of the counting device");
        }
        std::ranges::copy(data, data_.begin() + static_cast<std::ptrdiff_t>(offset));
        const std::scoped_lock lock(mutex_);
        ++writes_;
    }

    bool can_resize() const override
    {
        return true;
    }

    void resize(std::uint64_t size) override
    {
        data_.resize(size);
    }

    /// The reads and writes served so far.
    [[nodiscard]] std::size_t reads() const
    {
        const std::scoped_lock lock(mutex_);
        return reads_;
    }

    [[nodiscard]] std::size_t writes() const
    {
        const std::scoped_lock lock(mutex_);
        return writes_;
    }

    [[nodiscard]] const std::vector<std::byte>& data() const noexcept
    {
        return data_;
    }

private:
    std::vector<std::byte> data_;
    mutable std::mutex mutex_;
    mutable std::size_t reads_ = 0;
    std::size_t writes_ = 0;
};

/// A device as large as a test needs, beyond 4 GiB without disk space or memory: its first bytes are given, such as the
/// header of a unit, every other byte reads as zero, and the bytes written are kept in pages of 64 KiB.
class virtual_device final : public random_access_device
{
public:
    static constexpr std::uint64_t page = std::uint64_t{64} << 10;

    virtual_device(const std::vector<std::byte>& prefix, std::uint64_t size) : size_(size)
    {
        store(0, prefix);
    }

    std::uint64_t size() const override
    {
        return size_;
    }

    void read(std::uint64_t offset, std::span<std::byte> destination) const override
    {
        if (offset > size_ || destination.size() > size_ - offset) {
            throw io_error(errc::end_of_data, "a read beyond the end of the virtual device");
        }
        std::ranges::fill(destination, std::byte{0});
        for (std::uint64_t done = 0; done < destination.size();) {
            const std::uint64_t at = offset + done;
            const std::uint64_t within = at % page;
            const std::uint64_t part = std::min<std::uint64_t>(page - within, destination.size() - done);
            const auto found = pages_.find(at / page);
            if (found != pages_.end()) {
                std::copy_n(found->second.begin() + static_cast<std::ptrdiff_t>(within), part,
                            destination.begin() + static_cast<std::ptrdiff_t>(done));
            }
            done += part;
        }
    }

    void write(std::uint64_t offset, std::span<const std::byte> data) override
    {
        store(offset, data);
    }

    bool can_resize() const override
    {
        return true;
    }

    void resize(std::uint64_t size) override
    {
        // The pages beyond the new size are gone, and the bytes of the page it ends within after it zeros again.
        pages_.erase(pages_.lower_bound((size + page - 1) / page), pages_.end());
        if (size % page != 0) {
            const auto last = pages_.find(size / page);
            if (last != pages_.end()) {
                std::fill(last->second.begin() + static_cast<std::ptrdiff_t>(size % page), last->second.end(),
                          std::byte{0});
            }
        }
        size_ = size;
    }

    /// The bytes held in memory.
    [[nodiscard]] std::uint64_t held() const noexcept
    {
        return pages_.size() * page;
    }

private:
    void store(std::uint64_t offset, std::span<const std::byte> data)
    {
        if (offset > size_ || data.size() > size_ - offset) {
            throw usage_error(errc::invalid_argument, "a write beyond the end of the virtual device");
        }
        for (std::uint64_t done = 0; done < data.size();) {
            const std::uint64_t at = offset + done;
            const std::uint64_t within = at % page;
            const std::uint64_t part = std::min<std::uint64_t>(page - within, data.size() - done);
            std::vector<std::byte>& bytes = pages_[at / page];
            bytes.resize(page);
            std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(done), part,
                        bytes.begin() + static_cast<std::ptrdiff_t>(within));
            done += part;
        }
    }

    std::uint64_t size_;
    std::map<std::uint64_t, std::vector<std::byte>> pages_;
};

} // namespace openfits::test
