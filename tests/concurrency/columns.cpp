// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/table.h>

#include "support/counting_source.h"
#include "support/fixture_builder.h"
#include "support/table_fixtures.h"
#include "support/threads.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

using openfits::reader;
using openfits::row_range;
using openfits::test::bintable_records;
using openfits::test::byte_writer;
using openfits::test::card_record;
using openfits::test::counting_source;
using openfits::test::fixture;
using openfits::test::run_threads;
using openfits::test::with;

constexpr std::size_t thread_count = 16;
constexpr std::int32_t rows = 2000;

// A table of 2000 rows: an integer that is the row, a double half of it, a name, and an array of the row's digits in
// the heap; then a table for each thread, not yet walked.
fixture table_file()
{
    byte_writer data;
    byte_writer heap;
    std::int32_t heap_offset = 0;
    for (std::int32_t row = 0; row < rows; ++row) {
        const std::string digits = std::to_string(row);
        data.value<std::int32_t>(row).value<double>(row / 2.0).text("row " + digits, 10);
        data.value<std::int32_t>(static_cast<std::int32_t>(digits.size())).value<std::int32_t>(heap_offset);
        for (const char digit : digits) {
            heap.value<std::int16_t>(static_cast<std::int16_t>(digit - '0'));
        }
        heap_offset += static_cast<std::int32_t>(digits.size() * 2);
    }
    for (const std::byte b : heap.data()) {
        data.bytes({static_cast<std::uint8_t>(b)});
    }
    fixture file;
    file.primary();
    file.header(with(bintable_records(30, rows, {"J", "D", "10A", "PI"}, heap.size()),
                     {card_record("TTYPE1", "'ROW'"), card_record("TTYPE2", "'HALF'")}))
        .data(data.data());
    for (std::size_t t = 0; t < thread_count; ++t) {
        byte_writer row;
        row.value<std::int16_t>(static_cast<std::int16_t>(t));
        file.header(bintable_records(2, 1, {"I"})).data(row.data());
    }
    return file;
}

// Threads read columns, strings and arrays of one reader at once, from memory, through a source that serializes its
// reads, and through one that does not; those that read the later tables walk the file meanwhile.
TEST(concurrency_columns, threads_read_the_columns_of_one_reader)
{
    const fixture file = table_file();
    for (const int kind : {0, 1, 2}) {
        std::unique_ptr<openfits::input_source> source;
        if (kind == 0) {
            source = std::make_unique<openfits::memory_source>(std::vector<std::byte>(file.bytes()));
        } else {
            source = std::make_unique<counting_source>(file.bytes(), kind == 2);
        }
        const reader input(std::move(source));
        std::vector<std::vector<std::int64_t>> numbers(thread_count);
        std::vector<std::vector<double>> halves(thread_count);
        std::vector<std::vector<std::string>> names(thread_count);
        std::vector<openfits::column_arrays<int>> digits(thread_count);
        std::vector<std::int16_t> later(thread_count);
        run_threads(thread_count, [&](std::size_t t) {
            const row_range range{.first = t * 100, .count = 150};
            numbers[t] = input.read_column<std::int64_t>(1, "ROW", {.rows = range});
            halves[t] = input.read_column<double>(1, "half");
            names[t] = input.read_strings(1, 2, {.rows = range, .trim_strings = true});
            digits[t] = input.read_arrays<int>(1, 3, {.rows = range});
            later[t] = input.read_column<std::int16_t>(2 + t, 0).front();
        });
        for (std::size_t t = 0; t < thread_count; ++t) {
            ASSERT_EQ(numbers[t].size(), 150U);
            ASSERT_EQ(halves[t].size(), static_cast<std::size_t>(rows));
            ASSERT_EQ(names[t].size(), 150U);
            ASSERT_EQ(digits[t].size(), 150U);
            for (std::size_t i = 0; i < 150; ++i) {
                const std::size_t row = (t * 100) + i;
                EXPECT_EQ(numbers[t][i], static_cast<std::int64_t>(row));
                EXPECT_EQ(names[t][i], "row " + std::to_string(row));
                const std::string text = std::to_string(row);
                ASSERT_EQ(digits[t][i].size(), text.size());
                EXPECT_EQ(digits[t][i].front(), text.front() - '0');
            }
            EXPECT_EQ(halves[t][1999], 999.5);
            EXPECT_EQ(later[t], static_cast<std::int16_t>(t));
        }
    }
}

} // namespace
