// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/table.h>

#include "io/thread_safe_source.h"
#include "support/counting_source.h"
#include "support/fixture_builder.h"
#include "support/table_fixtures.h"
#include "support/throws.h"
#include "support/virtual_source.h"
#include "table/column_read.h"

#include <gtest/gtest.h>

#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using openfits::blank_policy;
using openfits::column_arrays;
using openfits::column_format;
using openfits::column_read_options;
using openfits::column_storage;
using openfits::column_type;
using openfits::errc;
using openfits::invalid_data_error;
using openfits::limit_error;
using openfits::reader;
using openfits::row_range;
using openfits::usage_error;
using openfits::test::bintable_records;
using openfits::test::byte_writer;
using openfits::test::card_record;
using openfits::test::counting_source;
using openfits::test::fixture;
using openfits::test::throws;
using openfits::test::with;

reader open(const fixture& file, const openfits::read_options& options = {})
{
    return reader(std::make_unique<openfits::memory_source>(std::vector<std::byte>(file.bytes())), options);
}

// A file of an empty primary HDU and a binary table: the rows, then gap bytes, then the heap.
fixture heap_table(const std::vector<std::string>& records, const byte_writer& rows, const byte_writer& heap,
                   std::size_t gap = 0)
{
    byte_writer data;
    for (const std::byte b : rows.data()) {
        data.bytes({static_cast<std::uint8_t>(b)});
    }
    data.text("", gap, '\0');
    for (const std::byte b : heap.data()) {
        data.bytes({static_cast<std::uint8_t>(b)});
    }
    fixture file;
    file.primary();
    file.header(records).data(data.data());
    return file;
}

// The descriptors of a P column: count and offset, two 32-bit integers.
byte_writer& descriptor(byte_writer& rows, std::int32_t count, std::int32_t offset)
{
    return rows.value<std::int32_t>(count).value<std::int32_t>(offset);
}

TEST(table_heap, reads_variable_length_arrays_of_p_descriptors)
{
    // Standard §7.3.5: (nelem, offset) in the row, the offset from the start of the heap.
    byte_writer rows;
    descriptor(rows, 3, 0);
    descriptor(rows, 0, 0);
    descriptor(rows, 2, 12);
    byte_writer heap;
    heap.values<std::int32_t>({10, 20, 30, 40, 50});
    const reader input = open(heap_table(bintable_records(8, 3, {"1PJ(3)"}, 20), rows, heap));
    const openfits::table_info table = input.table(1);
    EXPECT_EQ(table.columns[0].format,
              (column_format{.type = column_type::int32, .storage = column_storage::variable_p, .max_elements = 3}));
    EXPECT_EQ(table.heap_offset, 24U);
    EXPECT_EQ(table.heap_size, 20U);
    const column_arrays<std::int64_t> arrays = input.read_arrays<std::int64_t>(1, 0);
    ASSERT_EQ(arrays.size(), 3U);
    EXPECT_EQ(arrays.offsets, (std::vector<std::uint64_t>{0, 3, 3, 5}));
    EXPECT_EQ(std::vector<std::int64_t>(arrays[0].begin(), arrays[0].end()), (std::vector<std::int64_t>{10, 20, 30}));
    EXPECT_TRUE(arrays[1].empty());
    EXPECT_EQ(std::vector<std::int64_t>(arrays[2].begin(), arrays[2].end()), (std::vector<std::int64_t>{40, 50}));
    EXPECT_EQ(input.read_arrays<double>(1, 0, {.rows = row_range{.first = 2}}).values, (std::vector<double>{40, 50}));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { (void)input.read_column<double>(1, 0); }));
    EXPECT_TRUE(throws<usage_error>(errc::column_type, [&] { (void)input.read_strings(1, 0); }));
}

TEST(table_heap, finds_the_heap_at_theap)
{
    // Standard §7.3.5: THEAP gives the heap after a gap; PCOUNT counts the gap and the heap.
    byte_writer rows;
    descriptor(rows, 2, 4);
    byte_writer heap;
    heap.values<std::int16_t>({-1, -2, 7, 8});
    const fixture file =
        heap_table(with(bintable_records(8, 1, {"PI"}, 108), {card_record("THEAP", "108")}), rows, heap, 100);
    const reader input = open(file);
    EXPECT_EQ(input.table(1).heap_offset, 108U);
    EXPECT_EQ(input.table(1).heap_size, 8U);
    EXPECT_EQ(input.read_arrays<std::int16_t>(1, 0).values, (std::vector<std::int16_t>{7, 8}));
}

