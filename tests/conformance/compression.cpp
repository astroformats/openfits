// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Tiled compression (standard §10): compressed images and tables built from records and bytes, the rules of their
// keywords and columns, and hostile tiles, which every decoder refuses before it reads or writes beyond its tile.

#include <openfits/error.h>
#include <openfits/io.h>
#include <openfits/limits.h>
#include <openfits/reader.h>

#include "compress/plio.h"
#include "support/compressed_fixtures.h"
#include "support/fixture_builder.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using openfits::errc;
using openfits::integrity_error;
using openfits::invalid_data_error;
using openfits::limit_error;
using openfits::reader;
using openfits::unsupported_error;
using openfits::test::big_endian;
using openfits::test::card_record;
using openfits::test::field_of;
using openfits::test::fixture;
using openfits::test::image_keywords;
using openfits::test::p_descriptor;
using openfits::test::rice16;
using openfits::test::rice32;
using openfits::test::row_of;
using openfits::test::throws;
using openfits::test::with;

reader open(const fixture& file, openfits::limits limits = {})
{
    return reader(std::make_unique<openfits::memory_source>(std::vector<std::byte>(file.bytes())), {.limits = limits});
}

// A compressed image of 4 x 2 values in tiles of a row, its tiles in the heap in order: keywords and the columns after
// COMPRESSED_DATA, with the field of each tile in them.
struct image_fixture
{
    int bitpix = 16;
    std::vector<std::string> keywords{};
    std::vector<std::vector<std::byte>> tiles{};
    std::vector<std::pair<std::string, std::string>> columns{};
    std::vector<std::vector<std::byte>> fields{};
    std::string compressed_form = "1PB";
    // Added to the offset of the first tile, to point elsewhere.
    std::uint32_t displacement = 0;
    // Bytes of the heap after the tiles, which the other columns point to.
    std::vector<std::byte> extra_heap{};

    [[nodiscard]] fixture build() const
    {
        std::vector<std::pair<std::string, std::string>> all{{"COMPRESSED_DATA", compressed_form}};
        all.insert(all.end(), columns.begin(), columns.end());
        std::vector<std::vector<std::byte>> rows;
        std::vector<std::byte> heap;
        const std::size_t element = compressed_form == "1PI" ? 2 : 1;
        for (std::size_t t = 0; t < tiles.size(); ++t) {
            std::vector<std::byte> row =
                p_descriptor(static_cast<std::uint32_t>(tiles[t].size() / element),
                             static_cast<std::uint32_t>(heap.size()) + (t == 0 ? displacement : 0));
            if (t < fields.size()) {
                row.insert(row.end(), fields[t].begin(), fields[t].end());
            }
            rows.push_back(std::move(row));
            heap.insert(heap.end(), tiles[t].begin(), tiles[t].end());
        }
        heap.insert(heap.end(), extra_heap.begin(), extra_heap.end());
        fixture file;
        file.primary();
        openfits::test::binary_table_unit(file, all, rows, heap, keywords);
        return file;
    }
};

std::vector<std::string> rice_keywords(int bitpix, int bytepix, const std::vector<std::uint64_t>& tile = {4, 1})
{
    return with(image_keywords(bitpix, {4, 2}, tile, "RICE_1"),
                {card_record("ZNAME1", "'BLOCKSIZE'"), card_record("ZVAL1", "32"), card_record("ZNAME2", "'BYTEPIX'"),
                 card_record("ZVAL2", std::to_string(bytepix))});
}

// The values of short_image().
std::vector<std::int16_t> shorts()
{
    return {1, 2, 3, 4, -5, 6, -7, 8};
}

image_fixture short_image()
{
    image_fixture image;
    image.keywords = rice_keywords(16, 2);
    image.tiles = {rice16({1, 2, 3, 4}), rice16({-5, 6, -7, 8})};
    return image;
}

TEST(conformance_compression, a_compressed_image_reads_as_the_image_it_holds)
{
    // Standard §10.1: the table presented as an image, and as the table it is.
    const reader input = open(short_image().build());
    EXPECT_EQ(input.hdu(1).kind(), openfits::hdu_kind::compressed_image);
    EXPECT_EQ(input.image(1).geometry.axes, (std::vector<std::uint64_t>{4, 2}));
    EXPECT_EQ(input.read_pixels<std::int16_t>(1), shorts());
    EXPECT_EQ(input.table(1).rows, 2U);
    EXPECT_EQ(input.image(1).header[0].keyword(), "XTENSION");
    EXPECT_EQ(input.image(1).header.value<std::string>("XTENSION"), "IMAGE");
}

