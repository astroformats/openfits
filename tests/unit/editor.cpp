// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/checksum.h>
#include <openfits/editor.h>
#include <openfits/error.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/table.h>
#include <openfits/verify.h>
#include <openfits/writer.h>

#include "support/files.h"
#include "support/fixture_builder.h"
#include "support/temp_directory.h"
#include "support/throws.h"
#include "support/written_file.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The editor: keywords, values, rows and units changed, then committed together, in place where the layout of the file
// allows it, and through a copy or by moving its units otherwise.

namespace {

using openfits::bitpix;
using openfits::checksum_state;
using openfits::column_format;
using openfits::column_type;
using openfits::column_values;
using openfits::editor;
using openfits::editor_options;
using openfits::errc;
using openfits::hdu_kind;
using openfits::image_info;
using openfits::reader;
using openfits::region;
using openfits::relayout;
using openfits::table_info;
using openfits::usage_error;
using openfits::validation_error;
using openfits::write_options;
using openfits::writer;
using openfits::test::throws;
using openfits::test::written;

// An editor of a file in memory, and the bytes of the file as each commit leaves them.
class memory_file
{
public:
    explicit memory_file(std::vector<std::byte> bytes, const editor_options& options = {})
        : memory_file(std::make_unique<openfits::memory_device>(std::move(bytes)), options)
    {}

    [[nodiscard]] editor& edit() noexcept
    {
        return editor_;
    }

    [[nodiscard]] std::vector<std::byte> bytes() const
    {
        const std::span<const std::byte> data = device_->data();
        return {data.begin(), data.end()};
    }

    // A reader of the file as it is, and its report, which must be clean: every file that the editor writes conforms.
    [[nodiscard]] reader read() const
    {
        std::vector<std::byte> file = bytes();
        openfits::test::record_file(file);
        reader opened(std::make_unique<openfits::memory_source>(std::move(file)));
        for (const openfits::diagnostic& finding : openfits::verify(opened)) {
            ADD_FAILURE() << "the edited file departs from standard " << finding.section << ": " << finding.message;
        }
        return opened;
    }

private:
    // The device is kept before the editor takes it, the members initialized in their order.
    memory_file(std::unique_ptr<openfits::memory_device> device, const editor_options& options)
        : device_(device.get()), editor_(std::move(device), options)
    {}

    openfits::memory_device* device_ = nullptr;
    editor editor_;
};

image_info image_of(std::vector<std::uint64_t> axes, std::string_view name = {})
{
    image_info info;
    info.geometry.axes = std::move(axes);
    if (!name.empty()) {
        info.header.set("EXTNAME", name);
    }
    return info;
}

// A file of a 16-bit image of 37 x 23 values i x 7, with OBJECT and the given reserved cards, and checksums on request.
std::vector<std::byte> image_file(bool checksums = false, std::size_t reserved_cards = 0)
{
    std::vector<std::uint16_t> values(std::size_t{37} * 23);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = static_cast<std::uint16_t>(i * 7);
    }
    image_info info = image_of({37, 23});
    info.header.set("OBJECT", "M 31", "the source");
    writer output;
    (void)output.add_image(info, std::span<const std::uint16_t>(values), {.reserved_cards = reserved_cards});
    return written(output, {.checksums = checksums});
}

// The indices of the bytes where two files differ.
std::vector<std::size_t> differences(const std::vector<std::byte>& a, const std::vector<std::byte>& b)
{
    std::vector<std::size_t> found;
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
        if (a[i] != b[i]) {
            found.push_back(i);
        }
    }
    return found;
}

TEST(editor, changes_a_keyword_in_place_writing_its_header_block_alone)
{
    const std::vector<std::byte> original = image_file();
    memory_file file(original);
    file.edit().header(0).set("OBJECT", "NGC 224");
    EXPECT_TRUE(file.edit().has_pending_changes());
    // Pending until the commit.
    EXPECT_EQ(file.edit().file().hdu(0).header().value<std::string>("OBJECT"), "M 31");
    file.edit().commit();
    EXPECT_FALSE(file.edit().has_pending_changes());

    const std::vector<std::byte> edited = file.bytes();
    ASSERT_EQ(edited.size(), original.size());
    for (const std::size_t at : differences(original, edited)) {
        EXPECT_LT(at, 2880U);
    }
    EXPECT_EQ(file.read().hdu(0).header().value<std::string>("OBJECT"), "NGC 224");
    EXPECT_EQ(file.edit().file().hdu(0).header().value<std::string>("OBJECT"), "NGC 224");
    // The comment of the card is kept.
    EXPECT_EQ(file.edit().file().hdu(0).header().comment("OBJECT"), "the source");
}

