// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/checksum.h>
#include <openfits/compression.h>
#include <openfits/error.h>
#include <openfits/image.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/types.h>
#include <openfits/writer.h>

#include "support/sequence.h"
#include "support/throws.h"
#include "support/written_file.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using openfits::bitpix;
using openfits::compression;
using openfits::compression_options;
using openfits::errc;
using openfits::hdu_kind;
using openfits::image_info;
using openfits::image_write_options;
using openfits::numeric_type;
using openfits::quantization;
using openfits::reader;
using openfits::region;
using openfits::usage_error;
using openfits::validation_error;
using openfits::write_options;
using openfits::writer;
using openfits::test::splitmix64;
using openfits::test::throws;
using openfits::test::written;

constexpr std::uint64_t width = 37;
constexpr std::uint64_t height = 23;

reader open(const std::vector<std::byte>& file)
{
    return reader(std::make_unique<openfits::memory_source>(std::vector<std::byte>(file)));
}

// The file of an image of values with the scaling of info, compressed with type, PLIO_1 unless given, its ZBITPIX given
// the value field, right-justified in its 20 columns: the tiles then hold values that the stored type of the new
// ZBITPIX may not. Not recorded for other readers, some of which subtract no offset of BZERO from the values of PLIO_1
// lists.
template <typename T>
std::vector<std::byte> file_with_zbitpix(image_info info, const std::vector<T>& values, std::string_view field,
                                         compression type = compression::plio)
{
    writer output;
    output.add_image(std::move(info), std::span(values), {.compression = compression_options{.type = type}});
    openfits::memory_sink sink;
    output.save(sink);
    std::vector<std::byte> file = sink.release();
    // The algorithms of iterators: Clang 15 cannot instantiate the ranges of these spans with libstdc++ 11 and 12.
    // NOLINTBEGIN(modernize-use-ranges)
    const std::string_view keyword = "ZBITPIX = ";
    const std::span<const std::byte> pattern = std::as_bytes(std::span(keyword));
    const auto at = std::search(file.begin(), file.end(), pattern.begin(), pattern.end());
    EXPECT_NE(at, file.end());
    if (at == file.end()) {
        return file;
    }
    const std::string padded = std::string(20 - field.size(), ' ') + std::string(field);
    const std::span<const std::byte> replacement = std::as_bytes(std::span(padded));
    std::copy(replacement.begin(), replacement.end(), at + static_cast<std::ptrdiff_t>(keyword.size()));
    // NOLINTEND(modernize-use-ranges)
    return file;
}

// A PLIO_1 list holds values from 0 to 2^24 - 1: the tile of an image whose values lie beyond its BITPIX is corrupt,
// which the decode of the stored values checks as that of each algorithm does. The file is an image of 16-bit values
// of 1000 with its ZBITPIX made 8.
TEST(compressed_image, refuses_a_plio_tile_whose_values_lie_beyond_its_bitpix)
{
    image_info info;
    info.geometry.axes = {8, 8};
    const reader input = open(file_with_zbitpix(info, std::vector<std::int16_t>(64, 1000), "8"));
    EXPECT_TRUE(throws<openfits::integrity_error>(errc::corrupt_compressed_data,
                                                  [&] { (void)input.read_pixels<std::uint8_t>(1); }));
}

// A tile is checked whole, a value beyond the stored type found wherever it lies: a tile of 1000 values, each of 7 but
// one of 1000 at the first, the last, or on either side of 512 values, with ZBITPIX made 8.
TEST(compressed_image, refuses_a_plio_value_beyond_its_bitpix_anywhere_in_its_tile)
{
    image_info info;
    info.geometry.axes = {1000};
    const reader valid = open(file_with_zbitpix(info, std::vector<std::int16_t>(1000, 7), "8"));
    EXPECT_EQ(valid.read_pixels<std::uint8_t>(1), std::vector<std::uint8_t>(1000, 7));
    for (const std::size_t position :
         {std::size_t{0}, std::size_t{511}, std::size_t{512}, std::size_t{700}, std::size_t{999}}) {
        std::vector<std::int16_t> values(1000, 7);
        values[position] = 1000;
        const reader input = open(file_with_zbitpix(info, values, "8"));
        EXPECT_TRUE(throws<openfits::integrity_error>(errc::corrupt_compressed_data,
                                                      [&] { (void)input.read_pixels<std::uint8_t>(1); }))
            << "the value at " << position;
    }
}