TEST(conformance_compression, ztile_defaults_to_rows_and_a_longer_tile_holds_its_axis)
{
    // Standard §10.1.2: without ZTILEn, a tile is a row; a ZTILEn beyond its axis covers the axis.
    image_fixture rows = short_image();
    rows.keywords =
        with(image_keywords(16, {4, 2}, {}, "RICE_1"), {card_record("ZVAL1", "32"), card_record("ZVAL2", "2")});
    EXPECT_EQ(open(rows.build()).read_pixels<std::int16_t>(1), shorts());
    image_fixture whole;
    whole.keywords = rice_keywords(16, 2, {100, 100});
    whole.tiles = {rice16(shorts())};
    EXPECT_EQ(open(whole.build()).read_pixels<std::int16_t>(1), shorts());
}

TEST(conformance_compression, refuses_keywords_that_describe_no_compressed_image)
{
    const auto refused = [](std::vector<std::string> keywords, std::size_t tiles = 2) {
        image_fixture image = short_image();
        image.keywords = std::move(keywords);
        image.tiles.resize(tiles, rice16({1, 2, 3, 4}));
        const reader input = open(image.build());
        return throws<invalid_data_error>(errc::invalid_compression, [&] { (void)input.image(1); });
    };
    EXPECT_TRUE(refused(rice_keywords(16, 2, {0, 1})));
    EXPECT_TRUE(refused(rice_keywords(12, 2)));
    EXPECT_TRUE(refused(rice_keywords(16, 3)));
    EXPECT_TRUE(refused(
        with(image_keywords(16, {4, 2}, {4, 1}, "RICE_1"), {card_record("ZVAL1", "20"), card_record("ZVAL2", "2")})));
    // NAXIS2 is the number of tiles.
    EXPECT_TRUE(refused(rice_keywords(16, 2), 3));
    std::vector<std::string> without_bitpix = rice_keywords(16, 2);
    without_bitpix.erase(without_bitpix.begin() + 1);
    EXPECT_TRUE(refused(without_bitpix));
    // An algorithm of no name of Table 36.
    image_fixture unknown = short_image();
    unknown.keywords = image_keywords(16, {4, 2}, {4, 1}, "BZIP2_1");
    const reader input = open(unknown.build());
    EXPECT_TRUE(throws<unsupported_error>(errc::unsupported_compression, [&] { (void)input.image(1); }));
}

TEST(conformance_compression, the_zblank_column_wins_over_the_keyword)
{
    // Standard §10.1.3: ZBLANK as a keyword serves every tile, and a ZBLANK column, when present, is used instead.
    image_fixture image;
    image.bitpix = -32;
    image.keywords = with(rice_keywords(-32, 4), {card_record("ZQUANTIZ", "'NO_DITHER'"), card_record("ZBLANK", "7")});
    image.tiles = {rice32({0, 3, 7, 1}), rice32({3, 7, 2, 0})};
    image.columns = {{"ZSCALE", "1D"}, {"ZZERO", "1D"}};
    image.fields = {row_of({field_of(1.0), field_of(0.0)}), row_of({field_of(1.0), field_of(0.0)})};
    const std::vector<float> keyword = open(image.build()).read_pixels<float>(1);
    EXPECT_TRUE(std::isnan(keyword[2]));
    EXPECT_TRUE(std::isnan(keyword[5]));
    EXPECT_EQ(keyword[1], 3.0F);
    image.columns.emplace_back("ZBLANK", "1J");
    image.fields = {row_of({field_of(1.0), field_of(0.0), field_of(std::int32_t{3})}),
                    row_of({field_of(1.0), field_of(0.0), field_of(std::int32_t{3})})};
    const std::vector<float> column = open(image.build()).read_pixels<float>(1);
    EXPECT_TRUE(std::isnan(column[1]));
    EXPECT_TRUE(std::isnan(column[4]));
    EXPECT_EQ(column[2], 7.0F);
}

TEST(conformance_compression, the_zeros_of_subtractive_dither_2_follow_zblank)
{
    // Standard §10.2.1 reserves -2147483647 for exact zeros and ZBLANK for NaN, recommending -2147483648 for ZBLANK
    // (§10.1.3); CFITSIO and its files use -2147483647 for NaN and -2147483646 for zeros. The reader follows ZBLANK.
    const auto read = [](std::int64_t zblank) {
        image_fixture image;
        image.bitpix = -32;
        image.keywords =
            with(rice_keywords(-32, 4), {card_record("ZQUANTIZ", "'SUBTRACTIVE_DITHER_2'"),
                                         card_record("ZDITHER0", "1"), card_record("ZBLANK", std::to_string(zblank))});
        image.tiles = {rice32({-2147483647, -2147483646, 5, 6}),
                       rice32({std::numeric_limits<std::int32_t>::min(), 1, 2, 3})};
        image.columns = {{"ZSCALE", "1D"}, {"ZZERO", "1D"}};
        image.fields = {row_of({field_of(0.5), field_of(100.0)}), row_of({field_of(0.5), field_of(100.0)})};
        return open(image.build()).read_pixels<float>(1);
    };
    const std::vector<float> cfitsio = read(-2147483647);
    EXPECT_TRUE(std::isnan(cfitsio[0]));
    EXPECT_EQ(cfitsio[1], 0.0F);
    EXPECT_FALSE(std::isnan(cfitsio[4]));
    const std::vector<float> standard = read(std::numeric_limits<std::int32_t>::min());
    EXPECT_EQ(standard[0], 0.0F);
    EXPECT_NE(standard[1], 0.0F);
    EXPECT_TRUE(std::isnan(standard[4]));
    EXPECT_NEAR(standard[2], 102.5F, 0.5F);
}