TEST(editor, adds_keywords_in_the_blank_cards_before_end_then_grows_the_header_by_a_block)
{
    // The header of the writer: 6 records of the image, OBJECT, 3 blank cards, END; padded to 36 records.
    const std::vector<std::byte> original = image_file(false, 3);
    memory_file file(original);
    const std::vector<std::uint16_t> values =
        reader(std::make_unique<openfits::memory_source>(original)).read_pixels<std::uint16_t>(0);
    for (int i = 0; i < 3; ++i) {
        file.edit().header(0).append(openfits::card("KEY" + std::to_string(i), i));
    }
    file.edit().commit();
    EXPECT_EQ(file.bytes().size(), original.size());
    EXPECT_EQ(file.edit().file().hdu(0).header().free_cards(), 0U);

    // The header fills its block, END included, then a card more takes a block more, and the data move after it.
    int key = 3;
    while (file.edit().header(0).size() < 35) {
        file.edit().header(0).append(openfits::card("KEY" + std::to_string(key), key));
        ++key;
    }
    file.edit().commit();
    EXPECT_EQ(file.bytes().size(), original.size());
    file.edit().header(0).append(openfits::card("LAST", key));
    file.edit().commit();
    EXPECT_EQ(file.bytes().size(), original.size() + 2880);
    const reader edited = file.read();
    EXPECT_EQ(edited.hdu(0).header_size(), 5760U);
    EXPECT_EQ(edited.hdu(0).header().value<int>("LAST"), key);
    EXPECT_EQ(edited.read_pixels<std::uint16_t>(0), values);
}

TEST(editor, a_header_that_shrinks_keeps_its_blocks)
{
    std::vector<std::uint8_t> values(10, 1);
    image_info info = image_of({10});
    for (int i = 0; i < 60; ++i) {
        info.header.set("KEY" + std::to_string(i), i);
    }
    writer output;
    (void)output.add_image(info, std::span<const std::uint8_t>(values));
    const std::vector<std::byte> original = written(output);
    memory_file file(original);
    for (int i = 0; i < 60; ++i) {
        ASSERT_TRUE(file.edit().header(0).erase("KEY" + std::to_string(i)));
    }
    file.edit().commit();
    const reader edited = file.read();
    EXPECT_EQ(file.bytes().size(), original.size());
    EXPECT_EQ(edited.hdu(0).header_size(), 5760U);
    EXPECT_FALSE(edited.hdu(0).header().contains("KEY0"));
    EXPECT_EQ(edited.read_pixels<std::uint8_t>(0), values);
}

TEST(editor, refuses_a_header_that_changes_its_structure_and_keeps_the_change_pending)
{
    const std::vector<std::byte> original = image_file();
    const auto refused = [&original](const std::function<void(openfits::header&)>& change) {
        memory_file file(original);
        change(file.edit().header(0));
        const bool thrown = throws<validation_error>(errc::structural_keyword, [&file] { file.edit().commit(); });
        EXPECT_EQ(file.bytes(), original);
        EXPECT_TRUE(file.edit().has_pending_changes());
        file.edit().discard();
        EXPECT_FALSE(file.edit().has_pending_changes());
        file.edit().commit();
        EXPECT_EQ(file.bytes(), original);
        return thrown;
    };
    EXPECT_TRUE(refused([](openfits::header& cards) { cards.set("NAXIS1", 38); }));
    EXPECT_TRUE(refused([](openfits::header& cards) { cards.set("BITPIX", 32); }));
    EXPECT_TRUE(refused([](openfits::header& cards) { (void)cards.erase("BZERO"); }));
    EXPECT_TRUE(refused([](openfits::header& cards) { cards.set("BSCALE", 2.0); }));
    EXPECT_TRUE(refused([](openfits::header& cards) { cards.insert(2, openfits::card("FIRST", 1)); }));
    // The value of a structural keyword given again, its comment changed, is the same structure.
    memory_file file(original);
    file.edit().header(0).set("NAXIS1", 37, "the first axis");
    file.edit().commit();
    EXPECT_EQ(file.read().hdu(0).header().comment("NAXIS1"), "the first axis");
}

TEST(editor, writes_the_values_of_a_region_in_place)
{
    const std::vector<std::byte> original = image_file();
    memory_file file(original);
    std::vector<std::uint16_t> patch{1, 2, 3, 4, 5, 6};
    file.edit().write_pixels(0, std::span<const std::uint16_t>(patch),
                             {.region = region{.start = {10, 5}, .size = {3, 2}}});
    file.edit().commit();
    const reader edited = file.read();
    const std::vector<std::uint16_t> values = edited.read_pixels<std::uint16_t>(0);
    for (std::size_t y = 0; y < 23; ++y) {
        for (std::size_t x = 0; x < 37; ++x) {
            const std::size_t i = (y * 37) + x;
            const bool inside = x >= 10 && x < 13 && y >= 5 && y < 7;
            const std::uint16_t expected = inside ? patch[((y - 5) * 3) + (x - 10)] : static_cast<std::uint16_t>(i * 7);
            ASSERT_EQ(values[i], expected) << x << ", " << y;
        }
    }
    // The header is as it was: no checksums to update.
    for (const std::size_t at : differences(original, file.bytes())) {
        EXPECT_GE(at, 2880U);
    }
}