// A quantized image gives ZSCALE and ZZERO, as columns or as keywords (standard §10.2): one without the other leaves
// its tiles without a scaling to restore their values, a compressed image refused, where a default would read garbage
// (found by the differential run: a ZSCALE column renamed, whose ZZERO alone restored a tile as infinities).
TEST(compressed_image, refuses_a_quantized_image_with_zscale_or_zzero_alone)
{
    std::vector<float> values(width * height);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = 100.0F + static_cast<float>(i % 13);
    }
    image_info info;
    info.geometry.axes = {width, height};
    writer output;
    output.add_image(std::move(info), std::span<const float>(values),
                     {.compression = compression_options{.type = compression::rice, .quantize_level = -0.5}});
    openfits::memory_sink sink;
    output.save(sink);
    const std::vector<std::byte> file = sink.release();
    EXPECT_EQ(open(file).read_pixels<float>(1).size(), values.size());
    for (const std::string_view column : {std::string_view("'ZSCALE  '"), std::string_view("'ZZERO   '")}) {
        std::vector<std::byte> renamed(file.begin(), file.end());
        const std::span<const std::byte> pattern = std::as_bytes(std::span(column));
        // The algorithms of iterators: Clang 15 cannot instantiate the ranges of these spans with libstdc++ 11 and 12.
        // NOLINTNEXTLINE(modernize-use-ranges)
        const auto at = std::search(renamed.begin(), renamed.end(), pattern.begin(), pattern.end());
        ASSERT_NE(at, renamed.end()) << column;
        *(at + 2) = std::byte{'Q'};
        const reader input = open(renamed);
        EXPECT_TRUE(throws<openfits::invalid_data_error>(errc::invalid_compression, [&] {
            (void)input.read_pixels<float>(1);
        })) << column;
    }
}

// A lossless HCOMPRESS_1 tile restores the values that it compressed, which lie within the stored type: a value beyond
// it is corrupt, as in a tile of any other algorithm, where a lossy tile is taken within the type. The file is an image
// of 16-bit values of 1000, its ZBITPIX made 8 (found by the differential run, a fuzzed ZBITPIX that the tile ignored).
TEST(compressed_image, refuses_a_lossless_hcompress_tile_whose_values_lie_beyond_its_bitpix)
{
    image_info info;
    info.geometry.axes = {width, height};
    const reader valid =
        open(file_with_zbitpix(info, std::vector<std::int16_t>(width * height, 7), "8", compression::hcompress));
    EXPECT_EQ(valid.read_pixels<std::uint8_t>(1), std::vector<std::uint8_t>(width * height, 7));
    const reader input =
        open(file_with_zbitpix(info, std::vector<std::int16_t>(width * height, 1000), "8", compression::hcompress));
    EXPECT_TRUE(throws<openfits::integrity_error>(errc::corrupt_compressed_data,
                                                  [&] { (void)input.read_pixels<std::uint8_t>(1); }));
}

// The codes of a quantized tile are 32-bit integers: a lossless HCOMPRESS_1 stream that decodes beyond them is corrupt
// too. The stream of a valid tile is given the sum of a much larger tile in its header (bytes 14 to 21), from which the
// decoder restores values beyond 2^31 (the differential run: a fuzzed stream read as codes of 2^31 - 1 everywhere).
TEST(compressed_image, refuses_a_lossless_quantized_hcompress_tile_whose_codes_lie_beyond_32_bits)
{
    std::vector<float> values(width * height);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = 50.0F + static_cast<float>(i % 7);
    }
    image_info info;
    info.geometry.axes = {width, height};
    writer output;
    output.add_image(std::move(info), std::span<const float>(values),
                     {.compression = compression_options{.type = compression::hcompress, .quantize_level = -0.5}});
    openfits::memory_sink sink;
    output.save(sink);
    std::vector<std::byte> file = sink.release();
    EXPECT_EQ(open(file).read_pixels<float>(1).size(), values.size());
    const std::array<std::byte, 2> magic{std::byte{0xDD}, std::byte{0x99}};
    // The algorithms of iterators: Clang 15 cannot instantiate the ranges of these spans with libstdc++ 11 and 12.
    // NOLINTNEXTLINE(modernize-use-ranges)
    const auto stream = std::search(file.begin(), file.end(), magic.begin(), magic.end());
    ASSERT_NE(stream, file.end());
    const std::array<std::byte, 8> sum{std::byte{0}, std::byte{0}, std::byte{0x40}, std::byte{0},
                                       std::byte{0}, std::byte{0}, std::byte{0},    std::byte{0}};
    std::ranges::copy(sum, stream + 14);
    const reader input = open(file);
    EXPECT_TRUE(
        throws<openfits::integrity_error>(errc::corrupt_compressed_data, [&] { (void)input.read_pixels<float>(1); }));
}

