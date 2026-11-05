// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/checksum.h>
#include <openfits/editor.h>
#include <openfits/error.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/verify.h>
#include <openfits/writer.h>

#include "checksum/checksum.h"
#include "edit/header_edit.h"
#include "edit/patch.h"
#include "support/faulty_io.h"
#include "support/files.h"
#include "support/fixture_builder.h"
#include "support/temp_directory.h"
#include "support/test_devices.h"
#include "support/throws.h"
#include "support/written_file.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The commits of an editor: what they read and write, a failure, a cancellation or a crash at each point, and files
// beyond 4 GiB.

namespace {

using openfits::checksum_state;
using openfits::editor;
using openfits::editor_options;
using openfits::errc;
using openfits::hdu_kind;
using openfits::image_info;
using openfits::reader;
using openfits::region;
using openfits::relayout;
using openfits::usage_error;
using openfits::writer;
using openfits::test::throws;
using openfits::test::written;

reader open(std::vector<std::byte> bytes)
{
    return reader(std::make_unique<openfits::memory_source>(std::move(bytes)));
}

// Every file that an edit makes conforms: its report holds no finding.
void expect_clean(const std::vector<std::byte>& bytes)
{
    openfits::test::record_file(bytes);
    const reader file = open(bytes);
    for (const openfits::diagnostic& finding : openfits::verify(file)) {
        ADD_FAILURE() << "the edited file departs from standard " << finding.section << ": " << finding.message;
    }
}

image_info image_of(std::vector<std::uint64_t> axes, std::string_view name = {})
{
    image_info info;
    info.geometry.axes = std::move(axes);
    if (!name.empty()) {
        info.header.set("EXTNAME", name);
    }
    return info;
}

// The value of the pixel (x, y) of the images that the tests fill.
std::uint16_t pixel(std::uint64_t x, std::uint64_t y)
{
    return static_cast<std::uint16_t>(((x * 31) + (y * 17)) % 60000);
}

TEST(editor_commit, fills_a_reserved_image_region_by_region_as_one_write_would_and_reads_little)
{
    constexpr std::uint64_t width = 320;
    constexpr std::uint64_t height = 200;
    image_info info = image_of({width, height});
    info.bitpix = openfits::bitpix::int16;
    info.scaling.zero = 32768;
    info.header.set("OBJECT", "a mosaic");
    writer reserved;
    (void)reserved.add_reserved_image(info);
    auto device = std::make_unique<openfits::test::counting_device>(written(reserved, {.checksums = true}));
    const openfits::test::counting_device& counted = *device;
    editor file(std::move(device));
    const std::size_t opening = counted.reads();

    // A thousand regions of 16 x 4, each committed alone.
    std::vector<std::uint16_t> values(std::size_t{16} * 4);
    for (std::uint64_t top = 0; top < height; top += 4) {
        for (std::uint64_t left = 0; left < width; left += 16) {
            for (std::uint64_t y = 0; y < 4; ++y) {
                for (std::uint64_t x = 0; x < 16; ++x) {
                    values[(y * 16) + x] = pixel(left + x, top + y);
                }
            }
            file.write_pixels(0, std::span<const std::uint16_t>(values),
                              {.region = region{.start = {left, top}, .size = {16, 4}}});
            file.commit();
        }
    }
    // Each commit reads the rows of its region at once, the header that it rewrites, and the header again to open
    // the file as it now is: a bound on the reads of a region whatever the size of the image.
    EXPECT_LE(counted.reads() - opening, std::size_t{1000} * 4);

    std::vector<std::uint16_t> whole(width * height);
    for (std::uint64_t y = 0; y < height; ++y) {
        for (std::uint64_t x = 0; x < width; ++x) {
            whole[(y * width) + x] = pixel(x, y);
        }
    }
    info.bitpix.reset();
    info.scaling = {};
    writer at_once;
    (void)at_once.add_image(info, std::span<const std::uint16_t>(whole));
    EXPECT_EQ(counted.data(), written(at_once, {.checksums = true}));
    expect_clean(counted.data());
}

TEST(editor_commit, writes_at_odd_byte_offsets_with_the_checksums_updated)
{
    std::vector<std::uint8_t> values(41);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = static_cast<std::uint8_t>(i * 5);
    }
    writer output;
    (void)output.add_image(image_of({41}), std::span<const std::uint8_t>(values));
    auto device = std::make_unique<openfits::memory_device>(written(output, {.checksums = true}));
    const openfits::memory_device& memory = *device;
    editor file(std::move(device));
    for (const std::uint64_t start : {1U, 3U, 6U, 13U, 38U}) {
        const std::vector<std::uint8_t> patch{0xFF, 0x01, 0x80};
        file.write_pixels(0, std::span<const std::uint8_t>(patch), {.region = region{.start = {start}, .size = {3}}});
        file.commit();
        const std::vector<std::byte> bytes(memory.data().begin(), memory.data().end());
        const reader edited = open(bytes);
        EXPECT_EQ(edited.verify(0).datasum, checksum_state::valid) << start;
        EXPECT_EQ(edited.verify(0).checksum, checksum_state::valid) << start;
    }
    expect_clean({memory.data().begin(), memory.data().end()});
}

