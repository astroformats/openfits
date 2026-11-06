// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>
#include <openfits/types.h>

#include "checksum/checksum.h"
#include "data/conversion.h"
#include "data/image_read.h"
#include "edit/overlay.h"
#include "edit/patch.h"
#include "support/sequence.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

// The writes of the editor into the data of units before a commit writes them: the overlay of the bytes written, and
// the patches of regions and bytes with the change of the sums of standard §4.4.2.7 that they make (Appendix J.4).

namespace {

using openfits::detail::byte_overlay;
using openfits::detail::data_patch;
using openfits::detail::patch_run;
using openfits::detail::sum_change;
using openfits::test::splitmix64;

// A value of [low, high] from the sequence.
std::uint64_t in_range(splitmix64& sequence, std::uint64_t low, std::uint64_t high)
{
    return low + (sequence.next() % (high - low + 1));
}

std::vector<std::byte> random_bytes(std::size_t size, splitmix64& sequence)
{
    std::vector<std::byte> bytes(size);
    for (std::byte& value : bytes) {
        value = static_cast<std::byte>(sequence.next() & 0xFFU);
    }
    return bytes;
}

// Reads the bytes of data, those beyond its end as zeros.
openfits::detail::data_reader reader_of(const std::vector<std::byte>& data)
{
    return [&data](std::uint64_t offset, std::span<std::byte> destination) {
        for (std::size_t i = 0; i < destination.size(); ++i) {
            const std::uint64_t at = offset + i;
            destination[i] = at < data.size() ? data[at] : std::byte{0};
        }
    };
}

TEST(byte_overlay, keeps_the_latest_bytes_of_every_offset_in_stretches_that_do_not_overlap)
{
    splitmix64 sequence(1);
    std::vector<std::byte> expected(600, std::byte{0});
    std::vector<bool> written(600, false);
    byte_overlay overlay;
    for (int round = 0; round < 2000; ++round) {
        const std::size_t offset = in_range(sequence, 0, 500);
        const std::vector<std::byte> bytes = random_bytes(in_range(sequence, 1, 60), sequence);
        overlay.write(offset, bytes);
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            expected[offset + i] = bytes[i];
            written[offset + i] = true;
        }
        if (round % 97 != 0) {
            continue;
        }
        std::vector<std::byte> laid(600, std::byte{0});
        overlay.apply(0, laid);
        ASSERT_EQ(laid, expected) << "round " << round;
        std::uint64_t end = 0;
        std::uint64_t size = 0;
        for (const auto& [at, stretch] : overlay.stretches()) {
            ASSERT_GE(at, end);
            end = at + stretch.size();
            size += stretch.size();
        }
        EXPECT_EQ(size, static_cast<std::uint64_t>(std::ranges::count(written, true)));
        EXPECT_EQ(overlay.size(), size);
        EXPECT_EQ(overlay.end(), end);
    }
}

TEST(byte_overlay, lays_its_bytes_over_any_window)
{
    byte_overlay overlay;
    const std::vector<std::byte> bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    overlay.write(10, bytes);
    std::vector<std::byte> window(3, std::byte{9});
    overlay.apply(12, window);
    EXPECT_EQ(window, (std::vector<std::byte>{std::byte{3}, std::byte{4}, std::byte{9}}));
    overlay.apply(5, window);
    EXPECT_EQ(window, (std::vector<std::byte>{std::byte{3}, std::byte{4}, std::byte{9}}));
    // Bytes written in order grow one stretch.
    overlay.write(14, bytes);
    EXPECT_EQ(overlay.stretches().size(), 1U);
}

// Patches of runs of every shape over random data: the change of the sums that they make gives the sum of the data
// after them, as a pass over the data does.
TEST(apply_patch, the_sums_of_the_bytes_changed_give_the_sum_of_the_data_after_them)
{
    splitmix64 sequence(2);
    for (int round = 0; round < 200; ++round) {
        const std::vector<std::byte> data = random_bytes(4000 + (static_cast<std::size_t>(round) % 7), sequence);
        std::vector<std::byte> after = data;
        byte_overlay overlay;
        sum_change change;
        for (int write = 0; write < 5; ++write) {
            data_patch patch;
            std::uint64_t offset = in_range(sequence, 0, 3000);
            for (int run = 0; run < 3; ++run) {
                const std::uint64_t size = in_range(sequence, 1, 8);
                const std::uint64_t count = in_range(sequence, 1, 20);
                const std::uint64_t stride = size + in_range(sequence, 0, 9);
                const patch_run piece{.offset = offset, .count = count, .width = size, .stride = stride};
                if (piece.end() > data.size()) {
                    break;
                }
                patch.runs.push_back(piece);
                const std::size_t length = piece.count * piece.width;
                const std::vector<std::byte> elements = random_bytes(length, sequence);
                patch.bytes.insert(patch.bytes.end(), elements.begin(), elements.end());
                for (std::uint64_t e = 0; e < piece.count; ++e) {
                    for (std::uint64_t b = 0; b < piece.width; ++b) {
                        after[piece.offset + (e * piece.stride) + b] = elements[(e * piece.width) + b];
                    }
                }
                offset = piece.end() + in_range(sequence, 0, 9);
            }
            openfits::detail::apply_patch(patch, overlay, reader_of(data), &change, 64);
        }
        std::vector<std::byte> laid = data;
        overlay.apply(0, laid);
        ASSERT_EQ(laid, after);
        const std::optional<std::uint32_t> sum = openfits::detail::changed_sum(openfits::detail::sum_of(data), change);
        if (sum) {
            EXPECT_EQ(*sum, openfits::detail::sum_of(after)) << "round " << round;
        }
        EXPECT_EQ(openfits::detail::overlaid_sum(reader_of(data), overlay, data.size(), 100),
                  openfits::detail::sum_of(after));
    }
}