// The lists of an image of 16-bit integers whose BZERO is 32768 hold the stored values plus 32768, from 0 to 65535:
// those values read as their extremes, and one of 70000 is beyond. The file is an image of 32-bit integers with that
// BZERO, its ZBITPIX made 16.
TEST(compressed_image, checks_the_plio_values_of_16_bit_integers_with_the_offset_of_bzero)
{
    image_info info;
    info.geometry.axes = {1000};
    info.bitpix = bitpix::int32;
    info.scaling.zero = 32768.0;
    std::vector<std::int32_t> values(1000, 100);
    values[0] = 0;
    values[999] = 65535;
    const reader valid = open(file_with_zbitpix(info, values, "16"));
    const std::vector<std::uint16_t> read = valid.read_pixels<std::uint16_t>(1);
    ASSERT_EQ(read.size(), 1000U);
    EXPECT_EQ(read[0], 0);
    EXPECT_EQ(read[500], 100);
    EXPECT_EQ(read[999], 65535);
    values[700] = 70000;
    const reader beyond = open(file_with_zbitpix(info, values, "16"));
    EXPECT_TRUE(throws<openfits::integrity_error>(errc::corrupt_compressed_data,
                                                  [&] { (void)beyond.read_pixels<std::uint16_t>(1); }));
}

// Bytes whose BZERO is 32768 go into PLIO_1 lists as their physical values, from 32768 to 33023, and come back. Not
// recorded for other readers, some of which subtract no offset from the values of the lists.
TEST(compressed_image, round_trips_plio_bytes_with_the_offset_of_bzero)
{
    image_info info;
    info.geometry.axes = {1000};
    info.bitpix = bitpix::uint8;
    info.scaling.zero = 32768.0;
    std::vector<std::uint16_t> values(1000);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = static_cast<std::uint16_t>(32768 + (i % 256));
    }
    writer output;
    output.add_image(info, std::span(values), {.compression = compression_options{.type = compression::plio}});
    openfits::memory_sink sink;
    output.save(sink);
    const reader input = open(sink.release());
    EXPECT_EQ(input.read_pixels<std::uint16_t>(1), values);
}

image_info image_of(std::vector<std::uint64_t> axes)
{
    image_info info;
    info.geometry.axes = std::move(axes);
    return info;
}

// Values of every kind of tile: runs, noise and ramps, within a range of the type; for PLIO from 0 to 2^24 - 1.
template <typename T> std::vector<T> values_of(std::size_t count, std::uint64_t seed, bool small = false)
{
    splitmix64 sequence(seed);
    std::vector<T> values(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t random = sequence.next();
        const std::uint64_t column = i % width;
        const std::uint64_t row = i / width;
        const double base = static_cast<double>(column) * 3.0 + static_cast<double>(row) * 5.0;
        double value = (i / 50) % 3 == 0 ? base : base + static_cast<double>(random % 40);
        if constexpr (std::is_floating_point_v<T>) {
            values[i] = static_cast<T>(value / 7.0);
        } else {
            if (!small && std::is_signed_v<T>) {
                value -= 60.0;
            }
            values[i] = static_cast<T>(value);
        }
    }
    return values;
}

// One case of the matrix: an algorithm, a type of the values, and a tile shape.
struct round_trip_case
{
    std::string name{};
    compression type = compression::rice;
    numeric_type values = numeric_type::int16;
    std::vector<std::uint64_t> tile{};
    std::optional<double> level{};
    quantization method = quantization::subtractive_dither_1;
};

// NOLINTNEXTLINE(readability-identifier-naming)
void PrintTo(const round_trip_case& value, std::ostream* output)
{
    *output << value.name;
}

std::string name_of(compression type)
{
    switch (type) {
    case compression::rice:
        return "rice";
    case compression::gzip_1:
        return "gzip1";
    case compression::gzip_2:
        return "gzip2";
    case compression::plio:
        return "plio";
    case compression::hcompress:
        return "hcompress";
    case compression::nocompress:
        return "nocompress";
    case compression::automatic:
        return "automatic";
    case compression::none:
        break;
    }
    return "none";
}

std::string name_of(numeric_type type)
{
    constexpr std::array<std::string_view, 10> names{"i8",  "u8",  "i16", "u16", "i32",
                                                     "u32", "i64", "u64", "f32", "f64"};
    return std::string(names[static_cast<std::size_t>(type)]);
}

