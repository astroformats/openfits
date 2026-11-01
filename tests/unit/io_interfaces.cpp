// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>
#include <openfits/io.h>

#include "support/throws.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {

using openfits::errc;
using openfits::test::throws;

// The least an implementation must provide.
class minimal_source final : public openfits::input_source
{
public:
    std::uint64_t size() const override
    {
        return 0;
    }

    void read(std::uint64_t /*offset*/, std::span<std::byte> /*destination*/) const override {}
};

class minimal_sink final : public openfits::output_sink
{
public:
    void write(std::span<const std::byte> data) override
    {
        position_ += data.size();
    }

    std::uint64_t position() const override
    {
        return position_;
    }

private:
    std::uint64_t position_ = 0;
};

class minimal_device final : public openfits::random_access_device
{
public:
    std::uint64_t size() const override
    {
        return 0;
    }

    void read(std::uint64_t /*offset*/, std::span<std::byte> /*destination*/) const override {}

    void write(std::uint64_t /*offset*/, std::span<const std::byte> /*data*/) override {}
};

TEST(input_source, reads_one_at_a_time_through_read_alone_and_has_no_description_by_default)
{
    const minimal_source source;

    EXPECT_FALSE(source.supports_concurrent_reads());
    EXPECT_TRUE(source.bytes().empty());
    EXPECT_EQ(source.description(), "");
}

TEST(output_sink, cannot_rewrite_and_finishes_without_doing_anything_by_default)
{
    minimal_sink sink;
    const std::array<std::byte, 4> data{};
    sink.write(data);

    EXPECT_FALSE(sink.can_rewrite());
    EXPECT_TRUE(throws<openfits::usage_error>(errc::invalid_argument, [&] { sink.rewrite(0, data); }));
    sink.finish();
    EXPECT_EQ(sink.position(), 4U);
}

TEST(random_access_device, is_called_one_call_at_a_time_keeps_its_size_and_flushes_without_doing_anything_by_default)
{
    minimal_device device;

    EXPECT_FALSE(device.supports_concurrent_reads());
    EXPECT_FALSE(device.supports_concurrent_writes());
    EXPECT_FALSE(device.can_resize());
    EXPECT_TRUE(throws<openfits::usage_error>(errc::invalid_argument, [&] { device.resize(10); }));
    EXPECT_EQ(device.description(), "");
    device.flush();
}

} // namespace
