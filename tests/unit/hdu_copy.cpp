// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/checksum.h>
#include <openfits/compression.h>
#include <openfits/error.h>
#include <openfits/header.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/table.h>
#include <openfits/writer.h>

#include "support/fixture_builder.h"
#include "support/temp_directory.h"
#include "support/throws.h"
#include "support/written_file.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Units copied by the writer from a reader (writer::add_hdu()): as they are, byte for byte, or converted to their
// place, or with their headers edited.

namespace {

using openfits::column_values;
using openfits::compression;
using openfits::compression_options;
using openfits::errc;
using openfits::hdu_kind;
using openfits::header;
using openfits::image_info;
using openfits::reader;
using openfits::table_info;
using openfits::usage_error;
using openfits::validation_error;
using openfits::writer;
using openfits::test::card_record;
using openfits::test::fixture;
using openfits::test::throws;
using openfits::test::written;

reader open(std::vector<std::byte> file)
{
    return reader(std::make_unique<openfits::memory_source>(std::move(file)));
}

// The file that output writes, not recorded for other readers: an extension of an unknown type, or bytes outside
// printable ASCII, which they refuse.
std::vector<std::byte> saved(const writer& output, const openfits::write_options& options = {})
{
    openfits::memory_sink sink;
    output.save(sink, options);
    return sink.release();
}

image_info image_of(std::vector<std::uint64_t> axes, std::string_view name = {})
{
    image_info info;
    info.geometry.axes = std::move(axes);
    if (!name.empty()) {
        info.header.set("EXTNAME", name);
    }
    info.header.set("OBJECT", "the source");
    return info;
}

// A file of every kind of unit: images, tables with a heap and of characters, a compressed image and a compressed
// table, and an extension of a type that the library does not know, after them.
struct source_file
{
    explicit source_file(bool checksums)
    {
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            pixels[i] = static_cast<std::uint16_t>(i * 977);
            floats[i] = static_cast<float>(i) / 7.0F;
        }
        writer output;
        (void)output.add_image(image_of({37, 23}), std::span<const std::uint16_t>(pixels));
        (void)output.add_image(image_of({37, 23}, "FLOATS"), std::span<const float>(floats));
        (void)output.add_table(
            {.kind = hdu_kind::bintable, .rows = 3, .columns = {{.name = "ARRAYS"}}},
            std::vector<column_values>{column_values::arrays(std::span<const std::int16_t>(elements), offsets)});
        (void)output.add_table({.kind = hdu_kind::table, .rows = 3, .columns = {{.name = "NAME"}}},
                               std::vector<column_values>{std::span(names)});
        (void)output.add_image(image_of({37, 23}, "PACKED"), std::span<const std::uint16_t>(pixels),
                               {.compression = compression_options{.type = compression::rice}});
        (void)output.add_table({.kind = hdu_kind::bintable, .rows = 3, .columns = {{.name = "NAME"}}},
                               std::vector<column_values>{std::span(names)},
                               {.compression = compression_options{.type = compression::gzip_1}});
        bytes = written(output, {.checksums = checksums});
        // An extension of a type of Appendix F, walked by its size.
        fixture foreign;
        foreign
            .header(openfits::test::with(openfits::test::extension_records("FOREIGN", 8, {100}),
                                         {card_record("EXTNAME", "'ALIEN'")}))
            .data(100);
        bytes.insert(bytes.end(), foreign.bytes().begin(), foreign.bytes().end());
    }

    std::vector<std::uint16_t> pixels = std::vector<std::uint16_t>(std::size_t{37} * 23);
    std::vector<float> floats = std::vector<float>(std::size_t{37} * 23);
    std::vector<std::int16_t> elements{1, 2, 3, 4, 5, 6};
    std::vector<std::uint64_t> offsets{0, 3, 3, 6};
    std::vector<std::string> names{"M 31", "NGC 6992", "IC 434"};
    std::vector<std::byte> bytes;
};

