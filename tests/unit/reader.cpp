// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>
#include <openfits/io.h>
#include <openfits/reader.h>

#include "support/checksums.h"
#include "support/counting_source.h"
#include "support/faulty_io.h"
#include "support/files.h"
#include "support/fixture_builder.h"
#include "support/temp_directory.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using openfits::diagnostic;
using openfits::errc;
using openfits::hdu_info;
using openfits::hdu_kind;
using openfits::invalid_data_error;
using openfits::limit_error;
using openfits::read_options;
using openfits::reader;
using openfits::severity;
using openfits::usage_error;
using openfits::test::card_record;
using openfits::test::counting_source;
using openfits::test::extension_records;
using openfits::test::fixture;
using openfits::test::primary_records;
using openfits::test::record;
using openfits::test::served_read;
using openfits::test::throws;
using openfits::test::with;

// The source owns a copy of the bytes, so that a temporary fixture can be opened.
reader open(const fixture& file, read_options options = {})
{
    return reader(std::make_unique<openfits::memory_source>(std::vector<std::byte>(file.bytes())), options);
}

// A file of a primary header without data and the images given, each of one block of 8-bit pixels and named SCI.
fixture images(std::size_t count)
{
    fixture file;
    file.primary();
    for (std::size_t i = 0; i < count; ++i) {
        file.image({2880}, {card_record("EXTNAME", "'SCI'"), card_record("EXTVER", std::to_string(i + 1))});
    }
    return file;
}

bool has_code(const std::vector<diagnostic>& diagnostics, errc code, severity level)
{
    return std::ranges::any_of(diagnostics,
                               [&](const diagnostic& found) { return found.code == code && found.severity == level; });
}

TEST(reader, reads_the_primary_header_in_one_read_and_no_data)
{
    const fixture file = fixture().header(primary_records(16, {100, 100})).data(20000);
    auto source = std::make_unique<counting_source>(file.bytes());
    const counting_source& counter = *source;
    const reader input(std::move(source));
    EXPECT_EQ(counter.reads(), (std::vector<served_read>{{.offset = 0, .size = std::uint64_t{4} * 2880}}));
    const hdu_info& primary = input.hdu(0);
    EXPECT_EQ(primary.kind(), hdu_kind::primary);
    EXPECT_EQ(primary.index(), 0U);
    EXPECT_EQ(primary.header_offset(), 0U);
    EXPECT_EQ(primary.header_size(), 2880U);
    EXPECT_EQ(primary.data_offset(), 2880U);
    EXPECT_EQ(primary.data_size(), 20000U);
    EXPECT_EQ(primary.header().value<int>("NAXIS2"), 100);
    EXPECT_TRUE(primary.diagnostics().empty());
    EXPECT_EQ(counter.reads().size(), 1U);
    EXPECT_EQ(input.size(), file.size());
}

TEST(reader, reads_a_long_header_in_reads_that_double)
{
    std::vector<std::string> records = primary_records();
    for (int i = 0; i < 300; ++i) {
        records.push_back(card_record("KEY" + std::to_string(i), std::to_string(i)));
    }
    const fixture file = fixture().header(records);
    auto source = std::make_unique<counting_source>(file.bytes());
    const counting_source& counter = *source;
    const reader input(std::move(source));
    // 304 records are 9 blocks: 4, then 4 more, then the last one.
    constexpr std::uint64_t block = 2880;
    EXPECT_EQ(counter.reads(), (std::vector<served_read>{{.offset = 0, .size = 4 * block},
                                                         {.offset = 4 * block, .size = 4 * block},
                                                         {.offset = 8 * block, .size = block}}));
    EXPECT_EQ(input.hdu(0).header_size(), 9U * 2880U);
    EXPECT_EQ(input.hdu(0).header().size(), 303U);
}

TEST(reader, walks_units_when_they_are_asked_for)
{
    const fixture file = images(3);
    auto source = std::make_unique<counting_source>(file.bytes());
    const counting_source& counter = *source;
    const reader input(std::move(source));
    constexpr std::uint64_t block = 2880;
    EXPECT_EQ(counter.reads(), (std::vector<served_read>{{.offset = 0, .size = 4 * block}}));
    // The four blocks of the first read end within the header of unit 1, which the walk reads with twice as many
    // after it: the units are small, and their headers lie within the bytes of that read, up to the end of the file.
    const hdu_info& second = input.hdu(2);
    EXPECT_EQ(counter.reads(),
              (std::vector<served_read>{{.offset = 0, .size = 4 * block}, {.offset = block, .size = 6 * block}}));
    EXPECT_EQ(second.kind(), hdu_kind::image);
    EXPECT_EQ(second.header_offset(), 3U * 2880U);
    EXPECT_EQ(second.data_size(), 2880U);
    EXPECT_EQ(second.version(), 2);
    // A walked unit is not read again, and stays where it was.
    EXPECT_EQ(&input.hdu(2), &second);
    EXPECT_EQ(input.hdu_count(), 4U);
    EXPECT_EQ(counter.reads().size(), 2U);
    EXPECT_EQ(&input.hdus()[2].get(), &second);
    EXPECT_TRUE(throws<usage_error>(errc::hdu_not_found, [&] { (void)input.hdu(4); }));
    EXPECT_TRUE(input.diagnostics().empty());
}