TEST(editor_commit, completes_a_truncated_last_block_before_it_appends)
{
    std::vector<std::int16_t> values(100, 3);
    writer output;
    (void)output.add_image(image_of({100}), std::span<const std::int16_t>(values));
    std::vector<std::byte> bytes = written(output);
    // The fill of the last block cut short, as a truncated file has it.
    bytes.resize(bytes.size() - 2000);
    auto device = std::make_unique<openfits::memory_device>(std::move(bytes));
    const openfits::memory_device& memory = *device;
    editor file(std::move(device));
    std::vector<float> more{1.0F, 2.0F};
    writer appended;
    (void)appended.add_image(image_of({2}, "MORE"), std::span<const float>(more));
    file.append(std::move(appended));
    file.commit();
    const std::vector<std::byte> edited(memory.data().begin(), memory.data().end());
    EXPECT_EQ(edited.size(), 4U * 2880);
    const reader result = open(edited);
    EXPECT_EQ(result.read_pixels<std::int16_t>(0), values);
    EXPECT_EQ(result.read_pixels<float>(1), more);
    expect_clean(edited);
}

// The data that a header declares and the file lacks are not made up: a commit that would write them, for a unit after
// them or a write among them, is refused, as a copy refuses them, whatever their size, and leaves its changes pending;
// a change of the header and writes into the bytes that the file holds leave the unit as short as it was.
TEST(editor_commit, refuses_to_write_the_data_that_a_truncated_file_lacks)
{
    std::vector<std::string> records = openfits::test::primary_records(8, {1'000'000'000'000});
    openfits::test::fixture builder;
    builder.header(records).data(100);
    std::vector<std::byte> bytes = builder.bytes();
    bytes.resize(2880 + 100);
    auto device = std::make_unique<openfits::memory_device>(std::move(bytes));
    const openfits::memory_device& memory = *device;
    editor file(std::move(device));
    std::vector<std::uint8_t> more(3, 7);
    writer appended;
    (void)appended.add_image(image_of({3}, "MORE"), std::span<const std::uint8_t>(more));
    (void)file.append(std::move(appended));
    EXPECT_TRUE(throws<openfits::io_error>(errc::end_of_data, [&file] { file.commit(); }));
    EXPECT_TRUE(file.has_pending_changes());
    EXPECT_EQ(memory.data().size(), 2880U + 100);
    const std::vector<std::byte> raw{std::byte{1}};
    file.discard();
    file.write_raw(0, 500, raw);
    EXPECT_TRUE(throws<openfits::io_error>(errc::end_of_data, [&file] { file.commit(); }));
    file.discard();
    file.write_raw(0, 50, raw);
    file.header(0).set("OBJECT", "cut short");
    file.commit();
    EXPECT_EQ(memory.data().size(), 2880U + 100);
    EXPECT_EQ(memory.data()[2880 + 50], std::byte{1});
    EXPECT_EQ(file.file().hdu(0).header().value<std::string>("OBJECT"), "cut short");
}

