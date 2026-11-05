// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/checksum.h>
#include <openfits/compression.h>
#include <openfits/editor.h>
#include <openfits/error.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/table.h>
#include <openfits/verify.h>
#include <openfits/writer.h>

#include "checksum/checksum.h"
#include "compress/image_tiles.h"
#include "edit/header_edit.h"
#include "support/files.h"
#include "support/temp_directory.h"
#include "support/throws.h"
#include "support/written_file.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <vector>

// The values that the editor writes in place: the images of every BITPIX with their scaling, the fields of every type
// of column, and compressed images, whose tiles that the writes meet are encoded again (standard §10.1, §10.2).

namespace {

using openfits::bitpix;
using openfits::checksum_state;
using openfits::column_format;
using openfits::column_type;
using openfits::column_values;
using openfits::compression;
using openfits::compression_options;
using openfits::editor;
using openfits::errc;
using openfits::hdu_kind;
using openfits::image_info;
using openfits::reader;
using openfits::region;
using openfits::table_info;
using openfits::usage_error;
using openfits::writer;
using openfits::test::throws;
using openfits::test::written;

// An editor of a file in memory, and the file as each commit leaves it, read back with a clean report.
class memory_file
{
public:
    explicit memory_file(std::vector<std::byte> bytes, const openfits::editor_options& options = {})
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
    memory_file(std::unique_ptr<openfits::memory_device> device, const openfits::editor_options& options)
        : device_(device.get()), editor_(std::move(device), options)
    {}

    openfits::memory_device* device_ = nullptr;
    editor editor_;
};

// An image of every BITPIX, with a scaling or none: physical values written into a region read back as written.
struct scaled_case
{
    bitpix stored = bitpix::int16;
    openfits::scaling scaling{};
    std::optional<std::int64_t> blank{};
};

// The name is fixed by GoogleTest.
// NOLINTNEXTLINE(readability-identifier-naming)
void PrintTo(const scaled_case& value, std::ostream* out)
{
    *out << "bitpix" << (static_cast<int>(value.stored) < 0 ? "m" : "") << std::abs(static_cast<int>(value.stored))
         << (value.scaling.scale != 1.0 || value.scaling.zero != 0.0 ? "_scaled" : "");
}

class pixel_writes : public testing::TestWithParam<scaled_case>
{};

TEST_P(pixel_writes, write_physical_values_into_every_bitpix_in_place)
{
    const scaled_case& parameters = GetParam();
    image_info info;
    info.bitpix = parameters.stored;
    info.geometry.axes = {9, 7};
    info.scaling = parameters.scaling;
    info.blank = parameters.blank;
    std::vector<double> values(63);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = parameters.scaling.zero + (parameters.scaling.scale * static_cast<double>(i % 50));
    }
    writer output;
    (void)output.add_image(info, std::span<const double>(values));
    memory_file file(written(output, {.checksums = true}));
    std::vector<double> patch(6);
    for (std::size_t i = 0; i < patch.size(); ++i) {
        patch[i] = parameters.scaling.zero + (parameters.scaling.scale * static_cast<double>(90 + i));
    }
    file.edit().write_pixels(0, std::span<const double>(patch), {.region = region{.start = {2, 3}, .size = {3, 2}}});
    file.edit().commit();
    const reader edited = file.read();
    const std::vector<double> read = edited.read_pixels<double>(0);
    for (std::size_t y = 0; y < 7; ++y) {
        for (std::size_t x = 0; x < 9; ++x) {
            const bool inside = x >= 2 && x < 5 && y >= 3 && y < 5;
            const double expected = inside ? patch[((y - 3) * 3) + (x - 2)] : values[(y * 9) + x];
            ASSERT_NEAR(read[(y * 9) + x], expected, std::abs(expected) * 1e-6) << x << ", " << y;
        }
    }
    EXPECT_EQ(edited.verify(0).datasum, checksum_state::valid);
    EXPECT_EQ(edited.verify(0).checksum, checksum_state::valid);
}

