// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>
#include <openfits/io.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace openfits {

// The destructors are the key functions: defining them here emits the type information once, in the library, so that
// the classes are the same type on both sides of a shared-library boundary.

input_source::~input_source() = default;

bool input_source::supports_concurrent_reads() const
{
    return false;
}

std::string input_source::description() const
{
    return {};
}

std::span<const std::byte> input_source::bytes() const
{
    return {};
}

output_sink::~output_sink() = default;

void output_sink::write_zeros(std::uint64_t count)
{
    const std::vector<std::byte> zeros(std::min<std::uint64_t>(count, std::uint64_t{1} << 20));
    while (count > 0) {
        const std::size_t piece = std::min<std::uint64_t>(count, zeros.size());
        write(std::span(zeros).first(piece));
        count -= piece;
    }
}

bool output_sink::can_rewrite() const
{
    return false;
}

void output_sink::rewrite(std::uint64_t /*offset*/, std::span<const std::byte> /*data*/)
{
    throw usage_error(errc::invalid_argument, "this sink cannot rewrite");
}

void output_sink::finish() {}

random_access_device::~random_access_device() = default;

bool random_access_device::can_resize() const
{
    return false;
}

void random_access_device::resize(std::uint64_t /*size*/)
{
    throw usage_error(errc::invalid_argument, "this device cannot change its size");
}

void random_access_device::flush() {}

bool random_access_device::supports_concurrent_writes() const
{
    return false;
}

} // namespace openfits