// Each unit copied in its place is the bytes of the source, its header and its data records, and its checksums stay
// valid.
TEST(hdu_copy, copies_are_byte_identical_to_their_source_units)
{
    for (const bool checksums : {false, true}) {
        const source_file source(checksums);
        const reader input = open(source.bytes);
        ASSERT_EQ(input.hdu_count(), 7U);
        EXPECT_EQ(input.hdu(6).kind(), hdu_kind::unknown);
        writer output;
        for (std::size_t i = 0; i < input.hdu_count(); ++i) {
            EXPECT_EQ(output.add_hdu(input, i), i);
        }
        const std::vector<std::byte> copy = saved(output);
        EXPECT_EQ(copy, source.bytes);
        std::vector<std::byte> appended;
        openfits::callback_sink append_only([&appended](std::span<const std::byte> bytes) {
            appended.insert(appended.end(), bytes.begin(), bytes.end());
        });
        output.save(append_only);
        EXPECT_EQ(appended, source.bytes);
        EXPECT_EQ(output.size(), source.bytes.size());
        if (checksums) {
            const reader copied = open(copy);
            for (std::size_t i = 0; i < 6; ++i) {
                EXPECT_EQ(copied.verify(i).checksum, openfits::checksum_state::valid) << i;
            }
        }
    }
}

// A file appended to under its own path: the save copies the units from the reader of the file as it writes the new
// one, which replaces the file once it is complete, and the reader goes on reading the file it opened.
TEST(hdu_copy, appends_to_the_file_that_it_copies_from)
{
    const source_file source(false);
    const openfits::test::temp_directory directory;
    const std::string path = directory.file("units.fits");
    openfits::file_sink original(path);
    original.write(source.bytes);
    original.finish();

    const reader input(path);
    writer output;
    for (std::size_t i = 0; i < input.hdu_count(); ++i) {
        (void)output.add_hdu(input, i);
    }
    const std::vector<std::uint8_t> values{1, 2, 3};
    EXPECT_EQ(output.add_image(image_of({3}, "ADDED"), std::span<const std::uint8_t>(values)), 7U);
    output.save(path);

    EXPECT_EQ(input.read_pixels<std::uint16_t>(0), source.pixels);
    const openfits::file_source appended(path);
    std::vector<std::byte> head(source.bytes.size());
    ASSERT_GT(appended.size(), head.size());
    appended.read(0, head);
    EXPECT_EQ(head, source.bytes);
    const reader check(path);
    ASSERT_EQ(check.hdu_count(), 8U);
    EXPECT_EQ(check.read_pixels<std::uint8_t>(7), values);
    EXPECT_EQ(directory.entries(), 1U);
}

// A primary HDU copied after the first unit becomes an IMAGE extension, and an IMAGE extension copied first the
// primary HDU, their data as they are; the other extensions come after a primary HDU without data.
TEST(hdu_copy, places_a_unit_where_its_kind_can_be)
{
    for (const bool checksums : {false, true}) {
        const source_file source(checksums);
        const reader input = open(source.bytes);
        writer output;
        EXPECT_EQ(output.add_hdu(input, 1), 0U);
        EXPECT_EQ(output.add_hdu(input, 0), 1U);
        EXPECT_EQ(output.add_hdu(input, 2), 2U);
        const reader copy(std::make_unique<openfits::memory_source>(written(output)), {.strict = true});
        const header& primary = copy.hdu(0).header();
        EXPECT_EQ(primary[0].keyword(), "SIMPLE");
        EXPECT_TRUE(primary.value<bool>("EXTEND"));
        EXPECT_FALSE(primary.contains("PCOUNT"));
        EXPECT_FALSE(primary.contains("XTENSION"));
        EXPECT_EQ(primary.value<std::string>("EXTNAME"), "FLOATS");
        EXPECT_EQ(copy.read_pixels<float>(0), source.floats);
        const header& extension = copy.hdu(1).header();
        EXPECT_EQ(copy.hdu(1).kind(), hdu_kind::image);
        EXPECT_EQ(extension.value<std::string>("XTENSION"), "IMAGE");
        EXPECT_EQ(extension.value<std::int64_t>("PCOUNT"), 0);
        EXPECT_EQ(extension.value<std::int64_t>("GCOUNT"), 1);
        EXPECT_FALSE(extension.contains("EXTEND"));
        EXPECT_FALSE(extension.contains("SIMPLE"));
        EXPECT_EQ(extension.value<std::string>("OBJECT"), "the source");
        EXPECT_EQ(copy.read_pixels<std::uint16_t>(1), source.pixels);
        EXPECT_EQ(copy.read_arrays<std::int16_t>(2, 0).values, source.elements);
        for (std::size_t i = 0; i < 3; ++i) {
            EXPECT_TRUE(copy.hdu(i).diagnostics().empty()) << i;
            // Converted units have their checksums computed again; the copy as it is keeps its own.
            const openfits::checksum_report sums = copy.verify(i);
            EXPECT_EQ(sums.checksum, checksums ? openfits::checksum_state::valid : openfits::checksum_state::absent);
        }
    }
    // A table first follows a primary HDU without data.
    const source_file source(false);
    const reader input = open(source.bytes);
    writer tables;
    EXPECT_EQ(tables.add_hdu(input, 3), 1U);
    EXPECT_EQ(tables.add_hdu(input, 6), 2U);
    const reader copy = open(saved(tables));
    EXPECT_EQ(copy.hdu(0).data_size(), 0U);
    EXPECT_EQ(copy.read_strings(1, 0, {.trim_strings = true}), source.names);
    EXPECT_EQ(copy.hdu(2).kind(), hdu_kind::unknown);
    EXPECT_EQ(copy.hdu(2).name(), "ALIEN");
}