TEST(reader, walks_a_thousand_units)
{
    const reader input = open(images(999));
    EXPECT_EQ(input.hdu_count(), 1000U);
    EXPECT_EQ(input.hdu(999).header_offset(), 2880U + (999U * 2U * 2880U) - (2U * 2880U));
    EXPECT_EQ(input.hdu("SCI", 999).index(), 999U);
}

TEST(reader, finds_units_by_name_and_version)
{
    fixture file;
    file.header(with(primary_records(), {card_record("EXTNAME", "'PRIMARY'")}));
    file.image({1}, {card_record("EXTNAME", "'SCI'")});
    file.image({1}, {card_record("EXTNAME", "'sci   '"), card_record("EXTVER", "2")});
    file.image({1}, {card_record("HDUNAME", "'MASK'")});
    file.image({1}, {card_record("EXTNAME", "'OTHER'"), card_record("HDUNAME", "'ALIAS'")});
    file.image({1}, {card_record("EXTNAME", "'SCI'"), card_record("EXTVER", "'3'")});
    const reader input = open(file);
    EXPECT_EQ(input.hdu("PRIMARY").index(), 0U);
    EXPECT_EQ(input.hdu("Sci").index(), 1U);
    EXPECT_EQ(input.hdu("SCI", 1).index(), 1U);
    EXPECT_EQ(input.hdu("SCI", 2).index(), 2U);
    EXPECT_EQ(input.hdu("SCI", 3).index(), 5U);
    EXPECT_EQ(input.hdu("mask").index(), 3U);
    EXPECT_EQ(input.hdu("ALIAS").index(), 4U);
    EXPECT_EQ(input.hdu("OTHER").name(), "OTHER");
    EXPECT_EQ(input.hdu(3).name(), "MASK");
    EXPECT_EQ(input.hdu(2).name(), "sci");
    EXPECT_EQ(input.find_hdu("SCI", 4), std::nullopt);
    EXPECT_EQ(input.find_hdu("NONE"), std::nullopt);
    EXPECT_TRUE(throws<usage_error>(errc::hdu_not_found, [&] { (void)input.hdu("NONE"); }));
}

TEST(reader, finds_the_images_and_the_tables)
{
    fixture file;
    file.primary();
    file.image({0, 5});
    file.header(extension_records("BINTABLE", 8, {4, 2}, 0, 1)).data(8);
    file.image({3, 3});
    file.image({2});
    const reader input = open(file);
    EXPECT_EQ(input.first_image(), 3U);
    EXPECT_EQ(input.first_table(), 2U);
    EXPECT_EQ(input.images(), (std::vector<std::size_t>{3, 4}));
    EXPECT_EQ(input.tables(), (std::vector<std::size_t>{2}));
    EXPECT_EQ(input.first_image(4), 4U);
    EXPECT_EQ(input.first_image(5), std::nullopt);
    EXPECT_EQ(input.first_table(3), std::nullopt);
    EXPECT_EQ(open(fixture().header(primary_records(8, {2}))).first_image(), 0U);
    EXPECT_EQ(open(fixture().primary()).first_table(), std::nullopt);
}

TEST(reader, finds_the_next_image_walking_as_far_as_it)
{
    constexpr std::uint64_t block = 2880;
    fixture file;
    file.primary();
    for (int i = 0; i < 4; ++i) {
        file.image({100 * block});
    }
    auto source = std::make_unique<counting_source>(file.bytes());
    const counting_source& counter = *source;
    const reader input(std::move(source));
    EXPECT_EQ(input.first_image(), 1U);
    EXPECT_EQ(input.first_image(2), 2U);
    // The headers lie at blocks 0, 1, 102, 203 and 304: the walk has read no further than that of unit 2.
    EXPECT_EQ(counter.reads().back().offset, 102 * block);
    EXPECT_EQ(input.images(), (std::vector<std::size_t>{1, 2, 3, 4}));
    EXPECT_EQ(counter.reads().back().offset, 304 * block);
}