INSTANTIATE_TEST_SUITE_P(
    every_bitpix, pixel_writes,
    testing::Values(scaled_case{.stored = bitpix::uint8}, scaled_case{.stored = bitpix::int16},
                    scaled_case{.stored = bitpix::int32}, scaled_case{.stored = bitpix::int64},
                    scaled_case{.stored = bitpix::float32}, scaled_case{.stored = bitpix::float64},
                    scaled_case{.stored = bitpix::uint8, .scaling = {.scale = 0.5, .zero = -10.0}},
                    scaled_case{.stored = bitpix::int16, .scaling = {.scale = 0.25, .zero = 100.0}, .blank = -32768},
                    scaled_case{.stored = bitpix::int32, .scaling = {.scale = 2.0, .zero = 7.0}},
                    scaled_case{.stored = bitpix::int64, .scaling = {.scale = 1.0, .zero = 1000.0}},
                    scaled_case{.stored = bitpix::float32, .scaling = {.scale = 2.0, .zero = 1.0}}));

TEST(pixel_writes, stored_values_and_an_override_of_the_scaling)
{
    image_info info;
    info.bitpix = bitpix::int16;
    info.geometry.axes = {4};
    info.scaling = {.scale = 2.0, .zero = 0.0};
    std::vector<std::int16_t> stored{1, 2, 3, 4};
    writer output;
    (void)output.add_image(info, std::span<const std::int16_t>(stored), {.scaling = openfits::scaling_policy::raw});
    memory_file file(written(output));
    const std::vector<std::int16_t> raw{10, 20};
    file.edit().write_pixels(0, std::span<const std::int16_t>(raw),
                             {.region = region{.start = {0}, .size = {2}}, .scaling = openfits::scaling_policy::raw});
    const std::vector<double> overridden{100.0};
    file.edit().write_pixels(0, std::span<const double>(overridden),
                             {.region = region{.start = {3}, .size = {1}},
                              .scaling = openfits::scaling_policy::override,
                              .override_scaling = {.scale = 10.0, .zero = 0.0}});
    file.edit().commit();
    EXPECT_EQ(file.read().read_pixels<std::int16_t>(0, {.scaling = openfits::scaling_policy::raw}),
              (std::vector<std::int16_t>{10, 20, 3, 10}));
}

TEST(pixel_writes, refuse_values_that_do_not_fit_before_they_are_kept)
{
    std::vector<std::uint8_t> values(4, 0);
    writer output;
    (void)output.add_image(image_info{.geometry = {.axes = {4}}}, std::span<const std::uint8_t>(values));
    memory_file file(written(output));
    const std::vector<double> large{300.0};
    EXPECT_TRUE(throws<openfits::invalid_data_error>(errc::value_out_of_range, [&] {
        file.edit().write_pixels(0, std::span<const double>(large), {.region = region{.start = {0}, .size = {1}}});
    }));
    EXPECT_FALSE(file.edit().has_pending_changes());
    const std::vector<double> nan{std::nan("")};
    EXPECT_TRUE(throws<openfits::invalid_data_error>(errc::undefined_value, [&] {
        file.edit().write_pixels(0, std::span<const double>(nan), {.region = region{.start = {0}, .size = {1}}});
    }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument,
                                    [&] { file.edit().write_pixels(0, std::span<const double>(nan)); }));
    file.edit().write_pixels(
        0, std::span<const double>(large),
        {.region = region{.start = {1}, .size = {1}}, .overflow = openfits::overflow_policy::clamp});
    file.edit().commit();
    EXPECT_EQ(file.read().read_pixels<std::uint8_t>(0), (std::vector<std::uint8_t>{0, 255, 0, 0}));
}

// A table of a column of every type of a binary table, 3 rows.
std::vector<std::byte> every_column_file()
{
    std::vector<std::uint8_t> logicals{1, 0, 1};
    std::vector<std::uint8_t> bits{1, 0, 1, 1, 0, 0, 1, 0, 1};
    std::vector<std::uint8_t> bytes{1, 2, 3};
    std::vector<std::int16_t> shorts{-1, -2, -3};
    std::vector<std::int32_t> ints{100, 200, 300};
    std::vector<std::int64_t> longs{1, 2, 3};
    std::vector<std::string> strings{"a", "bb", "ccc"};
    std::vector<float> floats{0.5F, 1.5F, 2.5F};
    std::vector<double> doubles{0.25, 1.25, 2.25};
    std::vector<std::complex<float>> complexes{{1, 2}, {3, 4}, {5, 6}};
    std::vector<std::complex<double>> doubles_complex{{1, -1}, {2, -2}, {3, -3}};
    std::vector<std::uint16_t> unsigned_shorts{65535, 1, 2};
    table_info table{.kind = hdu_kind::bintable, .rows = 3};
    for (const char* name : {"L", "X", "B", "I", "J", "K", "A", "E", "D", "C", "M", "U"}) {
        table.columns.push_back({.name = name});
    }
    column_format bit_format;
    bit_format.type = column_type::bit;
    bit_format.repeat = 3;
    table.columns[1].format = bit_format;
    writer output;
    (void)output.add_table(table, std::vector<column_values>{
                                      column_values::logicals(logicals), column_values::bits(bits),
                                      std::span<const std::uint8_t>(bytes), std::span<const std::int16_t>(shorts),
                                      std::span<const std::int32_t>(ints), std::span<const std::int64_t>(longs),
                                      std::span(strings), std::span<const float>(floats),
                                      std::span<const double>(doubles), std::span<const std::complex<float>>(complexes),
                                      std::span<const std::complex<double>>(doubles_complex),
                                      std::span<const std::uint16_t>(unsigned_shorts)});
    return written(output, {.checksums = true});
}