TEST(editor_commit, keeps_special_records_after_the_last_unit)
{
    std::vector<std::uint8_t> values(10, 1);
    writer output;
    (void)output.add_image(image_of({10}), std::span<const std::uint8_t>(values));
    std::vector<std::byte> bytes = written(output);
    const std::vector<std::byte> special(2880, std::byte{'#'});
    bytes.insert(bytes.end(), special.begin(), special.end());
    auto device = std::make_unique<openfits::memory_device>(std::move(bytes));
    const openfits::memory_device& memory = *device;
    editor file(std::move(device));
    std::vector<std::uint8_t> more(3, 2);
    writer appended;
    (void)appended.add_image(image_of({3}, "MORE"), std::span<const std::uint8_t>(more));
    file.append(std::move(appended));
    file.commit();
    const std::vector<std::byte> edited(memory.data().begin(), memory.data().end());
    ASSERT_EQ(edited.size(), 5U * 2880);
    EXPECT_TRUE(std::equal(special.begin(), special.end(), edited.end() - 2880));
    const reader result = open(edited);
    ASSERT_EQ(result.hdu_count(), 2U);
    EXPECT_EQ(result.read_pixels<std::uint8_t>(1), more);
}

// The values of the unit that the tests of failures append, which its writer borrows until the commit.
const std::vector<std::int32_t>& appended_values()
{
    static const std::vector<std::int32_t> values{1, 2, 3};
    return values;
}

// The changes of the tests of failures: a keyword, the values of a region, and a unit appended.
void make_changes(editor& file)
{
    file.header(0).set("OBJECT", "changed");
    const std::vector<std::uint16_t> patch{9, 8, 7, 6};
    file.write_pixels(0, std::span<const std::uint16_t>(patch), {.region = region{.start = {3, 3}, .size = {2, 2}}});
    writer appended;
    (void)appended.add_image(image_of({3}, "MORE"), std::span<const std::int32_t>(appended_values()));
    file.append(std::move(appended));
}

std::vector<std::byte> failure_file()
{
    std::vector<std::uint16_t> values(std::size_t{37} * 23);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = static_cast<std::uint16_t>(i);
    }
    image_info info = image_of({37, 23});
    info.header.set("OBJECT", "original");
    writer output;
    (void)output.add_image(info, std::span<const std::uint16_t>(values));
    return written(output, {.checksums = true});
}

TEST(editor_commit, a_commit_in_place_that_fails_is_completed_by_the_next_one)
{
    const std::vector<std::byte> original = failure_file();
    std::vector<std::byte> expected;
    {
        auto device = std::make_unique<openfits::memory_device>(std::vector<std::byte>(original));
        const openfits::memory_device& memory = *device;
        editor file(std::move(device));
        make_changes(file);
        file.commit();
        expected.assign(memory.data().begin(), memory.data().end());
    }
    for (std::size_t failing = 1;; ++failing) {
        openfits::memory_device inner{std::vector<std::byte>(original)};
        editor file(
            std::make_unique<openfits::test::faulty_device>(inner, failing, openfits::test::fault::short_transfer));
        make_changes(file);
        bool failed = false;
        try {
            file.commit();
        } catch (const openfits::io_error&) {
            failed = true;
        }
        if (failed) {
            EXPECT_TRUE(file.has_pending_changes());
            file.commit();
        }
        const std::vector<std::byte> result(inner.data().begin(), inner.data().end());
        const auto mismatch = std::ranges::mismatch(result, expected);
        ASSERT_EQ(result, expected) << "failing call " << failing << ", first difference at "
                                    << (mismatch.in1 - result.begin()) << " of " << result.size() << " and "
                                    << expected.size();
        if (!failed) {
            break;
        }
    }
    expect_clean(expected);
}