// The header of a copy can be edited on the way: its structural cards stay those of the unit, and its checksums are
// computed again.
TEST(hdu_copy, takes_an_edited_header)
{
    const source_file source(true);
    const reader input = open(source.bytes);
    header cards = openfits::without_structural(input.hdu(4).header());
    cards.set("OBJECT", "edited on the way");
    cards.add_history("copied");
    writer output;
    (void)output.add_hdu(input, 0);
    EXPECT_EQ(output.add_hdu(input, 4, {.header = cards}), 1U);
    const std::vector<std::byte> file = written(output);
    const reader copy(std::make_unique<openfits::memory_source>(std::vector<std::byte>(file)), {.strict = true});
    EXPECT_EQ(copy.hdu(1).kind(), hdu_kind::compressed_image);
    EXPECT_EQ(copy.image(1).header.value<std::string>("OBJECT"), "edited on the way");
    EXPECT_EQ(copy.hdu(1).header().count("HISTORY"), 1U);
    EXPECT_EQ(copy.read_pixels<std::uint16_t>(1), source.pixels);
    EXPECT_EQ(copy.verify(1).checksum, openfits::checksum_state::valid);
    EXPECT_EQ(copy.verify(1).datasum, openfits::checksum_state::valid);
    // The data are those of the unit, byte for byte.
    std::vector<std::byte> copied(copy.hdu(1).data_size().value_or(0));
    std::vector<std::byte> original(copied.size());
    copy.read_raw(1, 0, copied);
    input.read_raw(4, 0, original);
    EXPECT_EQ(copied, original);

    // A structural keyword in the header is refused by the save, as in any header.
    cards.set("ZCMPTYPE", "GZIP_1");
    writer refusing;
    (void)refusing.add_hdu(input, 4, {.header = cards});
    EXPECT_TRUE(throws<validation_error>(errc::structural_keyword, [&] { (void)written(refusing); }));
}

TEST(hdu_copy, gives_copies_the_checksums_that_the_save_asks_for)
{
    const source_file source(false);
    const reader input = open(source.bytes);
    writer output;
    for (std::size_t i = 0; i < 6; ++i) {
        (void)output.add_hdu(input, i);
    }
    for (const openfits::test::sink_kind kind :
         {openfits::test::sink_kind::rewritable, openfits::test::sink_kind::append_only}) {
        const reader copy = open(written(output, {.checksums = true}, kind));
        for (std::size_t i = 0; i < copy.hdu_count(); ++i) {
            const openfits::checksum_report sums = copy.verify(i);
            EXPECT_EQ(sums.checksum, openfits::checksum_state::valid) << i;
            EXPECT_EQ(sums.datasum, openfits::checksum_state::valid) << i;
            // The cards of the unit in their order, the two of the checksums after them.
            std::vector<std::string> keywords;
            for (const openfits::card& value : copy.hdu(i).header()) {
                keywords.emplace_back(value.keyword());
            }
            ASSERT_GE(keywords.size(), 2U);
            EXPECT_EQ(keywords[keywords.size() - 2], "CHECKSUM");
            EXPECT_EQ(keywords.back(), "DATASUM");
            keywords.resize(keywords.size() - 2);
            std::vector<std::string> original;
            for (const openfits::card& value : input.hdu(i).header()) {
                original.emplace_back(value.keyword());
            }
            EXPECT_EQ(keywords, original) << i;
        }
    }
}