TEST(reader, lists_the_images_and_the_tables_of_a_walk_that_stops_with_its_error)
{
    fixture file;
    file.primary();
    file.image({4});
    file.header(extension_records("BINTABLE", 8, {4, 2}, 0, 1)).data(8);
    file.header(extension_records("IMAGE", 7, {10}));
    file.image({10});
    const reader input = open(file);
    EXPECT_EQ(input.first_image(), 1U);
    EXPECT_EQ(input.first_table(), 2U);
    // The unit of BITPIX 7 has axes, and no size: the walk stops after it.
    EXPECT_EQ(input.first_image(2), 3U);
    EXPECT_TRUE(throws<invalid_data_error>(errc::invalid_structure, [&] { (void)input.first_image(4); }));
    EXPECT_TRUE(throws<invalid_data_error>(errc::invalid_structure, [&] { (void)input.first_table(3); }));
    EXPECT_TRUE(throws<invalid_data_error>(errc::invalid_structure, [&] { (void)input.images(); }));
    EXPECT_TRUE(throws<invalid_data_error>(errc::invalid_structure, [&] { (void)input.tables(); }));
}

TEST(reader, reads_the_bytes_of_a_data_unit)
{
    const fixture file = images(2);
    const reader input = open(file);
    std::vector<std::byte> bytes(10);
    input.read_raw(2, 100, bytes);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        EXPECT_EQ(bytes[i], static_cast<std::byte>((100 + i) % 251));
    }
    std::vector<std::byte> all(2880);
    input.read_raw(1, 0, all);
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { input.read_raw(1, 2871, bytes); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { input.read_raw(0, 0, bytes); }));
    EXPECT_TRUE(throws<usage_error>(errc::hdu_not_found, [&] { input.read_raw(3, 0, bytes); }));
}

TEST(reader, reads_what_a_truncated_file_holds)
{
    fixture file = images(1);
    file.truncate(2 * 2880 + 1000);
    const reader input = open(file);
    EXPECT_EQ(input.hdu_count(), 2U);
    EXPECT_TRUE(has_code(input.diagnostics(), errc::truncated_file, severity::warning));
    std::vector<std::byte> present(1000);
    input.read_raw(1, 0, present);
    std::vector<std::byte> beyond(1001);
    EXPECT_TRUE(throws<openfits::io_error>(errc::end_of_data, [&] { input.read_raw(1, 0, beyond); }));
}

TEST(reader, stops_the_walk_at_a_unit_whose_header_gives_no_size)
{
    fixture file;
    file.primary();
    file.header(extension_records("IMAGE", 7, {10}));
    file.image({10});
    const reader input = open(file);
    const hdu_info& broken = input.hdu(1);
    EXPECT_EQ(broken.data_size(), std::nullopt);
    EXPECT_TRUE(has_code(broken.diagnostics(), errc::invalid_structure, severity::error));
    EXPECT_TRUE(throws<invalid_data_error>(errc::invalid_structure, [&] { (void)input.hdu(2); }));
    EXPECT_TRUE(throws<invalid_data_error>(errc::invalid_structure, [&] { (void)input.hdu("NONE"); }));
    EXPECT_EQ(input.hdu_count(), 2U);
    EXPECT_TRUE(throws<invalid_data_error>(errc::invalid_structure, [&] { (void)open(file, {.strict = true}); }));
}

TEST(reader, refuses_a_source_that_is_not_a_fits_file)
{
    EXPECT_TRUE(throws<invalid_data_error>(errc::not_a_fits_file, [] { (void)open(fixture()); }));
    EXPECT_TRUE(
        throws<invalid_data_error>(errc::not_a_fits_file, [] { (void)open(fixture().header(extension_records())); }));
    EXPECT_TRUE(throws<invalid_data_error>(errc::not_a_fits_file, [] { (void)open(fixture().append("GIF89a")); }));
    EXPECT_TRUE(throws<invalid_data_error>(errc::not_a_fits_file,
                                           [] { (void)open(fixture().header({record("SIMPLE    T")})); }));
    EXPECT_TRUE(
        throws<usage_error>(errc::invalid_argument, [] { (void)reader(std::unique_ptr<openfits::input_source>()); }));
}

TEST(reader, walks_every_unit_at_a_strict_open)
{
    const fixture file = images(3);
    auto source = std::make_unique<counting_source>(file.bytes());
    const counting_source& counter = *source;
    const reader input(std::move(source), {.strict = true});
    // The headers after the first in one read, which a unit without checksums adds none to.
    EXPECT_EQ(counter.reads().size(), 2U);
    EXPECT_EQ(input.hdu_count(), 4U);
    EXPECT_EQ(counter.reads().size(), 2U);
}

