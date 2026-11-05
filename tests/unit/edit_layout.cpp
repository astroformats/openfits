// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>
#include <openfits/io.h>

#include "checksum/checksum.h"
#include "edit/layout.h"
#include "edit/overlay.h"
#include "io/thread_safe_device.h"
#include "support/faulty_io.h"
#include "support/sequence.h"
#include "support/test_devices.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <vector>

// The file that a commit makes from pieces: the bytes of the file as it is, new bytes, fills and units that write
// themselves, written into a new file or moved within the file in an order that reads each byte before it is
// overwritten.

namespace {

using openfits::errc;
using openfits::detail::byte_overlay;
using openfits::detail::layout_piece;
using openfits::detail::layout_unit;
using openfits::detail::thread_safe_device;
using openfits::test::splitmix64;
using openfits::test::throws;

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

std::shared_ptr<const std::vector<std::byte>> shared(std::vector<std::byte> bytes)
{
    return std::make_shared<const std::vector<std::byte>>(std::move(bytes));
}

// Appends a piece of new bytes to a unit. The piece is built in its place: clang-tidy's analyzer of the clang-cl
// database takes a shared_ptr moved into a container of the standard library for a leak.
layout_piece& add_bytes(layout_unit& unit, std::vector<std::byte> bytes)
{
    layout_piece& piece = unit.pieces.emplace_back();
    piece.type = layout_piece::kind::bytes;
    piece.size = bytes.size();
    piece.bytes = shared(std::move(bytes));
    return piece;
}

const auto ignore = [](std::uint64_t) {};

// A random file and a new layout of it: pieces of the file in their order, some dropped, others with writes laid over
// them, with new bytes and fills between them; and the bytes that the layout makes.
struct random_layout
{
    explicit random_layout(splitmix64& sequence)
    {
        old = random_bytes(20'000, sequence);
        std::uint64_t at = 0;
        while (at < old.size()) {
            const std::uint64_t length = std::min<std::uint64_t>(in_range(sequence, 1, 3000), old.size() - at);
            const std::uint64_t kind = in_range(sequence, 0, 9);
            layout_unit unit;
            if (kind < 2) {
                // Dropped: an erased unit.
            } else {
                layout_piece piece{.type = layout_piece::kind::file, .size = length, .source = at};
                std::vector<std::byte> bytes(old.begin() + static_cast<std::ptrdiff_t>(at),
                                             old.begin() + static_cast<std::ptrdiff_t>(at + length));
                if (kind < 4) {
                    // With writes laid over it, at offsets of its own data.
                    byte_overlay& overlay = overlays.emplace_back();
                    for (int w = 0; w < 3; ++w) {
                        const std::uint64_t offset = in_range(sequence, 0, length - 1);
                        const std::size_t count = std::min<std::uint64_t>(17, length - offset);
                        const std::vector<std::byte> written = random_bytes(count, sequence);
                        overlay.write(offset, written);
                        std::ranges::copy(written, bytes.begin() + static_cast<std::ptrdiff_t>(offset));
                    }
                    piece.overlay = &overlay;
                }
                unit.pieces.push_back(piece);
                expected.insert(expected.end(), bytes.begin(), bytes.end());
            }
            if (kind == 5 || kind == 8) {
                std::vector<std::byte> fresh = random_bytes(in_range(sequence, 1, 3000), sequence);
                expected.insert(expected.end(), fresh.begin(), fresh.end());
                (void)add_bytes(unit, std::move(fresh));
            }
            if (kind == 6) {
                const std::uint64_t fill = in_range(sequence, 1, 3000);
                expected.insert(expected.end(), fill, std::byte{' '});
                unit.pieces.push_back({.type = layout_piece::kind::fill, .size = fill, .fill = std::byte{' '}});
            }
            if (!unit.pieces.empty()) {
                units.push_back(std::move(unit));
            }
            at += length;
        }
    }

    std::vector<std::byte> old;
    // A deque, whose elements stay where they are: the pieces point to them.
    std::deque<byte_overlay> overlays;
    std::vector<layout_unit> units;
    std::vector<std::byte> expected;
};

TEST(layout, moves_the_bytes_of_a_file_in_place_into_any_layout_that_keeps_their_order)
{
    splitmix64 sequence(3);
    for (int round = 0; round < 300; ++round) {
        const random_layout layout(sequence);
        thread_safe_device file(std::make_unique<openfits::memory_device>(std::vector<std::byte>(layout.old)));
        bool moved = false;
        openfits::detail::shift_layout(file, layout.units, ignore, moved, 1000);
        const std::size_t size = file.size();
        std::vector<std::byte> result(size);
        file.read(0, result);
        ASSERT_EQ(result, layout.expected) << "round " << round;
    }
}

TEST(layout, writes_the_same_layout_into_a_new_file)
{
    splitmix64 sequence(4);
    for (int round = 0; round < 100; ++round) {
        const random_layout layout(sequence);
        const thread_safe_device file(std::make_unique<openfits::memory_device>(std::vector<std::byte>(layout.old)));
        openfits::memory_sink sink;
        openfits::detail::write_layout(sink, layout.units, file, ignore, 777);
        const std::span<const std::byte> written = sink.data();
        ASSERT_EQ(std::vector<std::byte>(written.begin(), written.end()), layout.expected) << "round " << round;
    }
}

// A unit whose header records the sum of its data, which only their write tells.
layout_unit sealed_unit(std::uint64_t source, std::uint64_t size)
{
    layout_unit unit;
    (void)add_bytes(unit, std::vector<std::byte>(8));
    unit.pieces.push_back({.type = layout_piece::kind::file, .size = size, .source = source});
    (void)add_bytes(unit, std::vector<std::byte>(3, std::byte{9}));
    unit.seal = [](std::uint32_t sum) {
        std::vector<std::byte> header(8, std::byte{0});
        for (int i = 0; i < 4; ++i) {
            header[static_cast<std::size_t>(i)] = static_cast<std::byte>(sum >> (8 * (3 - i)));
        }
        return header;
    };
    return unit;
}

TEST(layout, completes_a_header_once_the_data_of_its_unit_are_written)
{
    splitmix64 sequence(5);
    const std::vector<std::byte> old = random_bytes(500, sequence);
    std::vector<std::byte> data(old.begin() + 100, old.begin() + 300);
    data.insert(data.end(), 3, std::byte{9});
    const std::uint32_t sum = openfits::detail::sum_of(data);
    std::vector<layout_unit> units;
    // Moved towards the start of the file.
    units.push_back(sealed_unit(100, 200));
    const auto check = [sum](std::span<const std::byte> header) {
        for (int i = 0; i < 4; ++i) {
            EXPECT_EQ(header[static_cast<std::size_t>(i)], static_cast<std::byte>(sum >> (8 * (3 - i))));
        }
    };
    {
        const thread_safe_device file(std::make_unique<openfits::memory_device>(std::vector<std::byte>(old)));
        openfits::memory_sink sink;
        openfits::detail::write_layout(sink, units, file, ignore);
        ASSERT_EQ(sink.data().size(), 211U);
        check(sink.data().first(8));
    }
    thread_safe_device file(std::make_unique<openfits::memory_device>(std::vector<std::byte>(old)));
    bool moved = false;
    openfits::detail::shift_layout(file, units, ignore, moved);
    EXPECT_TRUE(moved);
    ASSERT_EQ(file.size(), 211U);
    std::vector<std::byte> header(8);
    file.read(0, header);
    check(header);
}

TEST(layout, a_unit_of_unknown_size_ends_a_file_moved_in_place_and_grows_it)
{
    const std::vector<std::byte> old(100, std::byte{1});
    std::vector<layout_unit> units;
    units.push_back({.pieces = {{.type = layout_piece::kind::file, .size = 100, .source = 0}}});
    layout_piece unit{.type = layout_piece::kind::unit, .size_known = false};
    unit.write = [](openfits::output_sink& sink, const std::function<void(std::uint64_t)>& advance) {
        const std::vector<std::byte> bytes(5000, std::byte{2});
        sink.write(bytes);
        sink.write_zeros(3000);
        advance(8000);
    };
    units.push_back({.pieces = {unit}});
    thread_safe_device file(std::make_unique<openfits::memory_device>(std::vector<std::byte>(old)));
    bool moved = false;
    openfits::detail::shift_layout(file, units, ignore, moved);
    EXPECT_FALSE(moved);
    ASSERT_EQ(file.size(), 8100U);
    std::vector<std::byte> bytes(8100);
    file.read(0, bytes);
    EXPECT_EQ(bytes[99], std::byte{1});
    EXPECT_EQ(bytes[5099], std::byte{2});
    EXPECT_EQ(bytes[8099], std::byte{0});

    // Before the end of the file, a unit of unknown size is refused, before anything is written.
    std::vector<layout_unit> refused{units[1], units[0]};
    thread_safe_device other(std::make_unique<openfits::memory_device>(std::vector<std::byte>(old)));
    EXPECT_TRUE(throws<openfits::usage_error>(errc::invalid_argument,
                                              [&] { openfits::detail::shift_layout(other, refused, ignore, moved); }));
}

TEST(layout, writes_only_the_blocks_of_a_header_that_differ_when_nothing_moves)
{
    std::vector<std::byte> old(std::size_t{3} * 2880, std::byte{' '});
    auto device = std::make_unique<openfits::test::counting_device>(old);
    const openfits::test::counting_device& counted = *device;
    thread_safe_device file(std::move(device));
    std::vector<std::byte> header = old;
    header[2880 + 5] = std::byte{'X'};
    std::vector<layout_unit> units(1);
    add_bytes(units[0], header).previous = shared(old);
    bool moved = false;
    openfits::detail::shift_layout(file, units, ignore, moved);
    EXPECT_EQ(counted.writes(), 1U);
    EXPECT_EQ(counted.data(), header);
}

TEST(layout, a_failure_while_units_move_is_reported_as_one_after_the_first_move)
{
    splitmix64 sequence(6);
    const std::vector<std::byte> old = random_bytes(10'000, sequence);
    std::vector<layout_unit> units(1);
    (void)add_bytes(units[0], std::vector<std::byte>(100));
    units.push_back({.pieces = {{.type = layout_piece::kind::file, .size = 10'000, .source = 0}}});
    openfits::memory_device inner{std::vector<std::byte>(old)};
    auto faulty = std::make_unique<openfits::test::faulty_device>(inner, 3, openfits::test::fault::error);
    thread_safe_device file(std::move(faulty));
    bool moved = false;
    // The resize first, then the moves in pieces of 1000 bytes: the second write of a move fails.
    EXPECT_THROW(openfits::detail::shift_layout(file, units, ignore, moved, 1000), openfits::io_error);
    EXPECT_TRUE(moved);
}

} // namespace