TEST(editor, writes_regions_with_steps_reversed_axes_and_a_pitch)
{
    const std::vector<std::byte> original = image_file();
    memory_file file(original);
    // Every other element of 3 rows, the rows in reverse, from rows of 5 values in the caller's buffer.
    const std::vector<std::uint16_t> buffer{10, 11, 12, 0, 0, 20, 21, 22, 0, 0, 30, 31, 32, 0, 0};
    file.edit().write_pixels(0, std::span<const std::uint16_t>(buffer),
                             {.region = region{.start = {4, 2}, .size = {5, 3}, .step = {2, -1}}, .pitch = 5});
    file.edit().commit();
    const std::vector<std::uint16_t> values = file.read().read_pixels<std::uint16_t>(0);
    // The rows of the region from the last: y = 4 holds the first row of the buffer.
    EXPECT_EQ(values[(4 * 37) + 4], 10);
    EXPECT_EQ(values[(4 * 37) + 6], 11);
    EXPECT_EQ(values[(4 * 37) + 8], 12);
    EXPECT_EQ(values[(3 * 37) + 4], 20);
    EXPECT_EQ(values[(2 * 37) + 8], 32);
    // The elements between those of a step are as they were.
    EXPECT_EQ(values[(4 * 37) + 5], static_cast<std::uint16_t>(((4 * 37) + 5) * 7));
}

TEST(editor, updates_the_checksums_of_a_unit_from_the_bytes_changed)
{
    memory_file file(image_file(true));
    std::vector<float> patch(9, 1234.0F);
    file.edit().write_pixels(
        0, std::span<const float>(patch),
        {.region = region{.start = {1, 1}, .size = {3, 3}}, .overflow = openfits::overflow_policy::clamp});
    file.edit().header(0).set("OBSERVER", "someone");
    file.edit().commit();
    const reader edited = file.read();
    const openfits::checksum_report report = edited.verify(0);
    EXPECT_EQ(report.datasum, checksum_state::valid);
    EXPECT_EQ(report.checksum, checksum_state::valid);
    EXPECT_EQ(edited.read_pixels<std::uint16_t>(0)[38], 1234);
}

TEST(editor, carries_a_mismatch_of_the_checksums_through_a_change)
{
    // A unit whose data changed after its checksums were computed stays one that does not match them (Appendix J.4).
    std::vector<std::byte> original = image_file(true);
    original[2880 + 100] ^= std::byte{0x01};
    memory_file file(original);
    file.edit().header(0).set("OBJECT", "elsewhere");
    std::vector<std::uint16_t> patch{9};
    file.edit().write_pixels(0, std::span<const std::uint16_t>(patch),
                             {.region = region{.start = {0, 0}, .size = {1, 1}}});
    file.edit().commit();
    const reader edited(std::make_unique<openfits::memory_source>(file.bytes()));
    const openfits::checksum_report report = edited.verify(0);
    EXPECT_EQ(report.datasum, checksum_state::invalid);
    EXPECT_EQ(report.checksum, checksum_state::invalid);
    // The header still matches the data that DATASUM records: the mismatch is that of the data alone.
    EXPECT_TRUE(std::ranges::none_of(edited.hdu(0).diagnostics(), [](const openfits::diagnostic& found) {
        return found.code == errc::checksum_mismatch;
    }));
}

TEST(editor, adds_the_checksums_that_a_header_gains)
{
    memory_file file(image_file());
    file.edit().header(0).set("CHECKSUM", "0000000000000000");
    file.edit().header(0).set("DATASUM", "0");
    file.edit().commit();
    const openfits::checksum_report report = file.read().verify(0);
    EXPECT_EQ(report.datasum, checksum_state::valid);
    EXPECT_EQ(report.checksum, checksum_state::valid);
}

TEST(editor, writes_the_undefined_value_and_raw_bytes)
{
    std::vector<float> values(20, 1.0F);
    writer output;
    (void)output.add_image(image_of({20}), std::span<const float>(values));
    memory_file file(written(output));
    file.edit().write_blank(0, region{.start = {2}, .size = {3}});
    const std::vector<std::byte> raw{std::byte{0x40}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    file.edit().write_raw(0, std::uint64_t{4} * 10, raw);
    file.edit().commit();
    const std::vector<float> read = file.read().read_pixels<float>(0);
    EXPECT_TRUE(std::isnan(read[2]) && std::isnan(read[3]) && std::isnan(read[4]));
    EXPECT_EQ(read[10], 2.0F);
    EXPECT_EQ(read[11], 1.0F);
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument,
                                    [&file] { file.edit().write_raw(0, 79, std::vector<std::byte>(2)); }));
}

TEST(editor, refuses_the_undefined_value_of_integer_data_without_blank)
{
    memory_file file(image_file());
    EXPECT_TRUE(throws<usage_error>(errc::undefined_value, [&file] { file.edit().write_blank(0); }));
}