TEST(table_heap, refuses_descriptors_outside_the_heap)
{
    byte_writer heap;
    heap.values<std::int32_t>({1, 2});
    for (const auto& [count, offset] :
         std::vector<std::pair<std::int32_t, std::int32_t>>{{3, 0}, {1, 8}, {2, 4}, {1, 7}}) {
        SCOPED_TRACE(offset);
        byte_writer rows;
        descriptor(rows, count, offset);
        const reader input = open(heap_table(bintable_records(8, 1, {"PJ"}, 8), rows, heap));
        EXPECT_TRUE(throws<invalid_data_error>(errc::invalid_descriptor, [&] { (void)input.read_arrays<int>(1, 0); }));
    }
    // An empty array has no data, and its offset is not looked at (standard §7.3.5).
    byte_writer empty;
    descriptor(empty, 0, 999999);
    EXPECT_TRUE(open(heap_table(bintable_records(8, 1, {"PJ"}, 8), empty, heap)).read_arrays<int>(1, 0).values.empty());
}

TEST(table_heap, refuses_negative_q_descriptors)
{
    byte_writer heap;
    heap.values<std::int32_t>({1, 2});
    for (const auto& [count, offset] : std::vector<std::pair<std::int64_t, std::int64_t>>{{-1, 0}, {1, -4}}) {
        byte_writer rows;
        rows.value<std::int64_t>(count).value<std::int64_t>(offset);
        const reader input = open(heap_table(bintable_records(16, 1, {"QJ"}, 8), rows, heap));
        EXPECT_TRUE(throws<invalid_data_error>(errc::invalid_descriptor, [&] { (void)input.read_arrays<int>(1, 0); }));
    }
}

TEST(table_heap, reads_descriptors_beyond_2_to_the_31)
{
    // A heap of 7 GiB, served by a virtual source: the byte at each offset of the file is the offset modulo 256.
    const std::uint64_t heap_size = std::uint64_t{7} << 30;
    const std::uint64_t far = (std::uint64_t{1} << 31) + 1000;
    for (const bool wide : {false, true}) {
        SCOPED_TRACE(wide);
        byte_writer rows;
        if (wide) {
            rows.value<std::int64_t>(3).value<std::int64_t>(static_cast<std::int64_t>(far + (std::uint64_t{1} << 32)));
        } else {
            // A P descriptor read unsigned, as CFITSIO reads it: an offset from 2^31 to 2^32.
            rows.value<std::uint32_t>(3).value<std::uint32_t>(static_cast<std::uint32_t>(far));
        }
        fixture prefix;
        prefix.primary();
        const std::uint64_t row_size = wide ? 16 : 8;
        prefix.header(bintable_records(row_size, 1, {wide ? "QB" : "PB"}, heap_size));
        const std::uint64_t data_offset = prefix.size();
        prefix.append(std::string_view(reinterpret_cast<const char*>(rows.data().data()), rows.size()));
        const std::uint64_t size = data_offset + row_size + heap_size;
        const reader input(std::make_unique<openfits::test::virtual_source>(
            prefix.bytes(), size, [](std::uint64_t at) { return static_cast<std::byte>(at % 256); }));
        const std::uint64_t heap = data_offset + row_size + (wide ? far + (std::uint64_t{1} << 32) : far);
        std::vector<std::uint8_t> expected;
        expected.reserve(3);
        for (std::uint64_t i = 0; i < 3; ++i) {
            expected.push_back(static_cast<std::uint8_t>((heap + i) % 256));
        }
        EXPECT_EQ(input.read_arrays<std::uint8_t>(1, 0).values, expected);
    }
}

TEST(table_heap, the_limits_of_arrays_bound_them)
{
    byte_writer rows;
    descriptor(rows, 5, 0);
    byte_writer heap;
    heap.values<std::uint8_t>({1, 2, 3, 4, 5});
    const fixture file = heap_table(bintable_records(8, 1, {"PB"}, 5), rows, heap);
    EXPECT_EQ(open(file).read_arrays<int>(1, 0).values.size(), 5U);
    openfits::read_options few;
    few.limits.max_vla_elements = 4;
    EXPECT_TRUE(throws<limit_error>(errc::array_too_large, [&] { (void)open(file, few).read_arrays<int>(1, 0); }));
    openfits::read_options small_heap;
    small_heap.limits.max_heap_bytes = 4;
    EXPECT_TRUE(
        throws<limit_error>(errc::array_too_large, [&] { (void)open(file, small_heap).read_arrays<int>(1, 0); }));
    openfits::read_options small_allocation;
    small_allocation.limits.max_allocation = 19;
    EXPECT_TRUE(throws<limit_error>(errc::allocation_too_large,
                                    [&] { (void)open(file, small_allocation).read_arrays<int>(1, 0); }));
    EXPECT_EQ(open(file, small_allocation).read_arrays<std::uint8_t>(1, 0).values.size(), 5U);
}