TEST(editor_commit, a_failed_or_cancelled_copy_leaves_the_old_file_and_a_finished_one_the_new_one)
{
    const openfits::test::temp_directory directory;
    const std::string path = directory.file("copied.fits");
    const std::vector<std::byte> original = failure_file();
    for (const bool atomic : {false, true}) {
        std::vector<std::byte> expected;
        for (std::uint64_t cancelling = 1;; ++cancelling) {
            openfits::test::write_file(openfits::test::path_of(path), original);
            editor file(path, {.atomic = atomic});
            make_changes(file);
            // A header that grows by a block, which moves every unit after it.
            for (int i = 0; i < 40; ++i) {
                file.header(0).append(openfits::card("KEY" + std::to_string(i), i));
            }
            std::uint64_t calls = 0;
            bool cancelled = false;
            try {
                file.commit(
                    {.progress = [&calls, cancelling](std::uint64_t, std::uint64_t) { return ++calls != cancelling; }});
            } catch (const openfits::cancelled_error&) {
                cancelled = true;
            }
            const std::string text = openfits::test::read_file(openfits::test::path_of(path));
            const std::span<const std::byte> now = std::as_bytes(std::span(text));
            if (cancelled) {
                // The old file, as it was, and no temporary file left.
                ASSERT_TRUE(std::ranges::equal(now, original)) << cancelling;
                EXPECT_EQ(directory.entries(), 1U);
                EXPECT_TRUE(file.has_pending_changes());
                continue;
            }
            expected.assign(now.begin(), now.end());
            const reader edited(path);
            EXPECT_EQ(edited.hdu(0).header().value<std::string>("OBJECT"), "changed");
            EXPECT_EQ(edited.hdu(0).header().value<int>("KEY39"), 39);
            EXPECT_EQ(edited.hdu_count(), 2U);
            EXPECT_EQ(directory.entries(), 1U);
            break;
        }
        expect_clean(expected);
    }
}

TEST(editor_commit, an_atomic_commit_replaces_the_file_that_a_reader_holds_open)
{
    const openfits::test::temp_directory directory;
    const std::string path = directory.file("atomic.fits");
    openfits::test::write_file(openfits::test::path_of(path), failure_file());
    const reader before(path);
    editor file(path, {.atomic = true});
    file.header(0).set("OBJECT", "replaced");
    file.commit();
    // The reader opened before reads the old file, which the copy replaced; a reader opened now reads the new one.
    EXPECT_EQ(before.hdu(0).header().value<std::string>("OBJECT"), "original");
    EXPECT_EQ(before.read_pixels<std::uint16_t>(0)[5], 5);
    EXPECT_EQ(reader(path).hdu(0).header().value<std::string>("OBJECT"), "replaced");
}

TEST(editor_commit, a_failure_while_units_move_in_place_leaves_the_editor_refusing_changes)
{
    const std::vector<std::byte> original = failure_file();
    openfits::memory_device inner{std::vector<std::byte>(original)};
    // The resize to the new size, then the first write of the moves.
    editor file(std::make_unique<openfits::test::faulty_device>(inner, 2, openfits::test::fault::error),
                {.relayout = relayout::in_place});
    for (int i = 0; i < 40; ++i) {
        file.header(0).append(openfits::card("KEY" + std::to_string(i), i));
    }
    EXPECT_THROW(file.commit(), openfits::io_error);
    EXPECT_TRUE(throws<usage_error>(errc::not_editable, [&file] { file.commit(); }));
    EXPECT_TRUE(throws<usage_error>(errc::not_editable, [&file] { (void)file.header(0); }));
}