std::vector<round_trip_case> round_trip_cases()
{
    std::vector<round_trip_case> cases;
    const std::vector<std::vector<std::uint64_t>> shapes{{}, {16, 16}, {64, 64}, {1, 1}, {7, 5}};
    const std::vector<std::string> shape_names{"rows", "squares", "beyond", "pixels", "uneven"};
    const auto add = [&cases](compression type, numeric_type values, std::vector<std::uint64_t> tile,
                              const std::string& shape, std::optional<double> level = std::nullopt,
                              quantization method = quantization::subtractive_dither_1) {
        round_trip_case entry;
        entry.name = name_of(type) + "_" + name_of(values) + "_" + shape + (level ? "_quantized" : "");
        entry.type = type;
        entry.values = values;
        entry.tile = std::move(tile);
        entry.level = level;
        entry.method = method;
        cases.push_back(std::move(entry));
    };
    for (std::size_t s = 0; s < shapes.size(); ++s) {
        for (const numeric_type values : {numeric_type::uint8, numeric_type::int16, numeric_type::uint16,
                                          numeric_type::int32, numeric_type::uint32}) {
            add(compression::rice, values, shapes[s], shape_names[s]);
            add(compression::gzip_1, values, shapes[s], shape_names[s]);
            add(compression::gzip_2, values, shapes[s], shape_names[s]);
            if (values != numeric_type::uint32) {
                // PLIO_1 takes the stored values, which the offset of 2^31 makes negative.
                add(compression::plio, values, shapes[s], shape_names[s]);
            }
            add(compression::nocompress, values, shapes[s], shape_names[s]);
        }
        for (const numeric_type values : {numeric_type::int64, numeric_type::float32, numeric_type::float64}) {
            add(compression::gzip_1, values, shapes[s], shape_names[s]);
            add(compression::gzip_2, values, shapes[s], shape_names[s]);
            add(compression::nocompress, values, shapes[s], shape_names[s]);
        }
        for (const numeric_type values : {numeric_type::float32, numeric_type::float64}) {
            add(compression::rice, values, shapes[s], shape_names[s], -0.01);
            add(compression::gzip_2, values, shapes[s], shape_names[s], -0.01);
        }
        add(compression::rice, numeric_type::float32, shapes[s], shape_names[s] + "_no_dither", -0.01,
            quantization::no_dither);
        add(compression::rice, numeric_type::float32, shapes[s], shape_names[s] + "_dither2", -0.01,
            quantization::subtractive_dither_2);
    }
    // Two-dimensional tiles of 4 pixels or more alone for H-Compress.
    for (const std::vector<std::uint64_t>& tile : std::vector<std::vector<std::uint64_t>>{{}, {16, 16}, {37, 23}}) {
        const std::string shape = tile.empty() ? "default" : std::to_string(tile[0]) + "x" + std::to_string(tile[1]);
        for (const numeric_type values : {numeric_type::uint8, numeric_type::int16, numeric_type::int32}) {
            add(compression::hcompress, values, tile, shape);
        }
        add(compression::hcompress, numeric_type::float32, tile, shape, -0.01);
    }
    return cases;
}

class compressed_round_trip : public testing::TestWithParam<round_trip_case>
{};

template <typename T> void round_trip(const round_trip_case& parameter)
{
    const bool small = parameter.type == compression::plio;
    const std::vector<T> values = values_of<T>(width * height, 7, small);
    writer output;
    compression_options options;
    options.type = parameter.type;
    options.tile = parameter.tile;
    options.quantize_level = parameter.level;
    options.quantize_method = parameter.method;
    const std::size_t index =
        output.add_image(image_of({width, height}), std::span<const T>(values), {.compression = options});
    // A compressed image is an extension, after an empty primary HDU.
    EXPECT_EQ(index, 1U);
    const std::vector<std::byte> file = written(output);
    const reader input = open(file);
    ASSERT_EQ(input.hdu_count(), 2U);
    EXPECT_EQ(input.hdu(1).kind(), hdu_kind::compressed_image);
    EXPECT_EQ(input.first_image(), 1U);
    EXPECT_EQ(input.image(1).geometry.axes, (std::vector<std::uint64_t>{width, height}));
    const std::vector<T> back = input.read_pixels<T>(1);
    ASSERT_EQ(back.size(), values.size());
    if (parameter.level) {
        const double spacing = std::abs(*parameter.level);
        for (std::size_t i = 0; i < values.size(); ++i) {
            ASSERT_NEAR(static_cast<double>(back[i]), static_cast<double>(values[i]), spacing) << i;
        }
    } else {
        EXPECT_EQ(back, values);
    }
    // A region reads the values of the whole that it holds.
    const region box{.start = {3, 2}, .size = {30, 19}, .step = {3, -2}};
    const std::vector<T> part = input.read_pixels<T>(1, {.region = box});
    std::size_t k = 0;
    for (std::uint64_t y = 0; y < 10; ++y) {
        for (std::uint64_t x = 0; x < 10; ++x) {
            ASSERT_EQ(part[k], back[((20 - (2 * y)) * width) + 3 + (3 * x)]) << k;
            ++k;
        }
    }
}

