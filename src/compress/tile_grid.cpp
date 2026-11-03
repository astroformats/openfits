// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "compress/tile_grid.h"

#include "core/checked_math.h"

#include <algorithm>
#include <utility>

namespace openfits::detail {

namespace {

std::uint64_t magnitude(std::int64_t step) noexcept
{
    return step < 0 ? 0 - static_cast<std::uint64_t>(step) : static_cast<std::uint64_t>(step);
}

} // namespace

tile_grid::tile_grid(std::vector<std::uint64_t> axes, std::vector<std::uint64_t> tile)
    : axes_(std::move(axes)), tile_(std::move(tile)), counts_(axes_.size()), count_(axes_.empty() ? 0 : 1)
{
    for (std::size_t a = 0; a < axes_.size(); ++a) {
        tile_[a] = std::min(tile_[a], axes_[a]);
        counts_[a] = tile_[a] == 0 ? 0 : ((axes_[a] - 1) / tile_[a]) + 1;
        count_ = checked_multiply(count_, counts_[a]);
    }
}

void tile_grid::box(std::uint64_t index, std::span<std::uint64_t> start, std::span<std::uint64_t> size) const noexcept
{
    for (std::size_t a = 0; a < axes_.size(); ++a) {
        const std::uint64_t position = index % counts_[a];
        index /= counts_[a];
        start[a] = position * tile_[a];
        size[a] = std::min(tile_[a], axes_[a] - start[a]);
    }
}

std::uint64_t tile_grid::elements(std::uint64_t index) const noexcept
{
    std::uint64_t elements = 1;
    for (std::size_t a = 0; a < axes_.size(); ++a) {
        const std::uint64_t position = index % counts_[a];
        index /= counts_[a];
        elements *= std::min(tile_[a], axes_[a] - (position * tile_[a]));
    }
    return elements;
}

std::vector<std::uint64_t> tile_grid::tiles_of(const region_selection& selection) const
{
    // The tiles along each axis that hold a selected element, in order.
    std::vector<std::vector<std::uint64_t>> along(axes_.size());
    for (std::size_t a = 0; a < axes_.size(); ++a) {
        const axis_selection& axis = selection.axes[a];
        const std::uint64_t step = magnitude(axis.step);
        const std::uint64_t low = axis.step > 0 ? axis.first : axis.first - ((axis.count - 1) * step);
        // From the tile of an element to that of the first element beyond it, without the elements between; a step of
        // 0, which select_region() refuses, stays in one tile.
        for (std::uint64_t k = 0; k < axis.count;) {
            const std::uint64_t tile = (low + (k * step)) / tile_[a];
            along[a].push_back(tile);
            const std::uint64_t next = (tile + 1) * tile_[a];
            k = step == 0 ? axis.count : ((next - low) + step - 1) / step;
        }
    }
    std::vector<std::uint64_t> tiles;
    if (axes_.empty() || selection.elements == 0) {
        return tiles;
    }
    // Their combinations as an odometer, the first axis fastest, which keeps the order of the tiles.
    std::vector<std::size_t> position(axes_.size(), 0);
    for (;;) {
        std::uint64_t index = 0;
        std::uint64_t stride = 1;
        for (std::size_t a = 0; a < axes_.size(); ++a) {
            index += along[a][position[a]] * stride;
            stride *= counts_[a];
        }
        tiles.push_back(index);
        std::size_t a = 0;
        for (; a < axes_.size(); ++a) {
            if (++position[a] < along[a].size()) {
                break;
            }
            position[a] = 0;
        }
        if (a == axes_.size()) {
            return tiles;
        }
    }
}

index_range selected_within(const axis_selection& axis, std::uint64_t low, std::uint64_t high) noexcept
{
    const std::uint64_t step = magnitude(axis.step);
    if (step == 0) {
        // No selection has a step of 0, which select_region() refuses.
        return {};
    }
    if (axis.step > 0) {
        // first + k x step in [low, high].
        if (axis.first > high) {
            return {};
        }
        const std::uint64_t begin = axis.first >= low ? 0 : ((low - axis.first) + step - 1) / step;
        const std::uint64_t end = std::min(axis.count, ((high - axis.first) / step) + 1);
        return {.begin = begin, .end = end};
    }
    // first - k x step in [low, high].
    if (axis.first < low) {
        return {};
    }
    const std::uint64_t begin = axis.first <= high ? 0 : ((axis.first - high) + step - 1) / step;
    const std::uint64_t end = std::min(axis.count, ((axis.first - low) / step) + 1);
    return {.begin = begin, .end = end};
}

} // namespace openfits::detail