// A read of the walk that fails, whole or after half its bytes, leaves none of them behind: the walk tried again reads
// the header again, and finds the units that the file holds.
TEST(reader, a_walk_tried_again_after_a_failed_read_reads_again)
{
    const fixture file = images(3);
    const openfits::memory_source bytes(std::span<const std::byte>(file.bytes()));
    for (const openfits::test::fault kind : {openfits::test::fault::error, openfits::test::fault::short_transfer}) {
        // The first read is that of the primary header at the open; the second, that of unit 1, fails.
        const reader input(std::make_unique<openfits::test::faulty_source>(bytes, 2, kind));
        EXPECT_THROW((void)input.hdu(1), openfits::io_error);
        EXPECT_EQ(input.hdu(2).header().value<std::int64_t>("EXTVER"), 2);
        EXPECT_EQ(input.hdu(1).header().value<std::int64_t>("EXTVER"), 1);
        EXPECT_EQ(input.hdu_count(), 4U);
        EXPECT_TRUE(input.diagnostics().empty());
    }
}

// Headers far apart, after large data, are read four blocks each, as the first is; small units close together are read
// in reads that double, up to 256 KiB.
TEST(reader, reads_headers_after_large_data_alone_and_small_units_together)
{
    constexpr std::uint64_t block = 2880;
    fixture large;
    large.primary();
    for (int i = 0; i < 3; ++i) {
        large.image({100 * block});
    }
    auto source = std::make_unique<counting_source>(large.bytes());
    const counting_source& counter = *source;
    const reader few(std::move(source));
    EXPECT_EQ(few.hdu_count(), 4U);
    // The header of unit 1 starts within the first read, and takes twice its blocks; those after it lie far from the
    // bytes read before them, and take four blocks each.
    EXPECT_EQ(counter.reads(), (std::vector<served_read>{{.offset = 0, .size = 4 * block},
                                                         {.offset = block, .size = 8 * block},
                                                         {.offset = 102 * block, .size = 4 * block},
                                                         {.offset = 203 * block, .size = 4 * block}}));

    fixture small;
    small.primary();
    for (int i = 0; i < 1000; ++i) {
        small.image({100});
    }
    source = std::make_unique<counting_source>(small.bytes());
    const counting_source& reads = *source;
    const reader many(std::move(source));
    EXPECT_EQ(many.hdu_count(), 1001U);
    // 5.5 MiB of units of two blocks: each read starts at a header after the one before, and grows to 256 KiB.
    ASSERT_GE(reads.reads().size(), 2U);
    EXPECT_LT(reads.reads().size(), 32U);
    for (std::size_t i = 1; i < reads.reads().size(); ++i) {
        EXPECT_GT(reads.reads()[i].offset, reads.reads()[i - 1].offset) << i;
        EXPECT_LE(reads.reads()[i].size, std::uint64_t{1} << 18) << i;
    }
}

TEST(reader, a_verification_reports_progress_and_can_be_cancelled)
{
    // 5 MiB of data, which a verification sums in pieces of a few MiB, from memory and through reads.
    std::vector<std::int16_t> stored(std::size_t{2048} * 1280);
    for (std::size_t i = 0; i < stored.size(); ++i) {
        stored[i] = static_cast<std::int16_t>(i % 30000);
    }
    const std::vector<std::byte> file =
        openfits::test::checked_unit(primary_records(16, {2048, 1280}), openfits::test::big_endian(stored));
    for (const bool in_memory : {true, false}) {
        std::unique_ptr<openfits::input_source> source;
        if (in_memory) {
            // The file outlives the source, which borrows it.
            source = std::make_unique<openfits::memory_source>(std::span<const std::byte>(file));
        } else {
            source = std::make_unique<counting_source>(file);
        }
        const reader input(std::move(source));
        std::vector<std::pair<std::uint64_t, std::uint64_t>> calls;
        const openfits::checksum_report report =
            input.verify(0, {.progress = [&](std::uint64_t done, std::uint64_t total) {
                             calls.emplace_back(done, total);
                             return true;
                         }});
        EXPECT_EQ(report.datasum, openfits::checksum_state::valid);
        EXPECT_EQ(report.checksum, openfits::checksum_state::valid);
        // The start, the header, and the data records in two pieces.
        ASSERT_GE(calls.size(), 4U);
        EXPECT_EQ(calls.front(), (std::pair<std::uint64_t, std::uint64_t>{0, file.size()}));
        EXPECT_EQ(calls.back(), (std::pair<std::uint64_t, std::uint64_t>{file.size(), file.size()}));
        EXPECT_TRUE(std::ranges::is_sorted(calls));

        EXPECT_TRUE(throws<openfits::cancelled_error>(errc::cancelled, [&] {
            (void)input.verify(0, {.progress = [](std::uint64_t, std::uint64_t) { return false; }});
        }));
        EXPECT_TRUE(throws<openfits::cancelled_error>(errc::cancelled, [&] {
            (void)input.verify(0, {.progress = [](std::uint64_t done, std::uint64_t) { return done == 0; }});
        }));
    }
}