// A file of a primary HDU, a binary table of 3 rows of a number, a string and a logical, and an image after it.
std::vector<std::byte> table_file(bool image_after, bool checksums = false)
{
    std::vector<std::int32_t> numbers{10, 20, 30};
    std::vector<std::string> names{"one", "two", "three"};
    std::vector<std::uint8_t> flags{1, 0, 1};
    writer output;
    table_info table{.kind = hdu_kind::bintable, .rows = 3};
    column_format integer;
    integer.type = column_type::int32;
    table.columns.push_back({.name = "N", .format = integer, .scaling = {.scale = 2.0, .zero = 1.0}});
    column_format text;
    text.type = column_type::character;
    text.repeat = 8;
    table.columns.push_back({.name = "NAME", .format = text});
    table.columns.push_back({.name = "FLAG"});
    (void)output.add_table(table, std::vector<column_values>{std::span<const std::int32_t>(numbers), std::span(names),
                                                             column_values::logicals(flags)});
    // The writer borrows the values until the save.
    const std::vector<std::uint8_t> pixels(5, 7);
    if (image_after) {
        (void)output.add_image(image_of({5}, "AFTER"), std::span<const std::uint8_t>(pixels));
    }
    return written(output, {.checksums = checksums});
}

TEST(editor, writes_the_fields_of_columns_in_place)
{
    memory_file file(table_file(true, true));
    const std::vector<double> numbers{41.0, 61.0};
    file.edit().write_cells(1, "N", std::span<const double>(numbers), {.first_row = 1});
    const std::vector<std::string> names{"deux"};
    file.edit().write_cells(1, 1, std::span<const std::string>(names), {.first_row = 1});
    const std::vector<std::uint8_t> flags{0};
    file.edit().write_cells(1, 2, column_values::logicals(flags));
    file.edit().commit();
    const reader edited = file.read();
    // The values written were 10, 20 and 30, stored as 5, 10 and 15 under TSCAL1 = 2 and TZERO1 = 1.
    EXPECT_EQ(edited.read_column<double>(1, 0), (std::vector<double>{11.0, 41.0, 61.0}));
    EXPECT_EQ(edited.read_strings(1, 1, {.trim_strings = true}), (std::vector<std::string>{"one", "deux", "three"}));
    EXPECT_EQ(edited.read_column<std::uint8_t>(1, 2), (std::vector<std::uint8_t>{0, 0, 1}));
    EXPECT_EQ(edited.verify(1).datasum, checksum_state::valid);
    EXPECT_EQ(edited.verify(1).checksum, checksum_state::valid);
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&file] {
        const std::vector<double> more{1.0, 2.0};
        file.edit().write_cells(1, 0, std::span<const double>(more), {.first_row = 2});
    }));
    EXPECT_TRUE(throws<usage_error>(errc::not_a_table, [&file] {
        const std::vector<double> more{1.0};
        file.edit().write_cells(0, 0, std::span<const double>(more));
    }));
}

TEST(editor, appends_rows_in_place_to_the_table_that_ends_the_file)
{
    const std::vector<std::byte> original = table_file(false, true);
    memory_file file(original);
    const std::vector<std::int32_t> numbers{40, 50};
    const std::vector<std::string> names{"four", "five"};
    const std::vector<std::uint8_t> flags{0, 1};
    file.edit().append_rows(1,
                            std::vector<column_values>{std::span<const std::int32_t>(numbers), std::span(names),
                                                       column_values::logicals(flags)},
                            {.scaling = openfits::scaling_policy::raw});
    file.edit().commit();
    const reader edited = file.read();
    EXPECT_EQ(edited.table(1).rows, 5U);
    EXPECT_EQ(edited.read_column<std::int32_t>(1, 0, {.scaling = openfits::scaling_policy::raw}),
              (std::vector<std::int32_t>{5, 10, 15, 40, 50}));
    EXPECT_EQ(edited.read_strings(1, 1, {.trim_strings = true})[4], "five");
    EXPECT_EQ(edited.verify(1).datasum, checksum_state::valid);
    EXPECT_EQ(edited.verify(1).checksum, checksum_state::valid);
    // The table grew in place: the bytes before its rows are as they were but for NAXIS2 and the checksums.
    EXPECT_EQ(file.bytes().size(), original.size());
}

TEST(editor, appends_rows_through_a_rewrite_to_a_table_followed_by_a_unit)
{
    memory_file file(table_file(true, true));
    const std::vector<std::int32_t> numbers{40};
    const std::vector<std::string> names{"four"};
    const std::vector<std::uint8_t> flags{1};
    file.edit().append_rows(1,
                            std::vector<column_values>{std::span<const std::int32_t>(numbers), std::span(names),
                                                       column_values::logicals(flags)},
                            {.scaling = openfits::scaling_policy::raw});
    file.edit().commit();
    const reader edited = file.read();
    EXPECT_EQ(edited.table(1).rows, 4U);
    EXPECT_EQ(edited.read_column<std::int32_t>(1, 0, {.scaling = openfits::scaling_policy::raw})[3], 40);
    EXPECT_EQ(edited.read_pixels<std::uint8_t>(2), std::vector<std::uint8_t>(5, 7));
    EXPECT_EQ(edited.verify(1).datasum, checksum_state::valid);
    EXPECT_EQ(edited.verify(2).checksum, checksum_state::valid);
}

