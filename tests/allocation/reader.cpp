// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// What an untrusted file costs in memory to open and to walk, through the public reader: the bytes that the reader
// keeps for each card, each unit and each diagnostic, which the limits of the header bytes, of the units and of the
// diagnostics bound (limits.h); the refusals of those limits before the memory that they guard is taken; and opens and
// walks that run out of memory, which leave nothing behind and a reader that walks on.
//
// The bounds are those of MSVC's debug standard library, whose containers are the largest, with a margin: 373 to 394
// bytes for each card there, 288 to 314 in its release library, 2,085 and 1,501 for each unit of a short header.

#include <openfits/error.h>
#include <openfits/io.h>
#include <openfits/limits.h>
#include <openfits/reader.h>

#include "allocation/allocations.h"
#include "support/fixture_builder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using openfits::test::allocation_count;
using openfits::test::allocations_of;
using openfits::test::card_record;
using openfits::test::fixture;
using openfits::test::record;

constexpr std::int64_t card_bound = 512;
constexpr std::int64_t unit_bound = 3072;
constexpr std::int64_t diagnostic_bound = 512;

// The cards of a header as files have them: a third integers, a third reals and a third strings, each with a comment,
// under keywords of their own.
std::vector<std::string> typical_records(std::size_t count)
{
    std::vector<std::string> records;
    records.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::string keyword = "K" + std::to_string(i);
        switch (i % 3) {
        case 0:
            records.push_back(card_record(keyword, std::to_string(i), "an integer of the header"));
            break;
        case 1:
            records.push_back(card_record(keyword, std::to_string(i) + ".25E-03", "a real of the header"));
            break;
        default:
            records.push_back(card_record(keyword, "'the value " + std::to_string(i) + "'", "a string"));
            break;
        }
    }
    return records;
}

// A file of a primary header and units extensions of a header alone, each named after its index.
std::vector<std::byte> file_of_units(std::size_t units)
{
    fixture file;
    file.primary();
    for (std::size_t i = 1; i < units; ++i) {
        file.header(openfits::test::with(openfits::test::extension_records(),
                                         {card_record("EXTNAME", "'UNIT" + std::to_string(i) + "'")}));
    }
    return file.bytes();
}

// The reader of the bytes, which it borrows, so that a count holds what the reader keeps alone.
openfits::reader open_borrowed(std::span<const std::byte> bytes, const openfits::read_options& options = {})
{
    return openfits::reader(std::make_unique<openfits::memory_source>(bytes), options);
}

// A source of the bytes that reads them through read() alone, as a file is read: in pieces, into memory of the reader.
std::unique_ptr<openfits::input_source> read_source(std::span<const std::byte> bytes)
{
    return std::make_unique<openfits::callback_source>(
        bytes.size(), [bytes](std::uint64_t offset, std::span<std::byte> destination) {
            std::copy_n(bytes.subspan(offset).begin(), destination.size(), destination.begin());
        });
}

bool skip_without_counts()
{
    return !openfits::test::counts_the_allocations_of_the_library() || !openfits::test::measures_blocks();
}

TEST(reader, an_open_keeps_at_most_512_bytes_for_each_card)
{
    if (skip_without_counts()) {
        GTEST_SKIP() << "the allocations of the library are not counted or measured in this build";
    }
    constexpr std::size_t cards = 100'000;
    for (const bool commentary : {false, true}) {
        fixture file;
        file.primary(commentary ? std::vector<std::string>(cards, record("COMMENT   a line of the commentary"))
                                : typical_records(cards));
        const std::vector<std::byte>& bytes = file.bytes();

        std::optional<openfits::reader> opened;
        const allocation_count count = allocations_of([&opened, &bytes] { opened.emplace(read_source(bytes)); });
        if (!opened) {
            FAIL() << "the open made no reader";
        }
        ASSERT_EQ(opened->hdu(0).header().size(), cards + 3);
        EXPECT_LE(count.kept, card_bound * static_cast<std::int64_t>(cards)) << "commentary " << commentary;
        // What the open takes besides the cards is the text of the header, which it parses from its pieces.
        EXPECT_LE(count.peak, count.kept + static_cast<std::int64_t>(bytes.size()) + (std::int64_t{1} << 20))
            << "commentary " << commentary;
    }
}