TEST(reader, keeps_the_limits_of_the_walk)
{
    const fixture file = images(3);
    const reader few = open(file, {.limits = {.max_hdus = 2}});
    EXPECT_EQ(few.hdu_count(), 2U);
    EXPECT_TRUE(throws<limit_error>(errc::too_many_hdus, [&] { (void)few.hdu(2); }));
    EXPECT_TRUE(has_code(few.diagnostics(), errc::too_many_hdus, severity::error));
    EXPECT_TRUE(throws<limit_error>(errc::too_many_hdus,
                                    [&] { (void)open(file, {.limits = {.max_hdus = 2}, .strict = true}); }));
    EXPECT_EQ(open(file, {.limits = {.max_hdus = 0}}).hdu_count(), 4U);

    const reader total = open(file, {.limits = {.max_total_header_bytes = std::uint64_t{3} * 2880}});
    EXPECT_EQ(total.hdu_count(), 3U);
    EXPECT_TRUE(throws<limit_error>(errc::header_too_large, [&] { (void)total.hdu(3); }));
    EXPECT_TRUE(throws<limit_error>(errc::header_too_large,
                                    [&] { (void)open(file, {.limits = {.max_total_header_bytes = 2879}}); }));

    std::vector<std::string> records = extension_records();
    for (int i = 0; i < 40; ++i) {
        records.push_back(card_record("KEY" + std::to_string(i), "1"));
    }
    fixture large;
    large.primary().header(records);
    const reader header_limit = open(large, {.limits = {.max_header_bytes = 2880}});
    EXPECT_TRUE(throws<limit_error>(errc::header_too_large, [&] { (void)header_limit.hdu(1); }));
    EXPECT_EQ(open(large, {.limits = {.max_header_bytes = 5760}}).hdu(1).header_size(), 5760U);
    EXPECT_EQ(open(large, {.limits = {.max_header_bytes = 0}}).hdu_count(), 2U);
    EXPECT_TRUE(throws<limit_error>(errc::header_too_large,
                                    [&] { (void)open(large, {.limits = {.max_header_bytes = 1000}}); }));
}

TEST(reader, limits_the_diagnostics_of_the_file)
{
    std::vector<std::string> records = primary_records();
    for (int i = 0; i < 10; ++i) {
        records.push_back(record("lower" + std::to_string(i) + "  =                    1"));
    }
    fixture file;
    file.header(records);
    file.header(with(extension_records(),
                     {record("other   =                    1"), record("again   =                    1")}));
    const reader input = open(file, {.limits = {.max_diagnostics = 4}});
    const std::vector<diagnostic>& primary = input.hdu(0).diagnostics();
    ASSERT_EQ(primary.size(), 4U);
    EXPECT_NE(primary.back().message.find('7'), std::string::npos);
    // The budget of the file is spent: the next unit gets one, which stands for its two.
    EXPECT_EQ(input.hdu(1).diagnostics().size(), 1U);
    EXPECT_EQ(open(file, {.limits = {.max_diagnostics = 0}}).hdu(0).diagnostics().size(), 10U);
}

TEST(reader, opens_a_file_by_its_literal_path)
{
    const openfits::test::temp_directory directory;
    const std::string path = directory.file("M31 [Ha].fits");
    openfits::test::write_file(openfits::test::path_of(path), images(1).bytes());
    const reader input(path);
    EXPECT_EQ(input.hdu_count(), 2U);
    EXPECT_TRUE(throws<openfits::io_error>(errc::open_failed, [&] { (void)reader(directory.file("absent.fits")); }));
}

TEST(reader, moves)
{
    reader first = open(images(1));
    const hdu_info& unit = first.hdu(1);
    reader second(std::move(first));
    EXPECT_EQ(&second.hdu(1), &unit);
    reader third = open(fixture().primary());
    third = std::move(second);
    EXPECT_EQ(third.hdu_count(), 2U);
}

} // namespace