TEST_P(compressed_round_trip, reads_back_what_it_wrote)
{
    const round_trip_case& parameter = GetParam();
    switch (parameter.values) {
    case numeric_type::uint8:
        round_trip<std::uint8_t>(parameter);
        break;
    case numeric_type::int16:
        round_trip<std::int16_t>(parameter);
        break;
    case numeric_type::uint16:
        round_trip<std::uint16_t>(parameter);
        break;
    case numeric_type::int32:
        round_trip<std::int32_t>(parameter);
        break;
    case numeric_type::uint32:
        round_trip<std::uint32_t>(parameter);
        break;
    case numeric_type::int64:
        round_trip<std::int64_t>(parameter);
        break;
    case numeric_type::float32:
        round_trip<float>(parameter);
        break;
    case numeric_type::float64:
        round_trip<double>(parameter);
        break;
    case numeric_type::int8:
    case numeric_type::uint64:
        break;
    }
}

INSTANTIATE_TEST_SUITE_P(matrix, compressed_round_trip, testing::ValuesIn(round_trip_cases()),
                         [](const testing::TestParamInfo<round_trip_case>& instance) { return instance.param.name; });

TEST(compressed_image, a_compressed_primary_is_unit_1_after_an_empty_primary)
{
    const std::vector<std::uint16_t> values = values_of<std::uint16_t>(width * height, 1);
    image_info info = image_of({width, height});
    info.header.set("OBJECT", "M 31");
    writer output;
    EXPECT_EQ(output.add_image(info, std::span<const std::uint16_t>(values),
                               {.compression = compression_options{.type = compression::rice}}),
              1U);
    EXPECT_EQ(output.hdu_count(), 2U);
    const reader input = open(written(output));
    EXPECT_EQ(input.hdu(0).kind(), hdu_kind::primary);
    EXPECT_EQ(input.hdu(0).header().value<std::int64_t>("NAXIS"), 0);
    const openfits::header& table = input.hdu(1).header();
    EXPECT_TRUE(table.value<bool>("ZSIMPLE"));
    EXPECT_EQ(table.value<std::string>("EXTNAME"), "COMPRESSED_IMAGE");
    EXPECT_EQ(table.value<std::string>("ZCMPTYPE"), "RICE_1");
    EXPECT_EQ(input.hdu(1).name(), "COMPRESSED_IMAGE");
    // The image that it holds, with the header of a primary HDU as the decompression restores it.
    const image_info image = input.image(1);
    EXPECT_EQ(image.bitpix, bitpix::int16);
    EXPECT_EQ(image.value_type(), numeric_type::uint16);
    EXPECT_EQ(image.header[0].keyword(), "SIMPLE");
    EXPECT_EQ(image.header.value<std::int64_t>("NAXIS1"), 37);
    EXPECT_EQ(image.header.value<double>("BZERO"), 32768.0);
    EXPECT_EQ(image.header.value<std::string>("OBJECT"), "M 31");
    EXPECT_FALSE(image.header.index_of("EXTNAME").has_value());
    EXPECT_FALSE(image.header.index_of("ZCMPTYPE").has_value());
    EXPECT_FALSE(image.header.index_of("TFORM1").has_value());
    EXPECT_EQ(input.read_pixels<std::uint16_t>(1), values);
}

TEST(compressed_image, the_options_of_the_file_compress_the_units_that_give_none)
{
    const std::vector<std::int16_t> values = values_of<std::int16_t>(width * height, 2);
    writer output;
    EXPECT_EQ(output.add_image(image_of({width, height}), std::span<const std::int16_t>(values)), 0U);
    EXPECT_EQ(output.add_image(image_of({width, height}), std::span<const std::int16_t>(values)), 1U);
    EXPECT_EQ(output.add_image(image_of({width, height}), std::span<const std::int16_t>(values),
                               {.compression = compression_options{.type = compression::none}}),
              2U);
    const reader input = open(written(output, {.compression = compression_options{}}));
    // The first image moves to unit 1, after the empty primary HDU that the standard requires.
    ASSERT_EQ(input.hdu_count(), 4U);
    EXPECT_EQ(input.hdu(1).kind(), hdu_kind::compressed_image);
    EXPECT_EQ(input.hdu(2).kind(), hdu_kind::compressed_image);
    EXPECT_EQ(input.hdu(3).kind(), hdu_kind::image);
    EXPECT_EQ(input.hdu(1).header().value<std::string>("ZCMPTYPE"), "RICE_1");
    EXPECT_FALSE(input.hdu(2).header().index_of("ZSIMPLE").has_value());
    EXPECT_EQ(input.hdu(2).header().value<std::string>("ZTENSION"), "IMAGE");
    for (std::size_t unit = 1; unit < 4; ++unit) {
        EXPECT_EQ(input.read_pixels<std::int16_t>(unit), values) << unit;
    }
    // Without the options of the file, the same writer writes no compressed unit.
    EXPECT_EQ(open(written(output)).hdu_count(), 3U);
}