TEST(table_heap, reads_arrays_of_every_type)
{
    byte_writer rows;
    descriptor(rows, 3, 0);  // L
    descriptor(rows, 10, 3); // X, 2 bytes
    descriptor(rows, 1, 5);  // C
    descriptor(rows, 2, 13); // E, scaled
    descriptor(rows, 2, 21); // B, TNULLn
    byte_writer heap;
    heap.bytes({'T', 0, 'F'}).bytes({0xC1, 0x80}).values<float>({1.0F, -1.0F}).values<float>({2.0F, 4.0F});
    heap.bytes({9, 200});
    const fixture file =
        heap_table(with(bintable_records(40, 1, {"PL", "PX", "PC", "PE", "PB"}, 23),
                        {card_record("TSCAL4", "0.5"), card_record("TZERO4", "1"), card_record("TNULL5", "9")}),
                   rows, heap);
    const reader input = open(file);
    const column_arrays<float> logicals = input.read_arrays<float>(1, 0, {.blank = blank_policy::mask});
    EXPECT_EQ(logicals.values[0], 1.0F);
    EXPECT_EQ(logicals.values[2], 0.0F);
    EXPECT_EQ(logicals.mask, (std::vector<std::uint8_t>{0, 1, 0}));
    EXPECT_EQ(input.read_arrays<std::uint8_t>(1, 1).values, (std::vector<std::uint8_t>{1, 1, 0, 0, 0, 0, 0, 1, 1, 0}));
    EXPECT_EQ(input.read_arrays<std::complex<double>>(1, 2).values, (std::vector<std::complex<double>>{{1.0, -1.0}}));
    EXPECT_EQ(input.read_arrays<double>(1, 3).values, (std::vector<double>{2.0, 3.0}));
    const std::vector<double> bytes = input.read_arrays<double>(1, 4).values;
    EXPECT_TRUE(std::isnan(bytes[0]));
    EXPECT_EQ(bytes[1], 200.0);
}

TEST(table_heap, reads_arrays_of_characters_as_strings)
{
    byte_writer rows;
    descriptor(rows, 5, 0);
    descriptor(rows, 3, 5);
    descriptor(rows, 0, 0);
    byte_writer heap;
    heap.text("hello", 5).text("ab", 3, '\0');
    const reader input = open(heap_table(bintable_records(8, 3, {"PA(5)"}, 8), rows, heap));
    EXPECT_EQ(input.read_strings(1, 0), (std::vector<std::string>{"hello", "ab", ""}));
    EXPECT_TRUE(throws<usage_error>(errc::column_type, [&] { (void)input.read_arrays<int>(1, 0); }));
}

TEST(table_heap, reads_the_heap_in_ranges_of_sorted_descriptors)
{
    // Arrays stored in any order, one shared by two rows (standard §7.3.6): one read for the descriptors, one for the
    // heap.
    byte_writer rows;
    descriptor(rows, 2, 8);
    descriptor(rows, 2, 0);
    descriptor(rows, 2, 8);
    byte_writer heap;
    heap.values<std::int32_t>({1, 2, 3, 4});
    const fixture file = heap_table(bintable_records(8, 3, {"PJ"}, 16), rows, heap);
    const reader input = open(file);
    auto counting = std::make_unique<counting_source>(file.bytes());
    const counting_source* counter = counting.get();
    const openfits::detail::thread_safe_source source(std::move(counting));
    std::vector<std::int32_t> values;
    std::vector<std::uint64_t> offsets;
    std::vector<std::uint8_t> mask;
    openfits::detail::read_arrays(
        source, input.hdu(1), input.table(1), 0, openfits::element_type::int32, {}, {},
        [&values](std::uint64_t elements) {
            values.resize(elements);
            return std::as_writable_bytes(std::span<std::int32_t>(values));
        },
        offsets, mask);
    EXPECT_EQ(values, (std::vector<std::int32_t>{3, 4, 1, 2, 3, 4}));
    EXPECT_EQ(counter->reads().size(), 2U);
}

TEST(table_heap, reads_fixed_fields_as_arrays)
{
    byte_writer rows;
    rows.values<std::int16_t>({1, 2, 3, 4});
    const reader input = open(heap_table(bintable_records(4, 2, {"2I"}), rows, byte_writer()));
    const column_arrays<int> arrays = input.read_arrays<int>(1, 0);
    EXPECT_EQ(arrays.offsets, (std::vector<std::uint64_t>{0, 2, 4}));
    EXPECT_EQ(arrays.values, (std::vector<int>{1, 2, 3, 4}));
}

} // namespace