TEST(editor_commit, a_device_that_cannot_change_its_size_takes_the_changes_that_keep_it)
{
    std::vector<std::byte> bytes = failure_file();
    editor file(std::make_unique<openfits::memory_device>(std::span<std::byte>(bytes)));
    file.header(0).set("OBJECT", "in place");
    file.commit();
    EXPECT_EQ(open(bytes).hdu(0).header().value<std::string>("OBJECT"), "in place");
    std::vector<std::uint8_t> more(3, 2);
    writer appended;
    (void)appended.add_image(image_of({3}), std::span<const std::uint8_t>(more));
    file.append(std::move(appended));
    const std::vector<std::byte> before = bytes;
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&file] { file.commit(); }));
    EXPECT_EQ(bytes, before);
}

TEST(editor_commit, leaves_the_checksums_as_they_are_when_asked)
{
    auto device = std::make_unique<openfits::memory_device>(failure_file());
    const openfits::memory_device& memory = *device;
    editor file(std::move(device), {.update_checksums = false});
    const std::vector<std::uint16_t> patch{1};
    file.write_pixels(0, std::span<const std::uint16_t>(patch), {.region = region{.start = {0, 0}, .size = {1, 1}}});
    file.commit();
    const reader edited = open({memory.data().begin(), memory.data().end()});
    EXPECT_EQ(edited.verify(0).datasum, checksum_state::invalid);
}

TEST(editor_commit, pending_changes_are_what_differs_until_a_commit_or_a_discard)
{
    std::vector<std::byte> bytes = failure_file();
    editor file(std::make_unique<openfits::memory_device>(std::move(bytes)));
    // A header asked for and left as it is changes nothing.
    (void)file.header(0);
    EXPECT_FALSE(file.has_pending_changes());
    // The last unit of a file cannot be erased.
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&file] { file.erase_hdu(0); }));
    std::vector<std::uint8_t> more(3, 2);
    writer appended;
    (void)appended.add_image(image_of({3}), std::span<const std::uint8_t>(more));
    file.append(std::move(appended));
    EXPECT_TRUE(file.has_pending_changes());
    // With a unit appended, the first one can be erased, which leaves no change to it possible until the commit.
    file.erase_hdu(0);
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&file] { (void)file.header(0); }));
    file.discard();
    EXPECT_FALSE(file.has_pending_changes());
    EXPECT_NO_THROW((void)file.header(0));
}

TEST(editor_commit, random_groups_take_keywords_and_stay_first)
{
    // Random groups of 2 groups of 1 parameter and a 3-element array, of bytes.
    std::vector<std::string> records = openfits::test::primary_records(8, {0, 3});
    records.push_back(openfits::test::card_record("GROUPS", "T"));
    records.push_back(openfits::test::card_record("PCOUNT", "1"));
    records.push_back(openfits::test::card_record("GCOUNT", "2"));
    openfits::test::fixture builder;
    builder.header(records).data(8);
    auto device = std::make_unique<openfits::memory_device>(std::vector<std::byte>(builder.bytes()));
    const openfits::memory_device& memory = *device;
    editor file(std::move(device));
    ASSERT_EQ(file.file().hdu(0).kind(), hdu_kind::random_groups);
    file.header(0).set("OBJECT", "groups");
    file.commit();
    EXPECT_EQ(open({memory.data().begin(), memory.data().end()}).hdu(0).header().value<std::string>("OBJECT"),
              "groups");
    const std::vector<std::uint8_t> values{1};
    EXPECT_TRUE(throws<usage_error>(errc::not_an_image,
                                    [&file, &values] { file.write_pixels(0, std::span<const std::uint8_t>(values)); }));
    writer before;
    (void)before.add_header({});
    EXPECT_TRUE(
        throws<usage_error>(errc::invalid_argument, [&file, &before] { file.insert_hdu(0, std::move(before)); }));
}