TEST(compressed_image, floating_point_values_stay_lossless_by_default)
{
    std::vector<float> values = values_of<float>(width * height, 3);
    values[5] = std::bit_cast<float>(0x7FC01234U);
    values[6] = -0.0F;
    values[7] = std::numeric_limits<float>::infinity();
    writer output;
    output.add_image(image_of({width, height}), std::span<const float>(values), {.compression = compression_options{}});
    const reader input = open(written(output));
    const openfits::header& table = input.hdu(1).header();
    // GZIP_2, without ZQUANTIZ, ZSCALE and ZZERO: the form of the standard of a lossless image.
    EXPECT_EQ(table.value<std::string>("ZCMPTYPE"), "GZIP_2");
    EXPECT_FALSE(table.index_of("ZQUANTIZ").has_value());
    EXPECT_EQ(input.table(1).columns.size(), 1U);
    const std::vector<float> back = input.read_pixels<float>(1);
    for (std::size_t i = 0; i < values.size(); ++i) {
        ASSERT_EQ(std::bit_cast<std::uint32_t>(back[i]), std::bit_cast<std::uint32_t>(values[i])) << i;
    }
}

TEST(compressed_image, a_tile_without_noise_falls_back_to_gzip)
{
    // Constant rows have no noise, so that a level relative to the noise cannot quantize them.
    std::vector<float> values(width * height);
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::uint64_t row = i / width;
        values[i] = static_cast<float>(row) * 0.5F;
    }
    writer output;
    output.add_image(image_of({width, height}), std::span<const float>(values),
                     {.compression = compression_options{.quantize_level = 4.0}});
    const reader input = open(written(output));
    const openfits::table_info table = input.table(1);
    EXPECT_TRUE(table.find_column("GZIP_COMPRESSED_DATA").has_value());
    EXPECT_EQ(input.read_pixels<float>(1), values);
    // Without the fallback, the image cannot be written.
    writer strict;
    strict.add_image(image_of({width, height}), std::span<const float>(values),
                     {.compression = compression_options{.quantize_level = 4.0, .lossless_fallback = false}});
    EXPECT_TRUE(throws<validation_error>(errc::invalid_compression, [&] { (void)written(strict); }));
}