TEST(editor, appends_rows_with_arrays_after_the_heap)
{
    std::vector<std::int16_t> elements{1, 2, 3};
    std::vector<std::uint64_t> offsets{0, 2, 3};
    writer output;
    (void)output.add_table(
        {.kind = hdu_kind::bintable, .rows = 2, .columns = {{.name = "A"}}},
        std::vector<column_values>{column_values::arrays(std::span<const std::int16_t>(elements), offsets)});
    memory_file file(written(output, {.checksums = true}));
    std::vector<std::int16_t> more{7, 8, 9, 10};
    std::vector<std::uint64_t> more_offsets{0, 4};
    file.edit().append_rows(
        1, std::vector<column_values>{column_values::arrays(std::span<const std::int16_t>(more), more_offsets)});
    file.edit().commit();
    const reader edited = file.read();
    const openfits::column_arrays<std::int16_t> arrays = edited.read_arrays<std::int16_t>(1, 0);
    ASSERT_EQ(arrays.size(), 3U);
    EXPECT_EQ(std::vector<std::int16_t>(arrays[2].begin(), arrays[2].end()), more);
    EXPECT_EQ(std::vector<std::int16_t>(arrays[0].begin(), arrays[0].end()), (std::vector<std::int16_t>{1, 2}));
    EXPECT_EQ(edited.table(1).columns[0].format.value_or(column_format{}).max_elements, 4U);
    EXPECT_EQ(edited.verify(1).datasum, checksum_state::valid);
}

TEST(editor, appends_inserts_replaces_and_erases_units)
{
    memory_file file(table_file(true));
    std::vector<float> values{1.5F, 2.5F};
    writer appended;
    (void)appended.add_image(image_of({2}, "APPENDED"), std::span<const float>(values));
    EXPECT_EQ(file.edit().append(std::move(appended)), 3U);
    writer inserted;
    (void)inserted.add_header({});
    file.edit().insert_hdu(1, std::move(inserted));
    // The indices are those of the file as it is: the table, before which the header alone goes.
    file.edit().erase_hdu(1);
    file.edit().commit();
    const reader edited = file.read();
    ASSERT_EQ(edited.hdu_count(), 4U);
    EXPECT_EQ(edited.hdu(1).kind(), hdu_kind::image);
    EXPECT_EQ(edited.hdu(2).name(), "AFTER");
    EXPECT_EQ(edited.hdu(3).name(), "APPENDED");
    EXPECT_EQ(edited.read_pixels<float>(3), values);

    std::vector<std::uint8_t> bytes{1, 2, 3};
    writer replacement;
    (void)replacement.add_image(image_of({3}, "REPLACED"), std::span<const std::uint8_t>(bytes));
    file.edit().replace_hdu(2, std::move(replacement));
    file.edit().commit();
    EXPECT_EQ(file.read().hdu(2).name(), "REPLACED");
    EXPECT_EQ(file.read().read_pixels<std::uint8_t>(2), bytes);
}

TEST(editor, the_unit_after_an_erased_primary_becomes_the_primary_hdu)
{
    std::vector<std::int16_t> first(4, 1);
    std::vector<std::int16_t> second(6, 2);
    writer output;
    (void)output.add_image(image_of({4}), std::span<const std::int16_t>(first));
    (void)output.add_image(image_of({6}, "SECOND"), std::span<const std::int16_t>(second));
    memory_file file(written(output));
    file.edit().erase_hdu(0);
    file.edit().commit();
    const reader edited = file.read();
    ASSERT_EQ(edited.hdu_count(), 1U);
    EXPECT_EQ(edited.hdu(0).kind(), hdu_kind::primary);
    EXPECT_EQ(edited.hdu(0).name(), "SECOND");
    EXPECT_EQ(edited.read_pixels<std::int16_t>(0), second);

    // Before a table, a primary HDU without data comes first.
    memory_file tables(table_file(false));
    tables.edit().erase_hdu(0);
    tables.edit().commit();
    const reader rest = tables.read();
    ASSERT_EQ(rest.hdu_count(), 2U);
    EXPECT_EQ(rest.hdu(1).kind(), hdu_kind::bintable);
}

TEST(editor, moves_units_in_place_when_asked)
{
    memory_file file(table_file(true, true), {.relayout = relayout::in_place});
    for (int i = 0; i < 40; ++i) {
        file.edit().header(1).append(openfits::card("KEY" + std::to_string(i), i));
    }
    file.edit().commit();
    const reader edited = file.read();
    EXPECT_EQ(edited.hdu(1).header().value<int>("KEY39"), 39);
    EXPECT_EQ(edited.read_strings(1, 1, {.trim_strings = true})[2], "three");
    EXPECT_EQ(edited.read_pixels<std::uint8_t>(2), std::vector<std::uint8_t>(5, 7));
    EXPECT_EQ(edited.verify(1).checksum, checksum_state::valid);
    EXPECT_EQ(edited.verify(2).checksum, checksum_state::valid);
}