TEST(apply_patch, a_read_that_fails_leaves_the_overlay_and_the_sums_as_they_were)
{
    const std::vector<std::byte> data(100, std::byte{5});
    data_patch patch;
    patch.runs = {{.offset = 0, .count = 2, .width = 2, .stride = 4},
                  {.offset = 80, .count = 2, .width = 2, .stride = 4}};
    patch.bytes.assign(8, std::byte{7});
    byte_overlay overlay;
    sum_change change{.before = 1, .after = 2};
    int calls = 0;
    const openfits::detail::data_reader failing = [&](std::uint64_t offset, std::span<std::byte> destination) {
        if (++calls == 2) {
            throw std::runtime_error("injected read failure");
        }
        reader_of(data)(offset, destination);
    };
    // Runs apart, read one after the other: the second read fails.
    EXPECT_THROW(openfits::detail::apply_patch(patch, overlay, failing, &change, 8), std::runtime_error);
    EXPECT_TRUE(overlay.empty());
    EXPECT_EQ(change.before, 1U);
    EXPECT_EQ(change.after, 2U);
}

TEST(changed_sum, bytes_that_sum_as_they_did_leave_the_sum_and_a_zero_needs_a_pass)
{
    EXPECT_EQ(openfits::detail::changed_sum(12345, {.before = 7, .after = 7}), 12345U);
    // Zeros that stay zeros leave either zero as it was.
    EXPECT_EQ(openfits::detail::changed_sum(0, {.before = 0, .after = 0}), 0U);
    EXPECT_EQ(openfits::detail::changed_sum(0xFFFFFFFFU, {.before = 0, .after = 0}), 0xFFFFFFFFU);
    EXPECT_EQ(openfits::detail::changed_sum(0, {.before = 0, .after = 5}), 5U);
    // 5 taken away from 5: zero of one form or the other, which only the bytes tell.
    EXPECT_FALSE(openfits::detail::changed_sum(5, {.before = 5, .after = 0}).has_value());
}

// Changes folded from several writes sum alike before and after, the second write taking away what the first wrote,
// and still turn data of zeros, whose sum is 0, into data that sum to 0xFFFFFFFF, or the reverse: the cell of a table
// of zeros written with 127, and a row of -128 appended (found by fuzz_editor).
TEST(changed_sum, bytes_that_sum_as_they_did_leave_a_zero_to_a_pass)
{
    EXPECT_FALSE(openfits::detail::changed_sum(0, {.before = 0xFFFFFF80U, .after = 0xFFFFFF80U}).has_value());
    EXPECT_FALSE(openfits::detail::changed_sum(0xFFFFFFFFU, {.before = 7, .after = 7}).has_value());
}

TEST(region_patch, stores_the_values_of_a_region_in_the_order_of_the_data)
{
    // A region of 3 x 2 elements of a 5 x 4 array, with a step of -1 along the second axis, from rows of 4 values.
    const openfits::geometry array{.axes = {5, 4}};
    const openfits::detail::region_selection selection =
        openfits::detail::select_region(openfits::region{.start = {1, 1}, .size = {3, 2}, .step = {1, -1}}, array);
    const std::vector<std::int16_t> values{1, 2, 3, 0, 4, 5, 6, 0};
    const openfits::detail::write_conversion conversion = openfits::detail::make_write_conversion(
        openfits::numeric_type::int16, openfits::bitpix::int16, {}, std::nullopt, openfits::overflow_policy::refuse, 0);
    const data_patch patch = openfits::detail::region_patch(
        selection, array.axes, 4, conversion, openfits::numeric_type::int16, std::as_bytes(std::span(values)), 0);
    ASSERT_EQ(patch.runs.size(), 2U);
    // Row 1 of the array, the second of the values, first in the order of the data.
    EXPECT_EQ(patch.runs[0].offset, ((1 * 5) + 1) * 2U);
    EXPECT_EQ(patch.runs[1].offset, ((2 * 5) + 1) * 2U);
    const std::vector<std::byte> expected{std::byte{0}, std::byte{4}, std::byte{0}, std::byte{5},
                                          std::byte{0}, std::byte{6}, std::byte{0}, std::byte{1},
                                          std::byte{0}, std::byte{2}, std::byte{0}, std::byte{3}};
    EXPECT_EQ(patch.bytes, expected);
}

TEST(region_patch, refuses_stored_values_beyond_the_limit_of_allocations)
{
    const openfits::geometry array{.axes = {1000}};
    const openfits::detail::region_selection selection = openfits::detail::select_region(std::nullopt, array);
    const std::vector<std::int32_t> values(1000);
    const openfits::detail::write_conversion conversion = openfits::detail::make_write_conversion(
        openfits::numeric_type::int32, openfits::bitpix::int32, {}, std::nullopt, openfits::overflow_policy::refuse, 0);
    EXPECT_THROW((void)openfits::detail::region_patch(selection, array.axes, 0, conversion,
                                                      openfits::numeric_type::int32, std::as_bytes(std::span(values)),
                                                      3999),
                 openfits::limit_error);
}

} // namespace
