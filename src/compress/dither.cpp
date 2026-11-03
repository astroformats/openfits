// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "compress/dither.h"

#include <cmath>

namespace openfits::detail {

namespace {

// The numbers that a walk starts from, at the number of its tile times this.
constexpr float start_span = 500.0F;

std::size_t start_of(const std::array<float, dither_count>& numbers, std::size_t first) noexcept
{
    return static_cast<std::size_t>(numbers[first] * start_span);
}

} // namespace

const std::array<float, dither_count>& dither_numbers() noexcept
{
    // seed = a x seed mod m in doubles, which hold every product exactly, as the code of Appendix I computes it.
    static const std::array<float, dither_count> numbers = [] {
        constexpr double a = 16807.0;
        constexpr double m = 2147483647.0;
        std::array<float, dither_count> values{};
        double seed = 1.0;
        for (float& value : values) {
            const double product = a * seed;
            seed = product - (m * std::trunc(product / m));
            value = static_cast<float>(seed / m);
        }
        return values;
    }();
    return numbers;
}

dither_walk::dither_walk(std::uint64_t row, std::uint32_t seed) noexcept
    : numbers_(dither_numbers()), first_((row + seed - 2) % dither_count), current_(start_of(numbers_, first_))
{}

void dither_walk::restart() noexcept
{
    first_ = (first_ + 1) % dither_count;
    current_ = start_of(numbers_, first_);
}

} // namespace openfits::detail