TEST(editor, edits_a_file_by_its_path_through_a_copy)
{
    const openfits::test::temp_directory directory;
    const std::string path = directory.file("edited.fits");
    openfits::test::write_file(openfits::test::path_of(path), table_file(true));
    {
        editor file(path);
        for (int i = 0; i < 40; ++i) {
            file.header(0).append(openfits::card("KEY" + std::to_string(i), i));
        }
        file.commit();
        EXPECT_EQ(file.file().hdu(0).header().value<int>("KEY39"), 39);
        file.header(2).set("OBJECT", "again");
        file.commit();
    }
    const reader edited(path);
    EXPECT_EQ(edited.hdu(2).header().value<std::string>("OBJECT"), "again");
    EXPECT_EQ(edited.read_pixels<std::uint8_t>(2), std::vector<std::uint8_t>(5, 7));
    // The copy replaced the file: nothing else is left in the directory.
    EXPECT_EQ(directory.entries(), 1U);
}

TEST(editor, refuses_a_file_compressed_with_gzip_and_a_section)
{
    writer output;
    std::vector<std::uint8_t> values(4, 1);
    (void)output.add_image(image_of({4}), std::span<const std::uint8_t>(values));
    openfits::memory_sink sink;
    output.save(sink, {.gzip_level = 1});
    EXPECT_TRUE(throws<openfits::unsupported_error>(
        errc::not_editable, [&sink] { const editor file(std::make_unique<openfits::memory_device>(sink.release())); }));
    const openfits::test::temp_directory directory;
    const std::string path = directory.file("image.fits");
    openfits::test::write_file(openfits::test::path_of(path), table_file(true));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_file_specification, [&path] {
        const editor file(openfits::file_specification::parse(path + "[2][1:2]"));
    }));
    const editor named(openfits::file_specification::parse(path + "[AFTER]"));
    EXPECT_EQ(named.selected_hdu(), 2U);
}

TEST(editor, updates_keywords_of_a_file_in_one_call)
{
    const openfits::test::temp_directory directory;
    const std::string path = directory.file("keywords.fits");
    openfits::test::write_file(openfits::test::path_of(path), image_file(true));
    const std::vector<openfits::card> cards{openfits::card("OBJECT", "M 33", "another"),
                                            openfits::card("EXPTIME", 30.0, "[s] exposure"),
                                            openfits::card::commentary("HISTORY", "edited")};
    openfits::update_keywords(path, 0, cards);
    const reader edited(path);
    EXPECT_EQ(edited.hdu(0).header().value<std::string>("OBJECT"), "M 33");
    EXPECT_EQ(edited.hdu(0).header().value<double>("EXPTIME"), 30.0);
    EXPECT_EQ(edited.hdu(0).header().count("HISTORY"), 1U);
    EXPECT_EQ(edited.verify(0).checksum, checksum_state::valid);
}

// A file of every kind of unit with checksums: images, tables with a heap and of characters, an ASCII table, a
// compressed image and a compressed table, and an extension of a type that the library does not know.
struct every_kind
{
    every_kind()
    {
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            pixels[i] = static_cast<std::uint16_t>(i * 977);
        }
        writer output;
        (void)output.add_image(image_of({37, 23}), std::span<const std::uint16_t>(pixels));
        (void)output.add_image(image_of({37, 23}, "SECOND"), std::span<const std::uint16_t>(pixels));
        (void)output.add_table(
            {.kind = hdu_kind::bintable, .rows = 3, .columns = {{.name = "ARRAYS"}}},
            std::vector<column_values>{column_values::arrays(std::span<const std::int16_t>(elements), offsets)});
        (void)output.add_table({.kind = hdu_kind::table, .rows = 3, .columns = {{.name = "NAME"}}},
                               std::vector<column_values>{std::span(names)});
        (void)output.add_image(image_of({37, 23}, "PACKED"), std::span<const std::uint16_t>(pixels),
                               {.compression = openfits::compression_options{.type = openfits::compression::rice}});
        (void)output.add_table({.kind = hdu_kind::bintable, .rows = 3, .columns = {{.name = "NAME"}}},
                               std::vector<column_values>{std::span(names)},
                               {.compression = openfits::compression_options{.type = openfits::compression::gzip_1}});
        bytes = written(output, {.checksums = true});
        openfits::test::fixture foreign;
        foreign.header(openfits::test::extension_records("FOREIGN", 8, {100})).data(100);
        bytes.insert(bytes.end(), foreign.bytes().begin(), foreign.bytes().end());
    }

    std::vector<std::uint16_t> pixels = std::vector<std::uint16_t>(std::size_t{37} * 23);
    std::vector<std::int16_t> elements{1, 2, 3, 4, 5, 6};
    std::vector<std::uint64_t> offsets{0, 3, 3, 6};
    std::vector<std::string> names{"M 31", "NGC 6992", "IC 434"};
    std::vector<std::byte> bytes;
};

