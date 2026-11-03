// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include "data/image_read.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// The tiles of a compressed image (standard §10.1.2): rectangular boxes of the array, of ZTILEn elements along each
// axis, the last along an axis shorter where ZTILEn does not divide it, stored in the order of their first element, the
// first axis varying fastest.

namespace openfits::detail {

/// The tiles of an array of the given axes, of the given lengths along each.
class tile_grid
{
public:
    /// The grid of tiles of the lengths tile along axes; a tile longer than its axis covers the whole axis. axes and
    /// tile have one entry for each axis, each length at least 1.
    /// @throws usage_error (errc::arithmetic_overflow) when the number of tiles does not fit in 64 bits.
    tile_grid(std::vector<std::uint64_t> axes, std::vector<std::uint64_t> tile);

    [[nodiscard]] const std::vector<std::uint64_t>& axes() const noexcept
    {
        return axes_;
    }

    /// The length of the tiles along each axis, at most the axis.
    [[nodiscard]] const std::vector<std::uint64_t>& tile() const noexcept
    {
        return tile_;
    }

    /// The number of tiles along each axis.
    [[nodiscard]] const std::vector<std::uint64_t>& counts() const noexcept
    {
        return counts_;
    }

    /// The number of tiles.
    [[nodiscard]] std::uint64_t count() const noexcept
    {
        return count_;
    }

    /// The first element and the length along each axis of the tile at index, counted from 0 in the order of the
    /// tiles.
    void box(std::uint64_t index, std::span<std::uint64_t> start, std::span<std::uint64_t> size) const noexcept;

    /// The elements of the tile at index.
    [[nodiscard]] std::uint64_t elements(std::uint64_t index) const noexcept;

    /// The indices of the tiles that hold an element of selection, in their order. A tile within the box of a region
    /// that its steps skip is left out.
    [[nodiscard]] std::vector<std::uint64_t> tiles_of(const region_selection& selection) const;

private:
    std::vector<std::uint64_t> axes_;
    std::vector<std::uint64_t> tile_;
    std::vector<std::uint64_t> counts_;
    std::uint64_t count_ = 0;
};

/// A run of values of a region within a decoded tile: count values along the first axis, the first at element first
/// of the tile, each next one step elements further (backwards for a negative step), into the destination from its
/// element destination on, one after the other.
struct tile_run
{
    std::uint64_t first = 0;
    std::int64_t step = 1;
    std::uint64_t destination = 0;
    std::uint64_t count = 0;
};

/// Calls run(r) for every run of the elements of selection that lie in the tile whose first element is start along
/// each axis and whose lengths are size, with the destination of the region laid out in rows pitch elements apart (0
/// for rows that follow each other), as reader::read_pixels() lays it out.
template <typename Function>
void for_each_tile_run(const region_selection& selection, std::span<const std::uint64_t> start,
                       std::span<const std::uint64_t> size, std::uint64_t pitch, const Function& run);

/// The range of the indices k of the elements of an axis of a region, first + k x step, that lie from the element low
/// to the element high: in [begin, end), empty when begin >= end.
struct index_range
{
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
};

[[nodiscard]] index_range selected_within(const axis_selection& axis, std::uint64_t low, std::uint64_t high) noexcept;

template <typename Function>
void for_each_tile_run(const region_selection& selection, std::span<const std::uint64_t> start,
                       std::span<const std::uint64_t> size, std::uint64_t pitch, const Function& run)
{
    const std::size_t axes = selection.axes.size();
    std::vector<index_range> ranges(axes);
    for (std::size_t a = 0; a < axes; ++a) {
        ranges[a] = selected_within(selection.axes[a], start[a], start[a] + size[a] - 1);
        if (ranges[a].begin >= ranges[a].end) {
            return;
        }
    }
    const axis_selection& first = selection.axes.front();
    const std::uint64_t row_pitch = pitch == 0 ? first.count : pitch;
    // The indices of the region along the other axes, as an odometer from the first in the tile, the second fastest.
    std::vector<std::uint64_t> k(axes);
    for (std::size_t a = 0; a < axes; ++a) {
        k[a] = ranges[a].begin;
    }
    for (;;) {
        // The element of the tile and the row of the destination of the run at k.
        std::uint64_t element = 0;
        std::uint64_t stride = 1;
        std::uint64_t row = 0;
        std::uint64_t rows = 1;
        for (std::size_t a = 0; a < axes; ++a) {
            const axis_selection& axis = selection.axes[a];
            const std::uint64_t index = a == 0 ? ranges[0].begin : k[a];
            const std::uint64_t magnitude =
                axis.step < 0 ? 0 - static_cast<std::uint64_t>(axis.step) : static_cast<std::uint64_t>(axis.step);
            const std::uint64_t at =
                axis.step > 0 ? axis.first + (index * magnitude) : axis.first - (index * magnitude);
            element += (at - start[a]) * stride;
            stride *= size[a];
            if (a > 0) {
                row += k[a] * rows;
                rows *= axis.count;
            }
        }
        run(tile_run{.first = element,
                     .step = first.step,
                     .destination = (row * row_pitch) + ranges[0].begin,
                     .count = ranges[0].end - ranges[0].begin});
        std::size_t a = 1;
        for (; a < axes; ++a) {
            if (++k[a] < ranges[a].end) {
                break;
            }
            k[a] = ranges[a].begin;
        }
        if (a == axes) {
            return;
        }
    }
}

} // namespace openfits::detail