TEST(cell_writes, write_the_fields_of_every_type_of_column)
{
    memory_file file(every_column_file());
    editor& edit = file.edit();
    const std::vector<std::uint8_t> logical{0};
    edit.write_cells(1, "L", column_values::logicals(logical), {.first_row = 2});
    const std::vector<std::uint8_t> bits{0, 1, 0};
    edit.write_cells(1, "X", column_values::bits(bits), {.first_row = 1});
    const std::vector<std::uint8_t> bytes{9};
    edit.write_cells(1, "B", std::span<const std::uint8_t>(bytes));
    const std::vector<std::int16_t> shorts{-9};
    edit.write_cells(1, "I", std::span<const std::int16_t>(shorts), {.first_row = 1});
    const std::vector<std::int32_t> ints{900};
    edit.write_cells(1, "J", std::span<const std::int32_t>(ints), {.first_row = 2});
    const std::vector<double> longs{-7.0};
    edit.write_cells(1, "K", std::span<const double>(longs));
    const std::vector<std::string> strings{"zz"};
    edit.write_cells(1, "A", std::span<const std::string>(strings), {.first_row = 2});
    const std::vector<float> floats{-0.5F};
    edit.write_cells(1, "E", std::span<const float>(floats), {.first_row = 1});
    const std::vector<double> doubles{-0.25};
    edit.write_cells(1, "D", std::span<const double>(doubles));
    const std::vector<std::complex<float>> complexes{{7, 8}};
    edit.write_cells(1, "C", std::span<const std::complex<float>>(complexes), {.first_row = 1});
    const std::vector<std::complex<double>> doubles_complex{{9, -9}};
    edit.write_cells(1, "M", std::span<const std::complex<double>>(doubles_complex), {.first_row = 2});
    const std::vector<std::uint16_t> unsigned_shorts{40000};
    edit.write_cells(1, "U", std::span<const std::uint16_t>(unsigned_shorts));
    edit.commit();

    const reader edited = file.read();
    EXPECT_EQ(edited.read_column<std::uint8_t>(1, "L"), (std::vector<std::uint8_t>{1, 0, 0}));
    EXPECT_EQ(edited.read_column<std::uint8_t>(1, "X"), (std::vector<std::uint8_t>{1, 0, 1, 0, 1, 0, 1, 0, 1}));
    EXPECT_EQ(edited.read_column<std::uint8_t>(1, "B"), (std::vector<std::uint8_t>{9, 2, 3}));
    EXPECT_EQ(edited.read_column<std::int16_t>(1, "I"), (std::vector<std::int16_t>{-1, -9, -3}));
    EXPECT_EQ(edited.read_column<std::int32_t>(1, "J"), (std::vector<std::int32_t>{100, 200, 900}));
    EXPECT_EQ(edited.read_column<std::int64_t>(1, "K"), (std::vector<std::int64_t>{-7, 2, 3}));
    EXPECT_EQ(edited.read_strings(1, "A"), (std::vector<std::string>{"a  ", "bb ", "zz "}));
    EXPECT_EQ(edited.read_column<float>(1, "E"), (std::vector<float>{0.5F, -0.5F, 2.5F}));
    EXPECT_EQ(edited.read_column<double>(1, "D"), (std::vector<double>{-0.25, 1.25, 2.25}));
    EXPECT_EQ(edited.read_column<std::complex<float>>(1, "C")[1], (std::complex<float>{7, 8}));
    EXPECT_EQ(edited.read_column<std::complex<double>>(1, "M")[2], (std::complex<double>{9, -9}));
    EXPECT_EQ(edited.read_column<std::uint16_t>(1, "U"), (std::vector<std::uint16_t>{40000, 1, 2}));
    EXPECT_EQ(edited.verify(1).datasum, checksum_state::valid);
    EXPECT_EQ(edited.verify(1).checksum, checksum_state::valid);
}