TEST(conformance_compression, a_null_pixel_mask_marks_undefined_pixels)
{
    // Standard §10.2.2: the mask of a tile, compressed with ZMASKCMP, sets its pixels to BLANK.
    image_fixture image = short_image();
    image.keywords = with(rice_keywords(16, 2), {card_record("BLANK", "-99"), card_record("ZMASKCMP", "'GZIP_1'")});
    const std::vector<std::byte> mask_one = big_endian(std::vector<std::uint8_t>{0, 1, 0, 0});
    const std::vector<std::byte> mask_two = big_endian(std::vector<std::uint8_t>{0, 0, 0, 1});
    const std::vector<std::byte> first = openfits::test::gzip(mask_one);
    const std::vector<std::byte> second = openfits::test::gzip(mask_two);
    // The masks after the two tiles in the heap.
    const auto heap = static_cast<std::uint32_t>(image.tiles[0].size() + image.tiles[1].size());
    image.columns = {{"NULL_PIXEL_MASK", "1PB"}};
    image.fields = {
        p_descriptor(static_cast<std::uint32_t>(first.size()), heap),
        p_descriptor(static_cast<std::uint32_t>(second.size()), heap + static_cast<std::uint32_t>(first.size()))};
    image.tiles[1].insert(image.tiles[1].end(), first.begin(), first.end());
    image.tiles[1].insert(image.tiles[1].end(), second.begin(), second.end());
    // The second tile holds the masks after its stream, which its decoder leaves alone.
    EXPECT_EQ(open(image.build()).read_pixels<std::int16_t>(1, {.blank = openfits::blank_policy::ignore}),
              (std::vector<std::int16_t>{1, -99, 3, 4, -5, 6, -7, -99}));
}

TEST(conformance_compression, a_gzip_tile_takes_the_width_of_its_values_from_its_size)
{
    // CFITSIO reads the values of a GZIP tile by the size they inflate to: 16-bit values in a 32-bit image.
    image_fixture image;
    image.bitpix = 32;
    image.keywords = image_keywords(32, {4, 2}, {4, 1}, "GZIP_2");
    const std::vector<std::byte> values = big_endian(std::vector<std::int16_t>{1, -2, 3, -4});
    std::vector<std::byte> shuffled(values.size());
    for (std::size_t i = 0; i < 4; ++i) {
        shuffled[i] = values[i * 2];
        shuffled[4 + i] = values[(i * 2) + 1];
    }
    image.tiles = {openfits::test::gzip(shuffled), openfits::test::gzip(shuffled)};
    EXPECT_EQ(open(image.build()).read_pixels<std::int32_t>(1),
              (std::vector<std::int32_t>{1, -2, 3, -4, 1, -2, 3, -4}));
}

TEST(conformance_compression, nocompress_tiles_are_the_values_as_they_are)
{
    // Table 36, §10.3.5: NOCOMPRESS, in COMPRESSED_DATA or in UNCOMPRESSED_DATA as fpack writes it.
    image_fixture raw;
    raw.keywords = image_keywords(16, {4, 2}, {4, 1}, "NOCOMPRESS");
    raw.tiles = {big_endian(std::vector<std::int16_t>{1, 2, 3, 4}),
                 big_endian(std::vector<std::int16_t>{-5, 6, -7, 8})};
    EXPECT_EQ(open(raw.build()).read_pixels<std::int16_t>(1), shorts());
    image_fixture column;
    column.keywords = image_keywords(16, {4, 2}, {4, 1}, "NOCOMPRESS");
    column.tiles = {{}, {}};
    column.columns = {{"UNCOMPRESSED_DATA", "1PI"}};
    const std::vector<std::byte> one = big_endian(std::vector<std::int16_t>{1, 2, 3, 4});
    const std::vector<std::byte> two = big_endian(std::vector<std::int16_t>{-5, 6, -7, 8});
    column.fields = {p_descriptor(4, 0), p_descriptor(4, 8)};
    column.extra_heap = row_of({one, two});
    EXPECT_EQ(open(column.build()).read_pixels<std::int16_t>(1), shorts());
}

