// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// The random numbers of subtractive dithering (standard §10.2.1, Appendix I): 10,000 numbers between 0 and 1 from the
// generator of Park and Miller, seed = 16807 x seed mod (2^31 - 1) from a seed of 1, which every tile of a dithered
// image walks from a place that its row and ZDITHER0 give.

namespace openfits::detail {

/// The numbers of the sequence.
inline constexpr std::size_t dither_count = 10'000;

/// The sequence of standard Appendix I, each number the seed divided by 2^31 - 1, rounded to a float.
[[nodiscard]] const std::array<float, dither_count>& dither_numbers() noexcept;

/// The walk of a tile through the sequence (standard §10.2.1, steps 4 and 5): the number of its first pixel, then one
/// number for each pixel after it.
class dither_walk
{
public:
    /// The walk of the tile stored in row row of the table, counted from 1, under the seed ZDITHER0, from 1 to 10000:
    /// from the number at index (row - 1 + ZDITHER0) mod 10000 counted from 1, as every file has it, which standard
    /// Eq. 15 gives as I0 = mod(Ntile - 1 + ZDITHER0, 10000) and step 5 walks to the end of the 10,000 numbers, where
    /// the text of the standard says 500.
    dither_walk(std::uint64_t row, std::uint32_t seed) noexcept;

    /// The number of the current pixel, after which the walk moves to the next one.
    [[nodiscard]] float next() noexcept
    {
        const float number = numbers_[current_];
        if (++current_ == dither_count) {
            restart();
        }
        return number;
    }

private:
    // The end of the sequence: the walk starts again from the number of the next tile.
    void restart() noexcept;

    const std::array<float, dither_count>& numbers_;
    std::size_t first_;
    std::size_t current_;
};

} // namespace openfits::detail