TEST(editor, changes_a_keyword_of_every_kind_of_unit_and_keeps_its_data)
{
    const every_kind source;
    for (const relayout strategy : {relayout::safe_copy, relayout::in_place}) {
        memory_file file(source.bytes, {.relayout = strategy});
        const std::size_t count = file.edit().file().hdu_count();
        ASSERT_EQ(count, 7U);
        for (std::size_t i = 0; i < count; ++i) {
            file.edit().header(i).set("EDITED", static_cast<std::int64_t>(i));
        }
        // A header that grows by a block in the middle of the file, which moves the units after it.
        for (int k = 0; k < 40; ++k) {
            file.edit().header(3).append(openfits::card("KEY" + std::to_string(k), k));
        }
        file.edit().commit();
        const std::vector<std::byte> edited = file.bytes();
        // The extension of an unknown type is not one that verify() takes for conforming: read without it.
        const reader result(std::make_unique<openfits::memory_source>(std::vector<std::byte>(edited)));
        ASSERT_EQ(result.hdu_count(), count);
        const reader before(std::make_unique<openfits::memory_source>(std::vector<std::byte>(source.bytes)));
        for (std::size_t i = 0; i < count; ++i) {
            const openfits::header shown = result.hdu(i).kind() == hdu_kind::compressed_image ? result.image(i).header
                                           : result.hdu(i).kind() == hdu_kind::compressed_table
                                               ? result.table(i).header
                                               : result.hdu(i).header();
            EXPECT_EQ(shown.value<std::int64_t>("EDITED"), static_cast<std::int64_t>(i)) << i;
            EXPECT_EQ(result.hdu(i).kind(), before.hdu(i).kind());
            const std::size_t old_size = before.hdu(i).data_size().value_or(0);
            const std::size_t new_size = result.hdu(i).data_size().value_or(0);
            std::vector<std::byte> old_data(old_size);
            std::vector<std::byte> new_data(new_size);
            before.read_raw(i, 0, old_data);
            result.read_raw(i, 0, new_data);
            EXPECT_EQ(old_data, new_data) << i;
            if (i < 6) {
                EXPECT_EQ(result.verify(i).checksum, checksum_state::valid) << i;
                EXPECT_EQ(result.verify(i).datasum, checksum_state::valid) << i;
            }
        }
        EXPECT_EQ(result.hdu(3).header().value<int>("KEY39"), 39);
    }
}

TEST(editor, inserts_a_primary_hdu_before_the_first_unit)
{
    std::vector<std::int16_t> first(4, 1);
    writer output;
    (void)output.add_image(image_of({4}), std::span<const std::int16_t>(first));
    memory_file file(written(output, {.checksums = true}));
    std::vector<float> values{1.0F, 2.0F};
    writer primary;
    (void)primary.add_image(image_of({2}, "NEW"), std::span<const float>(values));
    file.edit().insert_hdu(0, std::move(primary));
    file.edit().commit();
    const reader edited = file.read();
    ASSERT_EQ(edited.hdu_count(), 2U);
    EXPECT_EQ(edited.hdu(0).kind(), hdu_kind::primary);
    EXPECT_EQ(edited.hdu(0).name(), "NEW");
    EXPECT_EQ(edited.read_pixels<float>(0), values);
    // The primary HDU of the file became an IMAGE extension, its checksums computed again for its header.
    EXPECT_EQ(edited.hdu(1).kind(), hdu_kind::image);
    EXPECT_EQ(edited.read_pixels<std::int16_t>(1), first);
    EXPECT_EQ(edited.verify(1).checksum, checksum_state::valid);
    EXPECT_FALSE(edited.hdu(1).header().contains("EXTEND"));
}

TEST(editor, appends_a_copy_of_a_unit_of_another_file)
{
    const every_kind source;
    const reader other(std::make_unique<openfits::memory_source>(std::vector<std::byte>(source.bytes)));
    memory_file file(image_file());
    EXPECT_EQ(file.edit().append(other, 3), 1U);
    EXPECT_EQ(file.edit().append(other, 0), 2U);
    file.edit().commit();
    const reader edited = file.read();
    ASSERT_EQ(edited.hdu_count(), 3U);
    EXPECT_EQ(edited.hdu(1).kind(), hdu_kind::table);
    EXPECT_EQ(edited.read_strings(1, 0, {.trim_strings = true}), source.names);
    // The primary HDU of the other file, an IMAGE extension here.
    EXPECT_EQ(edited.hdu(2).kind(), hdu_kind::image);
    EXPECT_EQ(edited.read_pixels<std::uint16_t>(2), source.pixels);
}