TEST(conformance_compression, plio_values_stand_beyond_the_offset_of_unsigned_images)
{
    // PLIO_1 (standard §10.4.3) stores the unsigned values of a 16-bit image under BZERO 32768, as CFITSIO does.
    image_fixture image;
    image.keywords = with(image_keywords(16, {4, 2}, {4, 1}, "PLIO_1"), {card_record("BZERO", "32768")});
    image.compressed_form = "1PI";
    const auto list = [](std::vector<std::int32_t> values) {
        std::vector<std::int16_t> words;
        openfits::detail::plio_encode(values, words);
        std::vector<std::uint16_t> bits(words.size());
        for (std::size_t i = 0; i < words.size(); ++i) {
            bits[i] = static_cast<std::uint16_t>(words[i]);
        }
        return big_endian(bits);
    };
    image.tiles = {list({0, 1, 65535, 32768}), list({7, 7, 0, 0})};
    EXPECT_EQ(open(image.build()).read_pixels<std::uint16_t>(1),
              (std::vector<std::uint16_t>{0, 1, 65535, 32768, 7, 7, 0, 0}));
}

TEST(conformance_compression, hostile_tiles_are_refused)
{
    const auto corrupt = [](const image_fixture& image) {
        const reader input = open(image.build());
        return throws<integrity_error>(errc::corrupt_compressed_data,
                                       [&] { (void)input.read_pixels<std::int16_t>(1); });
    };
    // A stream that ends early, and one too short for its tile whatever it holds.
    image_fixture truncated = short_image();
    truncated.tiles[1].pop_back();
    EXPECT_TRUE(corrupt(truncated));
    image_fixture tiny = short_image();
    tiny.tiles[0] = {std::byte{0}};
    EXPECT_TRUE(corrupt(tiny));
    // A GZIP stream of more values than its tile, and of fewer.
    image_fixture longer;
    longer.keywords = image_keywords(16, {4, 2}, {4, 1}, "GZIP_1");
    const std::vector<std::byte> five = big_endian(std::vector<std::int16_t>{1, 2, 3, 4, 5});
    const std::vector<std::byte> three = big_endian(std::vector<std::int16_t>{1, 2, 3});
    longer.tiles = {openfits::test::gzip(five), openfits::test::gzip(three)};
    EXPECT_TRUE(corrupt(longer));
    // A tile in no column.
    image_fixture empty = short_image();
    empty.tiles[1].clear();
    EXPECT_TRUE(corrupt(empty));
    // A descriptor beyond the heap.
    image_fixture beyond = short_image();
    beyond.displacement = 1000;
    const reader input = open(beyond.build());
    EXPECT_TRUE(
        throws<invalid_data_error>(errc::invalid_descriptor, [&] { (void)input.read_pixels<std::int16_t>(1); }));
}

TEST(conformance_compression, tiles_beyond_the_limits_are_refused)
{
    const fixture file = short_image().build();
    EXPECT_TRUE(throws<limit_error>(errc::tile_too_large,
                                    [&] { (void)open(file, {.max_tile_bytes = 7}).read_pixels<std::int16_t>(1); }));
    EXPECT_TRUE(throws<limit_error>(errc::too_many_tiles,
                                    [&] { (void)open(file, {.max_tiles = 1}).read_pixels<std::int16_t>(1); }));
    EXPECT_EQ(open(file, {.max_tile_bytes = 8, .max_tiles = 2}).read_pixels<std::int16_t>(1), shorts());
}

TEST(conformance_compression, a_compressed_table_without_zctyp_takes_gzip_2)
{
    // Standard §10.3: a table of one column of 16-bit integers, its tile shuffled and gzipped.
    const std::vector<std::byte> values = big_endian(std::vector<std::int16_t>{10, -20, 30});
    std::vector<std::byte> shuffled(values.size());
    for (std::size_t i = 0; i < 3; ++i) {
        shuffled[i] = values[i * 2];
        shuffled[3 + i] = values[(i * 2) + 1];
    }
    const std::vector<std::byte> cell = openfits::test::gzip(shuffled);
    fixture file;
    file.primary();
    openfits::test::binary_table_unit(
        file, {{"N", "1QB"}}, {row_of({field_of(std::uint64_t{cell.size()}), field_of(std::uint64_t{0})})}, cell,
        {card_record("ZTABLE", "T"), card_record("ZTILELEN", "3"), card_record("ZNAXIS1", "2"),
         card_record("ZNAXIS2", "3"), card_record("ZPCOUNT", "0"), card_record("ZFORM1", "'1I'")});
    const reader input = open(file);
    EXPECT_EQ(input.table(1).rows, 3U);
    EXPECT_EQ(input.read_column<std::int16_t>(1, 0), (std::vector<std::int16_t>{10, -20, 30}));
}

} // namespace