TEST(reader, a_walk_keeps_at_most_3_kib_for_each_unit_of_a_short_header)
{
    if (skip_without_counts()) {
        GTEST_SKIP() << "the allocations of the library are not counted or measured in this build";
    }
    constexpr std::size_t units = 10'000;
    const std::vector<std::byte> bytes = file_of_units(units);
    const openfits::reader opened = open_borrowed(bytes);

    std::size_t walked = 0;
    const allocation_count count = allocations_of([&opened, &walked] { walked = opened.hdu_count(); });
    ASSERT_EQ(walked, units);
    EXPECT_LE(count.kept, unit_bound * static_cast<std::int64_t>(units));
}

// Lowercase keywords, a warning each: the diagnostics stop at limits::max_diagnostics, and keep a bounded number of
// bytes each.
TEST(reader, the_diagnostics_of_a_header_keep_at_most_512_bytes_each_up_to_their_limit)
{
    if (skip_without_counts()) {
        GTEST_SKIP() << "the allocations of the library are not counted or measured in this build";
    }
    constexpr std::size_t cards = 20'000;
    std::vector<std::string> records;
    records.reserve(cards);
    for (std::size_t i = 0; i < cards; ++i) {
        records.push_back(card_record("k" + std::to_string(i), "1"));
    }
    fixture file;
    file.primary(records);
    const std::vector<std::byte>& bytes = file.bytes();

    std::optional<openfits::reader> opened;
    const allocation_count count = allocations_of([&opened, &bytes] { opened.emplace(open_borrowed(bytes)); });
    if (!opened) {
        FAIL() << "the open made no reader";
    }
    const std::size_t listed = opened->hdu(0).diagnostics().size();
    EXPECT_EQ(listed, openfits::limits{}.max_diagnostics);
    EXPECT_LE(count.kept,
              (card_bound * static_cast<std::int64_t>(cards)) + (diagnostic_bound * static_cast<std::int64_t>(listed)));
}