TEST(cell_writes, write_the_fields_of_an_ascii_table_as_text)
{
    std::vector<std::int32_t> ints{1, 22, 333};
    std::vector<double> reals{1.5, 2.5, 3.5};
    std::vector<std::string> names{"one", "two", "three"};
    writer output;
    (void)output.add_table(
        {.kind = hdu_kind::table, .rows = 3, .columns = {{.name = "N"}, {.name = "R"}, {.name = "S"}}},
        std::vector<column_values>{std::span<const std::int32_t>(ints), std::span<const double>(reals),
                                   std::span(names)});
    memory_file file(written(output, {.checksums = true}));
    const std::vector<std::int32_t> number{-4444};
    file.edit().write_cells(1, "N", std::span<const std::int32_t>(number), {.first_row = 1});
    const std::vector<double> real{-9.25};
    file.edit().write_cells(1, "R", std::span<const double>(real), {.first_row = 2});
    const std::vector<std::string> name{"deux"};
    file.edit().write_cells(1, "S", std::span<const std::string>(name), {.first_row = 1});
    file.edit().commit();
    const reader edited = file.read();
    EXPECT_EQ(edited.read_column<std::int32_t>(1, "N"), (std::vector<std::int32_t>{1, -4444, 333}));
    EXPECT_EQ(edited.read_column<double>(1, "R"), (std::vector<double>{1.5, 2.5, -9.25}));
    EXPECT_EQ(edited.read_strings(1, "S", {.trim_strings = true}), (std::vector<std::string>{"one", "deux", "three"}));
    EXPECT_EQ(edited.verify(1).checksum, checksum_state::valid);
    // A number that its field cannot hold is refused when it is written.
    const std::vector<std::int64_t> wide{12345678901234};
    EXPECT_TRUE(throws<openfits::invalid_data_error>(
        errc::value_out_of_range, [&] { file.edit().write_cells(1, "N", std::span<const std::int64_t>(wide)); }));
}

TEST(cell_writes, refuse_variable_length_arrays_and_compressed_tables)
{
    std::vector<std::int16_t> elements{1, 2, 3};
    std::vector<std::uint64_t> offsets{0, 1, 3};
    std::vector<std::int32_t> numbers{1, 2};
    writer output;
    (void)output.add_table(
        {.kind = hdu_kind::bintable, .rows = 2, .columns = {{.name = "A"}}},
        std::vector<column_values>{column_values::arrays(std::span<const std::int16_t>(elements), offsets)});
    (void)output.add_table({.kind = hdu_kind::bintable, .rows = 2, .columns = {{.name = "N"}}},
                           std::vector<column_values>{std::span<const std::int32_t>(numbers)},
                           {.compression = compression_options{.type = compression::gzip_1}});
    memory_file file(written(output));
    EXPECT_EQ(file.edit().file().hdu(2).kind(), hdu_kind::compressed_table);
    const std::vector<std::int16_t> value{5};
    EXPECT_TRUE(throws<usage_error>(errc::not_editable,
                                    [&] { file.edit().write_cells(1, "A", std::span<const std::int16_t>(value)); }));
    const std::vector<std::int32_t> number{5};
    EXPECT_TRUE(throws<usage_error>(errc::not_editable,
                                    [&] { file.edit().write_cells(2, "N", std::span<const std::int32_t>(number)); }));
    EXPECT_TRUE(throws<usage_error>(errc::not_editable, [&] {
        file.edit().append_rows(2, std::vector<column_values>{std::span<const std::int32_t>(number)});
    }));
    // The keywords of a compressed table are those of the table it holds, written back through its Z keywords.
    file.edit().header(2).set("OBJECT", "the table");
    file.edit().commit();
    const reader edited = file.read();
    EXPECT_EQ(edited.table(2).header.value<std::string>("OBJECT"), "the table");
    EXPECT_EQ(edited.read_column<std::int32_t>(2, "N"), numbers);
}

// A compressed 16-bit image of 40 x 30 values in tiles of 10 x 10.
std::vector<std::uint16_t> tile_values()
{
    std::vector<std::uint16_t> values(1200);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = static_cast<std::uint16_t>((i * 13) % 1000);
    }
    return values;
}