TEST(editor_commit, edits_a_file_beyond_4_gib_through_a_virtual_device)
{
    // A 16-bit image of 65536 x 36000 zeros, 4.7 GB, its checksums those of zeros.
    constexpr std::uint64_t width = 65536;
    constexpr std::uint64_t height = 36000;
    openfits::header cards(std::vector<openfits::card>{
        openfits::card::parse(openfits::test::card_record("SIMPLE", "T")),
        openfits::card::parse(openfits::test::card_record("BITPIX", "16")),
        openfits::card::parse(openfits::test::card_record("NAXIS", "2")),
        openfits::card::parse(openfits::test::card_record("NAXIS1", std::to_string(width))),
        openfits::card::parse(openfits::test::card_record("NAXIS2", std::to_string(height))),
        openfits::card::parse(openfits::test::card_record("OBJECT", "'zeros   '")),
        openfits::card::parse(openfits::test::card_record("CHECKSUM", "'0000000000000000'")),
        openfits::card::parse(openfits::test::card_record("DATASUM", "'0'"))});
    std::string blocks = openfits::detail::edited_blocks(cards, {}, false, 1);
    openfits::detail::seal_checksum(blocks, 0, 0xFFFFFFFFU);
    const std::span<const std::byte> header_bytes = std::as_bytes(std::span(blocks));
    const std::uint64_t data = width * height * 2;
    const std::uint64_t size = 2880 + ((data + 2879) / 2880 * 2880);
    auto device = std::make_unique<openfits::test::virtual_device>(
        std::vector<std::byte>(header_bytes.begin(), header_bytes.end()), size);
    const openfits::test::virtual_device& memory = *device;
    editor file(std::move(device));
    ASSERT_EQ(file.file().hdu(0).data_size(), data);

    // A keyword in place, values of the last row, beyond 4 GiB, and a unit appended at the end.
    file.header(0).set("OBJECT", "large");
    std::vector<std::int16_t> row(10);
    for (std::size_t i = 0; i < row.size(); ++i) {
        row[i] = static_cast<std::int16_t>(1000 + i);
    }
    file.write_pixels(0, std::span<const std::int16_t>(row),
                      {.region = region{.start = {65000, height - 1}, .size = {10, 1}}});
    std::vector<std::uint8_t> small(4, 9);
    writer appended;
    (void)appended.add_image(image_of({4}, "SMALL"), std::span<const std::uint8_t>(small));
    file.append(std::move(appended));
    file.commit();

    const reader& edited = file.file();
    ASSERT_EQ(edited.hdu_count(), 2U);
    EXPECT_EQ(edited.hdu(0).header().value<std::string>("OBJECT"), "large");
    EXPECT_EQ(edited.read_pixels<std::int16_t>(0, {.region = region{.start = {65000, height - 1}, .size = {10, 1}}}),
              row);
    EXPECT_EQ(edited.read_pixels<std::uint8_t>(1), small);
    EXPECT_GT(edited.hdu(1).header_offset(), std::uint64_t{4} << 30);
    // DATASUM records the sum of the values written, the rest zeros, and CHECKSUM makes the unit negative zero.
    const std::uint64_t offset = (((height - 1) * width) + 65000) * 2;
    std::vector<std::byte> written_bytes(20);
    edited.read_raw(0, offset, written_bytes);
    const std::uint32_t sum = openfits::detail::sum_at(written_bytes, offset);
    EXPECT_EQ(edited.hdu(0).header().value<std::string>("DATASUM"), std::to_string(sum));
    std::vector<std::byte> header_now(2880);
    memory.read(0, header_now);
    EXPECT_EQ(openfits::detail::add_sums(openfits::detail::sum_of(header_now), sum), 0xFFFFFFFFU);
    EXPECT_LT(memory.held(), std::uint64_t{1} << 20);
}

} // namespace
