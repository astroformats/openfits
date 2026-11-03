// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "compress/tile_grid.h"

#include <openfits/types.h>

#include "data/image_read.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace {

using openfits::region;
using openfits::detail::region_selection;
using openfits::detail::select_region;
using openfits::detail::tile_grid;
using openfits::detail::tile_run;

std::vector<std::uint64_t> box_start(const tile_grid& grid, std::uint64_t index)
{
    std::vector<std::uint64_t> start(grid.axes().size());
    std::vector<std::uint64_t> size(grid.axes().size());
    grid.box(index, start, size);
    return start;
}

std::vector<std::uint64_t> box_size(const tile_grid& grid, std::uint64_t index)
{
    std::vector<std::uint64_t> start(grid.axes().size());
    std::vector<std::uint64_t> size(grid.axes().size());
    grid.box(index, start, size);
    return size;
}

TEST(tile_grid, tiles_follow_the_first_pixel_with_the_first_axis_fastest)
{
    // Standard §10.1.2: the last tile along an axis that ZTILEn does not divide is shorter.
    const tile_grid grid({37, 23}, {16, 10});
    EXPECT_EQ(grid.counts(), (std::vector<std::uint64_t>{3, 3}));
    EXPECT_EQ(grid.count(), 9U);
    EXPECT_EQ(box_start(grid, 0), (std::vector<std::uint64_t>{0, 0}));
    EXPECT_EQ(box_start(grid, 1), (std::vector<std::uint64_t>{16, 0}));
    EXPECT_EQ(box_start(grid, 3), (std::vector<std::uint64_t>{0, 10}));
    EXPECT_EQ(box_size(grid, 2), (std::vector<std::uint64_t>{5, 10}));
    EXPECT_EQ(box_size(grid, 8), (std::vector<std::uint64_t>{5, 3}));
    EXPECT_EQ(grid.elements(8), 15U);
    std::uint64_t total = 0;
    for (std::uint64_t i = 0; i < grid.count(); ++i) {
        total += grid.elements(i);
    }
    EXPECT_EQ(total, 37U * 23U);
}

TEST(tile_grid, a_tile_longer_than_its_axis_covers_the_axis)
{
    const tile_grid grid({37, 23, 3}, {100, 1, 7});
    EXPECT_EQ(grid.tile(), (std::vector<std::uint64_t>{37, 1, 3}));
    EXPECT_EQ(grid.counts(), (std::vector<std::uint64_t>{1, 23, 1}));
    EXPECT_EQ(grid.count(), 23U);
}

TEST(tile_grid, a_region_meets_the_tiles_that_hold_its_elements)
{
    const tile_grid grid({37, 23}, {16, 10});
    const openfits::geometry array{.axes = {37, 23}};
    EXPECT_EQ(grid.tiles_of(select_region(std::nullopt, array)).size(), 9U);
    // Columns 15 to 17 of rows 9 and 10 lie across four tiles.
    const region_selection corner = select_region(region{.start = {15, 9}, .size = {3, 2}}, array);
    EXPECT_EQ(grid.tiles_of(corner), (std::vector<std::uint64_t>{0, 1, 3, 4}));
    // A step of 20 along the first axis takes columns 0 and 20, so that the third column of tiles holds none.
    const region_selection sparse = select_region(region{.start = {0, 0}, .size = {37, 1}, .step = {20, 1}}, array);
    EXPECT_EQ(grid.tiles_of(sparse), (std::vector<std::uint64_t>{0, 1}));
    // An axis read in reverse meets the same tiles.
    const region_selection reversed =
        select_region(region{.start = {32, 0}, .size = {5, 23}, .step = {-1, -11}}, array);
    EXPECT_EQ(grid.tiles_of(reversed), (std::vector<std::uint64_t>{2, 5, 8}));
}

// A tiling of an array and a region of it, whose values gathered tile by tile must be those of the region.
struct gather_case
{
    std::string name{};
    std::vector<std::uint64_t> axes{};
    std::vector<std::uint64_t> tile{};
    std::optional<region> box{};
    std::uint64_t pitch = 0;
};

// GoogleTest prints the parameter in messages, and CTest names the test after it.
// NOLINTNEXTLINE(readability-identifier-naming)
void PrintTo(const gather_case& value, std::ostream* output)
{
    *output << value.name;
}

class tile_gather : public testing::TestWithParam<gather_case>
{};