// A unit converted to its place keeps the order of its other cards, as CFITSIO converts the units that it copies.
TEST(hdu_copy, a_converted_unit_keeps_the_order_of_its_cards)
{
    fixture image;
    image
        .header(openfits::test::with(openfits::test::primary_records(16, {2, 2}),
                                     {card_record("BZERO", "32768"), card_record("EXTEND", "T"),
                                      card_record("OBJECT", "'M 31'"), card_record("BSCALE", "1")}))
        .data(8);
    const reader input = open(image.bytes());
    writer output;
    (void)output.add_header({});
    (void)output.add_hdu(input, 0);
    const reader copy = open(written(output));
    std::vector<std::string> keywords;
    for (const openfits::card& value : copy.hdu(1).header()) {
        keywords.emplace_back(value.keyword());
    }
    EXPECT_EQ(keywords, (std::vector<std::string>{"XTENSION", "BITPIX", "NAXIS", "NAXIS1", "NAXIS2", "PCOUNT", "GCOUNT",
                                                  "BZERO", "OBJECT", "BSCALE"}));
    EXPECT_EQ(copy.read_pixels<std::uint16_t>(1), input.read_pixels<std::uint16_t>(0));
    EXPECT_TRUE(copy.hdu(1).diagnostics().empty());
}

// Standard §6: random groups are the primary HDU of a file, which only the first unit can be.
TEST(hdu_copy, copies_random_groups_as_the_primary_alone)
{
    fixture groups;
    groups
        .header(openfits::test::with(openfits::test::primary_records(16, {0, 2, 2}),
                                     {card_record("GROUPS", "T"), card_record("PCOUNT", "1"),
                                      card_record("GCOUNT", "2"), card_record("PTYPE1", "'UU'")}))
        .data(20);
    const reader input = open(groups.bytes());
    ASSERT_EQ(input.hdu(0).kind(), hdu_kind::random_groups);
    writer output;
    (void)output.add_hdu(input, 0);
    EXPECT_EQ(written(output), groups.bytes());
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { (void)output.add_hdu(input, 0); }));
    EXPECT_EQ(output.hdu_count(), 1U);
}

// A short file: the fill of its last block taken as far as it goes, zeros after; data that it lacks fail the save.
TEST(hdu_copy, copies_what_a_short_file_holds)
{
    fixture image;
    image.header(openfits::test::primary_records(8, {100})).data(100);
    const std::vector<std::byte> full = image.bytes();
    image.truncate(2880 + 200);
    const reader input = open(image.bytes());
    writer output;
    (void)output.add_hdu(input, 0);
    EXPECT_EQ(written(output), full);

    fixture cut;
    cut.header(openfits::test::primary_records(8, {100})).data(100).truncate(2880 + 50);
    const reader cut_input = open(cut.bytes());
    writer failing;
    (void)failing.add_hdu(cut_input, 0);
    EXPECT_TRUE(throws<openfits::io_error>(errc::end_of_data, [&] { (void)written(failing); }));
}

// Standard §4.1.2.3: a header of bytes outside printable ASCII is refused, copied as it is, or written with question
// marks, as the save asks.
TEST(hdu_copy, writes_bytes_outside_printable_ascii_as_the_save_asks)
{
    fixture latin;
    latin
        .header(openfits::test::with(openfits::test::primary_records(8, {10}), {card_record("TEMP", "-10.5",
                                                                                            "[\xB0"
                                                                                            "C] of the sensor")}))
        .data(10);
    const reader input = open(latin.bytes());
    writer output;
    (void)output.add_hdu(input, 0);
    EXPECT_TRUE(throws<validation_error>(errc::invalid_character, [&] { (void)saved(output); }));
    EXPECT_EQ(saved(output, {.non_ascii = openfits::non_ascii::keep}), latin.bytes());
    const reader replaced = open(written(output, {.non_ascii = openfits::non_ascii::replace}));
    EXPECT_EQ(replaced.hdu(0).header().comment("TEMP"), "[?C] of the sensor");
    EXPECT_TRUE(replaced.hdu(0).diagnostics().empty());
}

TEST(hdu_copy, refuses_units_that_it_cannot_copy)
{
    const source_file source(false);
    const reader input = open(source.bytes);
    writer output;
    EXPECT_TRUE(throws<usage_error>(errc::hdu_not_found, [&] { (void)output.add_hdu(input, 7); }));
    // A header that gives no size for its data.
    fixture broken;
    broken.header(openfits::test::primary_records(7, {10}));
    const reader broken_input = open(broken.bytes());
    EXPECT_TRUE(
        throws<openfits::invalid_data_error>(errc::invalid_structure, [&] { (void)output.add_hdu(broken_input, 0); }));
    EXPECT_EQ(output.hdu_count(), 0U);
}

} // namespace