TEST(compressed_image, the_seed_of_the_dithering_comes_from_the_data)
{
    const std::vector<float> values = values_of<float>(width * height, 4);
    const auto save = [&values](std::optional<std::uint32_t> seed) {
        writer output;
        output.add_image(image_of({width, height}), std::span<const float>(values),
                         {.compression = compression_options{.quantize_level = -0.01, .dither_seed = seed}});
        return written(output);
    };
    // The same values give the same file, the seed included.
    const std::vector<std::byte> first = save(std::nullopt);
    EXPECT_EQ(first, save(std::nullopt));
    const auto seed = open(first).hdu(1).header().value<std::int64_t>("ZDITHER0");
    EXPECT_GE(seed, 1);
    EXPECT_LE(seed, 10'000);
    EXPECT_EQ(open(save(1234)).hdu(1).header().value<std::int64_t>("ZDITHER0"), 1234);
}

TEST(compressed_image, nulls_of_quantized_tiles_are_nan)
{
    std::vector<float> values = values_of<float>(width * height, 5);
    values[100] = std::numeric_limits<float>::quiet_NaN();
    values[200] = 0.0F;
    writer output;
    output.add_image(image_of({width, height}), std::span<const float>(values),
                     {.compression = compression_options{.quantize_level = -0.01,
                                                         .quantize_method = quantization::subtractive_dither_2}});
    const reader input = open(written(output));
    EXPECT_EQ(input.hdu(1).header().value<std::int64_t>("ZBLANK"), -2147483647);
    const std::vector<float> back = input.read_pixels<float>(1);
    EXPECT_TRUE(std::isnan(back[100]));
    EXPECT_EQ(back[200], 0.0F);
    EXPECT_NEAR(back[300], values[300], 0.01);
}

TEST(compressed_image, lossy_hcompress_keeps_the_range_of_the_type_and_undefined_pixels)
{
    // A smooth image of unsigned 16-bit values with peaks at the top of the type, which a lossy scale restores a
    // little beyond it: read within the type.
    constexpr std::uint64_t columns = 61;
    constexpr std::uint64_t rows = 43;
    std::vector<std::uint16_t> peaks(columns * rows);
    for (std::size_t i = 0; i < peaks.size(); ++i) {
        const std::uint64_t column = i % columns;
        const std::uint64_t row = i / columns;
        peaks[i] = i % 211 == 0 ? 65535 : static_cast<std::uint16_t>(30000 + (column * 10) + (row * 7));
    }
    writer smooth;
    smooth.add_image(image_of({columns, rows}), std::span<const std::uint16_t>(peaks),
                     {.compression = compression_options{
                          .type = compression::hcompress, .tile = {columns, 16}, .hcompress_scale = -50.0}});
    const std::vector<double> restored = open(written(smooth)).read_pixels<double>(1);
    EXPECT_LE(*std::ranges::max_element(restored), 65535.0);
    EXPECT_GE(*std::ranges::min_element(restored), 0.0);

    // BLANK and NaN, which a lossy scale would blur into values: their tiles are compressed losslessly.
    std::vector<std::int16_t> integers = values_of<std::int16_t>(width * height, 13);
    integers[50] = -32768;
    image_info blank = image_of({width, height});
    blank.blank = -32768;
    std::vector<float> reals = values_of<float>(width * height, 14);
    reals[100] = std::numeric_limits<float>::quiet_NaN();
    writer output;
    output.add_image(blank, std::span<const std::int16_t>(integers),
                     {.compression = compression_options{.type = compression::hcompress, .hcompress_scale = -20.0}});
    output.add_image(image_of({width, height}), std::span<const float>(reals),
                     {.compression = compression_options{
                          .type = compression::hcompress, .quantize_level = 4.0, .hcompress_scale = 2.0}});
    const reader input = open(written(output));
    EXPECT_TRUE(std::isnan(input.read_pixels<double>(1)[50]));
    EXPECT_TRUE(std::isnan(input.read_pixels<float>(2)[100]));
}

TEST(compressed_image, rice_compresses_64_bit_integers_on_request)
{
    const std::vector<std::int64_t> values = values_of<std::int64_t>(width * height, 6);
    writer refused;
    EXPECT_TRUE(throws<validation_error>(errc::invalid_compression, [&] {
        refused.add_image(image_of({width, height}), std::span<const std::int64_t>(values),
                          {.compression = compression_options{.type = compression::rice}});
    }));
    writer output;
    output.add_image(image_of({width, height}), std::span<const std::int64_t>(values),
                     {.compression = compression_options{.type = compression::rice, .rice_bytepix_8 = true}});
    const reader input = open(written(output));
    EXPECT_EQ(input.hdu(1).header().value<std::int64_t>("ZVAL2"), 8);
    EXPECT_EQ(input.read_pixels<std::int64_t>(1), values);
    // By default a 64-bit image takes GZIP_2.
    writer automatic;
    automatic.add_image(image_of({width, height}), std::span<const std::int64_t>(values),
                        {.compression = compression_options{}});
    EXPECT_EQ(open(written(automatic)).hdu(1).header().value<std::string>("ZCMPTYPE"), "GZIP_2");
}

TEST(compressed_image, refuses_what_its_algorithm_cannot_compress)
{
    const std::vector<float> floats = values_of<float>(width * height, 8);
    const std::vector<std::int32_t> integers = values_of<std::int32_t>(width * height, 9);
    const auto refused = [](auto values, const compression_options& options,
                            std::vector<std::uint64_t> axes = {37, 23}) {
        writer output;
        output.add_image(image_of(std::move(axes)), std::span(values), {.compression = options});
        (void)written(output);
    };
    // Floating-point values kept as they are with an algorithm of integers.
    for (const compression type : {compression::rice, compression::plio, compression::hcompress}) {
        EXPECT_TRUE(throws<validation_error>(errc::invalid_compression,
                                             [&] { refused(floats, compression_options{.type = type}); }));
    }
    // SUBTRACTIVE_DITHER_2 with H-Compress, and tiles that are not two-dimensional or too small.
    EXPECT_TRUE(throws<validation_error>(errc::invalid_compression, [&] {
        refused(floats, compression_options{.type = compression::hcompress,
                                            .quantize_level = 4.0,
                                            .quantize_method = quantization::subtractive_dither_2});
    }));
    EXPECT_TRUE(throws<validation_error>(errc::invalid_compression, [&] {
        refused(integers, compression_options{.type = compression::hcompress, .tile = {37, 1}});
    }));
    EXPECT_TRUE(throws<validation_error>(errc::invalid_compression, [&] {
        refused(integers, compression_options{.type = compression::hcompress, .tile = {3, 23}});
    }));
    // Values of PLIO beyond 0 to 2^24 - 1, and 64-bit values of H-Compress whose coefficients leave 2^60.
    EXPECT_TRUE(throws<validation_error>(errc::invalid_compression,
                                         [&] { refused(integers, compression_options{.type = compression::plio}); }));
    std::vector<std::int64_t> large(static_cast<std::size_t>(37) * 23, 5);
    large[100] = std::int64_t{1} << 60;
    EXPECT_TRUE(throws<validation_error>(errc::invalid_compression,
                                         [&] { refused(large, compression_options{.type = compression::hcompress}); }));
    large[100] = -(std::int64_t{1} << 40);
    EXPECT_NO_THROW(refused(large, compression_options{.type = compression::hcompress}));
    // Options outside their values.
    EXPECT_TRUE(
        throws<usage_error>(errc::invalid_argument, [&] { refused(integers, compression_options{.tile = {37}}); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument,
                                    [&] { refused(integers, compression_options{.rice_block_size = 20}); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument,
                                    [&] { refused(floats, compression_options{.quantize_level = 0.0}); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] {
        refused(floats, compression_options{.quantize_level = 4.0, .dither_seed = 10'001});
    }));
}

TEST(compressed_image, checksums_cover_the_table)
{
    const std::vector<std::int16_t> values = values_of<std::int16_t>(width * height, 10);
    writer output;
    output.add_image(image_of({width, height}), std::span<const std::int16_t>(values),
                     {.compression = compression_options{}});
    for (const openfits::test::sink_kind kind :
         {openfits::test::sink_kind::rewritable, openfits::test::sink_kind::append_only}) {
        const reader input = open(written(output, {.checksums = true}, kind));
        for (std::size_t unit = 0; unit < 2; ++unit) {
            const openfits::checksum_report report = input.verify(unit);
            EXPECT_EQ(report.datasum, openfits::checksum_state::valid) << unit;
            EXPECT_EQ(report.checksum, openfits::checksum_state::valid) << unit;
        }
        // A read of the whole image verifies the DATASUM of its table.
        EXPECT_EQ(input.read_pixels<std::int16_t>(1), values);
    }
}

TEST(compressed_image, a_tile_cache_decodes_each_tile_once)
{
    const std::vector<std::int32_t> values = values_of<std::int32_t>(width * height, 11);
    writer output;
    output.add_image(image_of({width, height}), std::span<const std::int32_t>(values),
                     {.compression = compression_options{.tile = {16, 8}}});
    const reader input = open(written(output));
    openfits::tile_cache cache(std::uint64_t{1} << 20);
    std::vector<std::int32_t> row(width);
    for (std::uint64_t y = 0; y < height; ++y) {
        input.read_pixels(1, std::span<std::int32_t>(row),
                          {.region = region{.start = {0, y}, .size = {width, 1}}, .cache = &cache});
        ASSERT_EQ(row, std::vector<std::int32_t>(values.begin() + static_cast<std::ptrdiff_t>(y * width),
                                                 values.begin() + static_cast<std::ptrdiff_t>((y + 1) * width)))
            << y;
    }
    // 3 x 3 tiles, each decoded once, and found by the other rows of its band.
    EXPECT_EQ(cache.misses(), 9U);
    EXPECT_EQ(cache.hits(), (height * 3) - 9);
    EXPECT_GT(cache.size(), 0U);
}

TEST(compressed_image, a_tile_cache_too_small_for_a_tile_keeps_none)
{
    const std::vector<std::int32_t> values = values_of<std::int32_t>(width * height, 11);
    writer output;
    output.add_image(image_of({width, height}), std::span<const std::int32_t>(values),
                     {.compression = compression_options{.tile = {16, 8}}});
    const reader input = open(written(output));
    // Tiles of 512 bytes, which the read uses although the cache keeps none of them.
    openfits::tile_cache cache(100);
    EXPECT_EQ(input.read_pixels<std::int32_t>(1, {.cache = &cache}), values);
    EXPECT_EQ(cache.misses(), 9U);
    EXPECT_EQ(cache.size(), 0U);
}

TEST(compressed_image, a_read_and_a_save_report_their_progress_and_can_be_cancelled)
{
    const std::vector<std::int16_t> values = values_of<std::int16_t>(width * height, 12);
    writer output;
    output.add_image(image_of({width, height}), std::span<const std::int16_t>(values),
                     {.compression = compression_options{}});
    std::uint64_t last = 0;
    std::uint64_t total = 0;
    const std::vector<std::byte> file = written(output, {.progress = [&](std::uint64_t done, std::uint64_t all) {
                                                    last = done;
                                                    total = all;
                                                    return true;
                                                }});
    EXPECT_EQ(last, total);
    EXPECT_EQ(total, (width * height * 2) + file.size());
    EXPECT_TRUE(throws<openfits::cancelled_error>(errc::cancelled, [&] {
        (void)written(output, {.progress = [](std::uint64_t, std::uint64_t) { return false; }});
    }));
    const reader input = open(file);
    std::uint64_t read = 0;
    (void)input.read_pixels<std::int16_t>(1, {.progress = [&](std::uint64_t done, std::uint64_t all) {
                                              read = done;
                                              EXPECT_EQ(all, width * height * 2);
                                              return true;
                                          }});
    EXPECT_EQ(read, width * height * 2);
    EXPECT_TRUE(throws<openfits::cancelled_error>(errc::cancelled, [&] {
        (void)input.read_pixels<std::int16_t>(1, {.progress = [](std::uint64_t, std::uint64_t) { return false; }});
    }));
}

} // namespace