// A header of 4 MB against a limit of 1 MiB: refused once the reader has read the limit, before a card is parsed, and
// at once when the source lies in memory.
TEST(reader, a_header_beyond_its_limit_is_refused_before_it_is_parsed)
{
    if (skip_without_counts()) {
        GTEST_SKIP() << "the allocations of the library are not counted or measured in this build";
    }
    fixture file;
    file.primary(typical_records(50'000));
    const std::vector<std::byte>& bytes = file.bytes();
    openfits::read_options options;
    options.limits.max_header_bytes = std::uint64_t{1} << 20;

    for (const bool in_memory : {true, false}) {
        std::unique_ptr<openfits::input_source> source =
            in_memory ? std::make_unique<openfits::memory_source>(std::span<const std::byte>(bytes))
                      : read_source(bytes);
        std::optional<openfits::errc> refused;
        const allocation_count count = allocations_of([&source, &options, &refused] {
            try {
                const openfits::reader opened(std::move(source), options);
            } catch (const openfits::limit_error& failure) {
                refused = failure.code();
            }
        });
        EXPECT_EQ(refused, openfits::errc::header_too_large) << "in memory " << in_memory;
        const std::uint64_t bound = in_memory ? std::uint64_t{64} << 10 : (std::uint64_t{3} << 20) / 2;
        EXPECT_LE(count.requested, bound) << "in memory " << in_memory;
    }
}

// Units of headers of 400 KB each against a total of 1 MiB: the walk stops at the unit that would pass the total,
// whose header it does not parse, with an error among the diagnostics of the walk.
TEST(reader, headers_beyond_their_total_are_refused_before_they_are_parsed)
{
    if (skip_without_counts()) {
        GTEST_SKIP() << "the allocations of the library are not counted or measured in this build";
    }
    fixture file;
    file.primary();
    for (std::size_t i = 1; i < 10; ++i) {
        file.header(openfits::test::with(openfits::test::extension_records(), typical_records(5'000)));
    }
    const std::vector<std::byte>& bytes = file.bytes();
    openfits::read_options options;
    options.limits.max_total_header_bytes = std::uint64_t{1} << 20;
    const openfits::reader opened = open_borrowed(bytes, options);

    std::size_t walked = 0;
    const allocation_count count = allocations_of([&opened, &walked] { walked = opened.hdu_count(); });
    EXPECT_EQ(walked, 3U);
    const std::vector<openfits::diagnostic> stops = opened.diagnostics();
    EXPECT_TRUE(std::ranges::any_of(stops, [](const openfits::diagnostic& found) {
        return found.code == openfits::errc::header_too_large && found.severity == openfits::severity::error;
    }));
    // The cards of the headers within the total, two units of 5,005 cards, and nothing of the others: the source lies
    // in memory, so that the walk reads nothing into memory of its own either.
    EXPECT_LE(count.kept, card_bound * 2 * 5'005);
    EXPECT_LE(count.peak, count.kept + (std::int64_t{64} << 10));
}

TEST(reader, units_beyond_their_limit_are_not_walked)
{
    if (skip_without_counts()) {
        GTEST_SKIP() << "the allocations of the library are not counted or measured in this build";
    }
    const std::vector<std::byte> bytes = file_of_units(1'000);
    openfits::read_options options;
    options.limits.max_hdus = 100;
    const openfits::reader opened = open_borrowed(bytes, options);

    std::optional<openfits::errc> refused;
    const allocation_count count = allocations_of([&opened, &refused] {
        try {
            (void)opened.hdu(999);
        } catch (const openfits::limit_error& failure) {
            refused = failure.code();
        }
    });
    EXPECT_EQ(refused, openfits::errc::too_many_hdus);
    EXPECT_LE(count.kept, unit_bound * 100);
}

// An open that runs out of memory at any allocation throws std::bad_alloc and frees everything it took, and an open
// with memory reads the file.
TEST(reader, an_open_out_of_memory_frees_what_it_took)
{
    if (!openfits::test::can_fail_allocations() || skip_without_counts()) {
        GTEST_SKIP() << "the allocations of the library cannot fail or be measured in this build";
    }
    fixture file;
    file.primary(typical_records(40));
    file.image({11, 7}, typical_records(20));
    const std::vector<std::byte>& bytes = file.bytes();
    const auto open = [&bytes] {
        const openfits::reader opened(read_source(bytes));
        return opened.hdu_count() == 2 && opened.read_pixels<float>(1).size() == 77;
    };
    ASSERT_TRUE(open());

    for (std::uint64_t index = 0;; ++index) {
        openfits::test::fail_allocation(index);
        bool read = false;
        bool ran_out = false;
        const allocation_count count = allocations_of([&open, &read, &ran_out] {
            try {
                read = open();
            } catch (const std::bad_alloc&) {
                ran_out = true;
            }
        });
        if (!openfits::test::stop_failing_allocations()) {
            EXPECT_TRUE(read);
            break;
        }
        EXPECT_TRUE(ran_out) << "allocation " << index;
        EXPECT_EQ(count.kept, 0) << "allocation " << index;
    }
}

// A walk that runs out of memory at any allocation throws std::bad_alloc and leaves the reader as it was: it walks
// the same units afterwards.
TEST(reader, a_walk_out_of_memory_walks_on_afterwards)
{
    if (!openfits::test::can_fail_allocations()) {
        GTEST_SKIP() << "the allocations of the library cannot fail in this build";
    }
    constexpr std::size_t units = 20;
    const std::vector<std::byte> bytes = file_of_units(units);

    for (std::uint64_t index = 0;; ++index) {
        const openfits::reader opened(read_source(bytes));
        openfits::test::fail_allocation(index);
        bool ran_out = false;
        try {
            (void)opened.hdu(units - 1);
        } catch (const std::bad_alloc&) {
            ran_out = true;
        }
        if (!openfits::test::stop_failing_allocations()) {
            break;
        }
        ASSERT_TRUE(ran_out) << "allocation " << index;
        ASSERT_EQ(opened.hdu_count(), units) << "allocation " << index;
        for (std::size_t i = 1; i < units; ++i) {
            ASSERT_EQ(opened.hdu(i).name(), "UNIT" + std::to_string(i)) << "allocation " << index;
            ASSERT_EQ(opened.hdu(i).header_offset(), i * 2880) << "allocation " << index;
        }
        ASSERT_TRUE(opened.diagnostics().empty()) << "allocation " << index;
    }
}

} // namespace