TEST_P(tile_gather, gathers_the_region_from_its_tiles)
{
    const gather_case& parameter = GetParam();
    const openfits::geometry array{.axes = parameter.axes};
    const std::uint64_t elements = array.element_count();
    const region_selection selection = select_region(parameter.box, array);
    const std::uint64_t span = openfits::detail::destination_span(selection, parameter.pitch);
    const std::uint64_t row = selection.axes.front().count;
    const std::uint64_t row_pitch = parameter.pitch == 0 ? row : parameter.pitch;

    // The region read from the array as it is: the element of each index of the region, the first axis fastest.
    const std::uint64_t untouched = elements + 1;
    std::vector<std::uint64_t> expected(span, untouched);
    std::vector<std::uint64_t> k(parameter.axes.size(), 0);
    for (std::uint64_t i = 0; i < selection.elements; ++i) {
        std::uint64_t element = 0;
        std::uint64_t stride = 1;
        std::uint64_t destination_row = 0;
        std::uint64_t rows = 1;
        for (std::size_t a = 0; a < parameter.axes.size(); ++a) {
            const auto& axis = selection.axes[a];
            const std::int64_t at =
                static_cast<std::int64_t>(axis.first) + (static_cast<std::int64_t>(k[a]) * axis.step);
            element += static_cast<std::uint64_t>(at) * stride;
            stride *= parameter.axes[a];
            if (a > 0) {
                destination_row += k[a] * rows;
                rows *= axis.count;
            }
        }
        expected[(destination_row * row_pitch) + k[0]] = element;
        for (std::size_t a = 0; a < k.size() && ++k[a] == selection.axes[a].count; ++a) {
            k[a] = 0;
        }
    }

    // The same values gathered from the tiles that the region meets, each a copy of the elements of its box.
    const tile_grid grid(parameter.axes, parameter.tile);
    std::vector<std::uint64_t> gathered(span, untouched);
    std::vector<int> writes(span, 0);
    std::vector<std::uint64_t> start(parameter.axes.size());
    std::vector<std::uint64_t> size(parameter.axes.size());
    for (const std::uint64_t index : grid.tiles_of(selection)) {
        grid.box(index, start, size);
        std::vector<std::uint64_t> tile(grid.elements(index));
        std::vector<std::uint64_t> local(parameter.axes.size(), 0);
        for (std::uint64_t& value : tile) {
            std::uint64_t element = 0;
            std::uint64_t stride = 1;
            for (std::size_t a = 0; a < local.size(); ++a) {
                element += (start[a] + local[a]) * stride;
                stride *= parameter.axes[a];
            }
            value = element;
            for (std::size_t a = 0; a < local.size() && ++local[a] == size[a]; ++a) {
                local[a] = 0;
            }
        }
        openfits::detail::for_each_tile_run(selection, start, size, parameter.pitch, [&](const tile_run& run) {
            for (std::uint64_t i = 0; i < run.count; ++i) {
                const std::int64_t at =
                    static_cast<std::int64_t>(run.first) + (static_cast<std::int64_t>(i) * run.step);
                gathered[run.destination + i] = tile[static_cast<std::uint64_t>(at)];
                ++writes[run.destination + i];
            }
        });
    }
    EXPECT_EQ(gathered, expected);
    for (std::uint64_t i = 0; i < span; ++i) {
        ASSERT_EQ(writes[i], expected[i] == untouched ? 0 : 1) << i;
    }
}

INSTANTIATE_TEST_SUITE_P(
    regions, tile_gather,
    testing::Values(gather_case{.name = "whole_rows", .axes = {37, 23}, .tile = {37, 1}},
                    gather_case{.name = "whole_squares", .axes = {37, 23}, .tile = {16, 16}},
                    gather_case{.name = "whole_one_pixel", .axes = {7, 5}, .tile = {1, 1}},
                    gather_case{.name = "whole_one_tile", .axes = {37, 23}, .tile = {64, 64}},
                    gather_case{.name = "cutout",
                                .axes = {37, 23},
                                .tile = {16, 10},
                                .box = region{.start = {5, 3}, .size = {20, 15}}},
                    gather_case{.name = "steps",
                                .axes = {37, 23},
                                .tile = {16, 10},
                                .box = region{.start = {1, 2}, .size = {33, 20}, .step = {3, 4}}},
                    gather_case{.name = "reversed",
                                .axes = {37, 23},
                                .tile = {16, 10},
                                .box = region{.start = {0, 0}, .size = {37, 23}, .step = {-1, -1}}},
                    gather_case{.name = "reversed_steps",
                                .axes = {37, 23},
                                .tile = {6, 7},
                                .box = region{.start = {2, 1}, .size = {31, 20}, .step = {-4, 3}}},
                    gather_case{.name = "pitch",
                                .axes = {37, 23},
                                .tile = {16, 10},
                                .box = region{.start = {5, 3}, .size = {20, 15}},
                                .pitch = 29},
                    gather_case{.name = "cube",
                                .axes = {5, 4, 3, 2},
                                .tile = {2, 3, 2, 1},
                                .box = region{.start = {1, 0, 0, 0}, .size = {4, 4, 3, 2}, .step = {2, -1, 2, -1}}},
                    gather_case{.name = "one_axis",
                                .axes = {100},
                                .tile = {7},
                                .box = region{.start = {3}, .size = {90}, .step = {-5}}}));

} // namespace