std::vector<std::byte> compressed_file(compression algorithm, bool checksums)
{
    const std::vector<std::uint16_t> values = tile_values();
    image_info info;
    info.geometry.axes = {40, 30};
    info.header.set("OBJECT", "packed");
    writer output;
    (void)output.add_image(info, std::span<const std::uint16_t>(values),
                           {.compression = compression_options{.type = algorithm, .tile = {10, 10}}});
    return written(output, {.checksums = checksums});
}

class compressed_writes : public testing::TestWithParam<compression>
{};

TEST_P(compressed_writes, encode_again_the_tiles_that_a_region_meets_and_keep_the_others)
{
    const std::vector<std::byte> original = compressed_file(GetParam(), true);
    memory_file file(original);
    // A region within tiles 1 and 2 of the first band of tiles, across their boundary.
    const std::vector<std::uint16_t> patch{1, 2, 3, 4, 5, 6};
    file.edit().write_pixels(1, std::span<const std::uint16_t>(patch),
                             {.region = region{.start = {18, 4}, .size = {3, 2}}});
    file.edit().commit();
    const reader edited = file.read();
    std::vector<std::uint16_t> expected = tile_values();
    for (std::size_t y = 0; y < 2; ++y) {
        for (std::size_t x = 0; x < 3; ++x) {
            expected[((4 + y) * 40) + 18 + x] = patch[(y * 3) + x];
        }
    }
    EXPECT_EQ(edited.read_pixels<std::uint16_t>(1), expected);
    EXPECT_EQ(edited.verify(1).datasum, checksum_state::valid);
    EXPECT_EQ(edited.verify(1).checksum, checksum_state::valid);
    // Every tile but the two that the region meets keeps its compressed bytes.
    const reader before(std::make_unique<openfits::memory_source>(std::vector<std::byte>(original)));
    const std::string column = GetParam() == compression::nocompress ? "UNCOMPRESSED_DATA" : "COMPRESSED_DATA";
    const auto tiles_before = before.read_arrays<std::int32_t>(1, column);
    const auto tiles_after = edited.read_arrays<std::int32_t>(1, column);
    ASSERT_EQ(tiles_before.size(), 12U);
    ASSERT_EQ(tiles_after.size(), 12U);
    for (std::size_t t = 0; t < 12; ++t) {
        if (t != 1 && t != 2) {
            EXPECT_TRUE(std::ranges::equal(tiles_before[t], tiles_after[t])) << "tile " << t;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(every_algorithm, compressed_writes,
                         testing::Values(compression::rice, compression::gzip_1, compression::gzip_2, compression::plio,
                                         compression::hcompress, compression::nocompress));

TEST(compressed_writes, quantize_the_tiles_again_at_the_spacing_of_their_levels)
{
    std::vector<float> values(1200);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = static_cast<float>(std::sin(static_cast<double>(i) * 0.37)) * 100.0F;
    }
    image_info info;
    info.geometry.axes = {40, 30};
    writer output;
    (void)output.add_image(
        info, std::span<const float>(values),
        {.compression = compression_options{
             .type = compression::rice, .tile = {40, 10}, .quantize_level = -0.01, .dither_seed = 77}});
    const std::vector<std::byte> original = written(output, {.checksums = true});
    memory_file file(original);
    std::vector<float> patch(10, 12.3456F);
    patch[3] = std::nanf("");
    file.edit().write_pixels(1, std::span<const float>(patch), {.region = region{.start = {5, 12}, .size = {10, 1}}});
    file.edit().commit();
    const reader edited = file.read();
    const std::vector<float> read = edited.read_pixels<float>(1);
    for (std::size_t i = 0; i < 10; ++i) {
        const float value = read[(12 * 40) + 5 + i];
        if (i == 3) {
            EXPECT_TRUE(std::isnan(value));
        } else {
            EXPECT_NEAR(value, 12.3456F, 0.01F);
        }
    }
    // The other tiles are as they were, value for value, and the image keeps its seed.
    const reader before(std::make_unique<openfits::memory_source>(std::vector<std::byte>(original)));
    const std::vector<float> old = before.read_pixels<float>(1);
    for (std::size_t i = 0; i < 400; ++i) {
        EXPECT_EQ(read[i], old[i]);
    }
    EXPECT_EQ(edited.hdu(1).header().value<int>("ZDITHER0"), 77);
    EXPECT_TRUE(edited.hdu(1).header().contains("ZBLANK"));
    EXPECT_EQ(edited.verify(1).checksum, checksum_state::valid);
}

TEST(compressed_headers, are_edited_as_the_header_of_the_image_they_hold)
{
    memory_file file(compressed_file(compression::rice, true));
    // The image is the primary one, compressed after a primary HDU without data (standard §10.1.2).
    openfits::header& cards = file.edit().header(1);
    EXPECT_EQ(cards[0].keyword(), "SIMPLE");
    cards.set("OBJECT", "repacked");
    cards.append(openfits::card("EXPTIME", 12.5));
    file.edit().commit();
    const reader edited = file.read();
    EXPECT_EQ(edited.image(1).header.value<std::string>("OBJECT"), "repacked");
    EXPECT_EQ(edited.image(1).header.value<double>("EXPTIME"), 12.5);
    EXPECT_EQ(edited.hdu(1).header().value<std::string>("ZCMPTYPE"), "RICE_1");
    EXPECT_EQ(edited.verify(1).checksum, checksum_state::valid);
    EXPECT_EQ(edited.read_pixels<std::uint16_t>(1), tile_values());
    // Its structural keywords stay as they are.
    file.edit().header(1).set("NAXIS1", 41);
    EXPECT_TRUE(throws<openfits::validation_error>(errc::structural_keyword, [&file] { file.edit().commit(); }));
}

// The sum of the blocks of a header as its records make them, as a decompression writes the header of an image.
std::uint32_t restored_sum(const openfits::header& cards)
{
    const std::string blocks = openfits::detail::edited_blocks(
        cards, {.hierarch = true, .truncate_comment = true, .non_ascii = openfits::detail::byte_policy::keep}, false,
        0);
    return openfits::detail::sum_of(std::as_bytes(std::span(blocks)));
}

// The sum of the stored values of an image as an uncompressed unit holds them: ZDATASUM.
std::uint32_t values_sum(const reader& file, std::size_t index)
{
    const std::vector<std::int16_t> stored =
        file.read_pixels<std::int16_t>(index, {.scaling = openfits::scaling_policy::raw});
    std::vector<std::byte> bytes(stored.size() * 2);
    for (std::size_t i = 0; i < stored.size(); ++i) {
        const auto value = static_cast<std::uint16_t>(stored[i]);
        bytes[2 * i] = static_cast<std::byte>(value >> 8U);
        bytes[(2 * i) + 1] = static_cast<std::byte>(value & 0xFFU);
    }
    return openfits::detail::sum_of(bytes);
}

TEST(compressed_headers, keep_the_checksums_of_the_image_that_fpack_recorded)
{
    const std::string path = std::string(OPENFITS_TEST_DATA_DIR) + "/fpack/c3-rice-checksum.fits";
    const std::string text = openfits::test::read_file(openfits::test::path_of(path));
    const std::span<const std::byte> bytes = std::as_bytes(std::span(text));
    const reader original(
        std::make_unique<openfits::memory_source>(std::vector<std::byte>(bytes.begin(), bytes.end())));
    const openfits::header before = original.image(1).header;
    ASSERT_TRUE(before.contains("CHECKSUM"));
    // The image that fpack compressed: its header as restored, and its values, sum to negative zero.
    const std::uint32_t data_before = values_sum(original, 1);
    ASSERT_EQ(before.value<std::string>("DATASUM"), std::to_string(data_before));
    ASSERT_EQ(openfits::detail::add_sums(restored_sum(before), data_before), 0xFFFFFFFFU);

    memory_file file(std::vector<std::byte>(bytes.begin(), bytes.end()));
    file.edit().header(1).set("OBJECT", "edited");
    const std::vector<std::uint16_t> patch{1000, 2000, 3000};
    file.edit().write_pixels(1, std::span<const std::uint16_t>(patch),
                             {.region = region{.start = {5, 5}, .size = {3, 1}}});
    file.edit().commit();
    const reader edited = file.read();
    const openfits::header after = edited.image(1).header;
    const std::uint32_t data_after = values_sum(edited, 1);
    EXPECT_EQ(after.value<std::string>("DATASUM"), std::to_string(data_after));
    EXPECT_EQ(openfits::detail::add_sums(restored_sum(after), data_after), 0xFFFFFFFFU);
    EXPECT_EQ(edited.verify(1).checksum, checksum_state::valid);
}

} // namespace