TEST(editor, rebuilds_tables_and_compressed_images_through_either_strategy)
{
    for (const relayout strategy : {relayout::safe_copy, relayout::in_place}) {
        const every_kind source;
        memory_file file(source.bytes, {.relayout = strategy});
        // Rows appended to a table with a heap, in the middle of the file.
        std::vector<std::int16_t> more{7, 8};
        std::vector<std::uint64_t> more_offsets{0, 2};
        file.edit().append_rows(
            2, std::vector<column_values>{column_values::arrays(std::span<const std::int16_t>(more), more_offsets)});
        // Rows appended to an ASCII table, in the middle too.
        std::vector<std::string> names{"M 42"};
        file.edit().append_rows(3, std::vector<column_values>{std::span(names)});
        // Values written into the compressed image.
        std::vector<std::uint16_t> patch{1, 2, 3};
        file.edit().write_pixels(4, std::span<const std::uint16_t>(patch),
                                 {.region = region{.start = {30, 20}, .size = {3, 1}}});
        file.edit().commit();
        const reader edited(std::make_unique<openfits::memory_source>(file.bytes()));
        EXPECT_EQ(edited.read_arrays<std::int16_t>(2, 0).size(), 4U);
        EXPECT_EQ(edited.read_strings(3, 0, {.trim_strings = true})[3], "M 42");
        std::vector<std::uint16_t> expected = source.pixels;
        std::ranges::copy(patch, expected.begin() + (std::ptrdiff_t{20} * 37) + 30);
        EXPECT_EQ(edited.read_pixels<std::uint16_t>(4), expected);
        for (const std::size_t unit : {2U, 3U, 4U, 5U}) {
            EXPECT_EQ(edited.verify(unit).datasum, checksum_state::valid) << unit;
            EXPECT_EQ(edited.verify(unit).checksum, checksum_state::valid) << unit;
        }
        EXPECT_EQ(edited.read_strings(5, 0, {.trim_strings = true}), source.names);
    }
}

TEST(editor, appends_rows_to_an_ascii_table_that_ends_the_file_in_place)
{
    std::vector<std::string> names{"one", "two"};
    std::vector<std::int32_t> numbers{1, 2};
    writer output;
    (void)output.add_table({.kind = hdu_kind::table, .rows = 2, .columns = {{.name = "NAME"}, {.name = "N"}}},
                           std::vector<column_values>{std::span(names), std::span<const std::int32_t>(numbers)});
    const std::vector<std::byte> original = written(output, {.checksums = true});
    memory_file file(original);
    std::vector<std::string> more_names{"six"};
    std::vector<std::int32_t> more_numbers{-6};
    file.edit().append_rows(
        1, std::vector<column_values>{std::span(more_names), std::span<const std::int32_t>(more_numbers)});
    file.edit().commit();
    EXPECT_EQ(file.bytes().size(), original.size());
    const reader edited = file.read();
    EXPECT_EQ(edited.read_column<std::int32_t>(1, "N"), (std::vector<std::int32_t>{1, 2, -6}));
    EXPECT_EQ(edited.verify(1).datasum, checksum_state::valid);
    EXPECT_EQ(edited.verify(1).checksum, checksum_state::valid);
}

TEST(editor, a_rewrite_of_a_table_puts_its_heap_after_its_rows)
{
    // A table whose heap starts after a gap, as THEAP allows (standard §7.3.5), its column named, since astropy refuses
    // a column without a name in the files that the tests record.
    std::vector<std::string> records = openfits::test::extension_records("BINTABLE", 8, {8, 2}, 2868);
    records.push_back(openfits::test::card_record("TFIELDS", "1"));
    records.push_back(openfits::test::card_record("TTYPE1", "'ARRAYS  '"));
    records.push_back(openfits::test::card_record("TFORM1", "'1PB(2) '"));
    records.push_back(openfits::test::card_record("THEAP", "2880"));
    std::vector<std::byte> data(2884, std::byte{0});
    // Row 0: 2 elements at offset 0; row 1: 2 elements at offset 2.
    data[3] = std::byte{2};
    data[11] = std::byte{2};
    data[15] = std::byte{2};
    data[2880] = std::byte{1};
    data[2881] = std::byte{2};
    data[2882] = std::byte{3};
    data[2883] = std::byte{4};
    openfits::test::fixture builder;
    builder.primary().header(records).data(data);
    memory_file file(std::vector<std::byte>(builder.bytes()));
    std::vector<std::uint8_t> more{9};
    std::vector<std::uint64_t> more_offsets{0, 1};
    file.edit().append_rows(
        1, std::vector<column_values>{column_values::arrays(std::span<const std::uint8_t>(more), more_offsets)});
    file.edit().commit();
    const reader edited = file.read();
    EXPECT_FALSE(edited.hdu(1).header().contains("THEAP"));
    EXPECT_EQ(edited.table(1).heap_offset, 3U * 8);
    const openfits::column_arrays<std::uint8_t> arrays = edited.read_arrays<std::uint8_t>(1, 0);
    ASSERT_EQ(arrays.size(), 3U);
    EXPECT_EQ(std::vector<std::uint8_t>(arrays[1].begin(), arrays[1].end()), (std::vector<std::uint8_t>{3, 4}));
    EXPECT_EQ(std::vector<std::uint8_t>(arrays[2].begin(), arrays[2].end()), more);
}

} // namespace
