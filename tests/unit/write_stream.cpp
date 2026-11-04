// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/checksum.h>
#include <openfits/compression.h>
#include <openfits/error.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/table.h>
#include <openfits/writer.h>

#include "support/checksums.h"
#include "support/files.h"
#include "support/temp_directory.h"
#include "support/throws.h"
#include "support/written_file.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// Streamed and reserved data units: images whose values a function gives chunk after chunk, tables whose rows it gives,
// and images whose data are reserved for an editor, written as the values in memory would be.

namespace {

using openfits::bitpix;
using openfits::column_format;
using openfits::column_values;
using openfits::compression;
using openfits::compression_options;
using openfits::errc;
using openfits::hdu_kind;
using openfits::image_info;
using openfits::numeric_type;
using openfits::reader;
using openfits::table_info;
using openfits::usage_error;
using openfits::write_options;
using openfits::writer;
using openfits::test::sink_kind;
using openfits::test::throws;
using openfits::test::written;

reader open(const std::vector<std::byte>& file)
{
    return reader(std::make_unique<openfits::memory_source>(std::vector<std::byte>(file)));
}

image_info image_of(std::vector<std::uint64_t> axes)
{
    image_info info;
    info.geometry.axes = std::move(axes);
    return info;
}

// The calls of a fill function: the first element of each and its length.
struct fill_call
{
    std::uint64_t first = 0;
    std::uint64_t count = 0;
};

// A fill function that copies the values from those of a vector, and records its calls.
template <typename T>
openfits::image_fill filling(const std::vector<T>& values, std::vector<fill_call>* calls = nullptr)
{
    return [&values, calls](std::uint64_t first, std::span<std::byte> chunk) {
        const std::size_t count = chunk.size() / sizeof(T);
        if (calls != nullptr) {
            calls->push_back({.first = first, .count = count});
        }
        std::memcpy(chunk.data(), values.data() + first, chunk.size());
    };
}

// The values of a pattern of count elements, of every magnitude of T.
template <typename T> std::vector<T> pattern_of(std::size_t count)
{
    std::vector<T> values(count);
    for (std::size_t i = 0; i < count; ++i) {
        if constexpr (std::floating_point<T>) {
            values[i] = static_cast<T>(std::sin(static_cast<double>(i)) * 1000.0);
        } else {
            values[i] = static_cast<T>((i * 2654435761U) >> 7U);
        }
    }
    return values;
}

// A model of an image, its options and the options of the save, which a streamed image writes as its values in memory.
struct image_case
{
    std::string name;
    numeric_type type = numeric_type::uint16;
    std::vector<std::uint64_t> axes{};
    std::optional<bitpix> stored{};
    openfits::scaling scaling{};
    std::optional<std::int64_t> blank{};
    openfits::image_write_options options{};
    write_options save{};
};

// NOLINTNEXTLINE(readability-identifier-naming)
void PrintTo(const image_case& value, std::ostream* out)
{
    *out << value.name;
}

class streamed_images : public testing::TestWithParam<image_case>
{};

template <typename T> void check_streamed_image(const image_case& parameter)
{
    std::size_t count = 1;
    for (const std::uint64_t axis : parameter.axes) {
        count *= axis;
    }
    std::vector<T> values = pattern_of<T>(count);
    if constexpr (std::floating_point<T>) {
        if (parameter.blank) {
            values[3] = std::numeric_limits<T>::quiet_NaN();
        }
    }
    image_info info = image_of(parameter.axes);
    info.bitpix = parameter.stored;
    info.scaling = parameter.scaling;
    info.blank = parameter.blank;
    info.header.set("OBJECT", "a streamed image");

    writer borrowed;
    (void)borrowed.add_image(info, std::span<const T>(values), parameter.options);
    std::vector<fill_call> calls;
    writer streamed;
    (void)streamed.add_image(info, openfits::numeric_type_of<T>(), filling(values, &calls), parameter.options);

    const std::vector<std::byte> file = written(borrowed, parameter.save);
    EXPECT_EQ(written(streamed, parameter.save), file);
    // The calls of the function cover the values once, in order.
    std::uint64_t next = 0;
    for (const fill_call& call : calls) {
        EXPECT_EQ(call.first, next);
        next = call.first + call.count;
    }
    EXPECT_EQ(next, count);
    // The file reads back the values.
    if (!parameter.save.compression && !parameter.options.compression) {
        const reader input = open(file);
        EXPECT_EQ(input.image(input.hdu_count() - 1).geometry.axes, parameter.axes);
    }
}

TEST_P(streamed_images, give_the_file_of_their_values_in_memory)
{
    const image_case& parameter = GetParam();
    switch (parameter.type) {
    case numeric_type::uint16:
        check_streamed_image<std::uint16_t>(parameter);
        break;
    case numeric_type::int32:
        check_streamed_image<std::int32_t>(parameter);
        break;
    case numeric_type::float32:
        check_streamed_image<float>(parameter);
        break;
    case numeric_type::float64:
        check_streamed_image<double>(parameter);
        break;
    default:
        check_streamed_image<std::uint8_t>(parameter);
        break;
    }
}

std::vector<image_case> image_cases()
{
    const compression_options rice{.type = compression::rice};
    const compression_options squares{.type = compression::rice, .tile = {16, 16}};
    const compression_options hcompress{.type = compression::hcompress};
    const compression_options lossless{.type = compression::gzip_2};
    const compression_options quantized{.type = compression::rice, .quantize_level = 4.0};
    const compression_options plio{.type = compression::plio};
    return {
        {.name = "u16", .type = numeric_type::uint16, .axes = {37, 23}},
        {.name = "u16_checksums", .type = numeric_type::uint16, .axes = {37, 23}, .save = {.checksums = true}},
        {.name = "u8_1d", .type = numeric_type::uint8, .axes = {1000}},
        {.name = "i32_cube", .type = numeric_type::int32, .axes = {5, 4, 3}, .save = {.checksums = true}},
        {.name = "f32", .type = numeric_type::float32, .axes = {37, 23}},
        {.name = "f64_scaled_into_i16_with_blank",
         .type = numeric_type::float64,
         .axes = {37, 23},
         .stored = bitpix::int16,
         .scaling = {.scale = 0.1, .zero = 10.0},
         .blank = -32768},
        {.name = "i32_raw",
         .type = numeric_type::int32,
         .axes = {37, 23},
         .stored = bitpix::int32,
         .scaling = {.scale = 2.0},
         .options = {.scaling = openfits::scaling_policy::raw}},
        {.name = "u16_override",
         .type = numeric_type::uint16,
         .axes = {37, 23},
         .stored = bitpix::int16,
         .scaling = {.scale = 3.0},
         .options = {.scaling = openfits::scaling_policy::override, .override_scaling = {.zero = 32768.0}}},
        {.name = "u16_rice", .type = numeric_type::uint16, .axes = {37, 23}, .options = {.compression = rice}},
        {.name = "u16_rice_squares_checksums",
         .type = numeric_type::uint16,
         .axes = {37, 23},
         .options = {.compression = squares},
         .save = {.checksums = true}},
        {.name = "u16_rice_first_by_the_file",
         .type = numeric_type::uint16,
         .axes = {37, 23},
         .save = {.compression = rice}},
        {.name = "i32_hcompress", .type = numeric_type::int32, .axes = {37, 41}, .options = {.compression = hcompress}},
        {.name = "f32_gzip_2", .type = numeric_type::float32, .axes = {37, 23}, .options = {.compression = lossless}},
        {.name = "f32_quantized",
         .type = numeric_type::float32,
         .axes = {37, 23},
         .options = {.compression = quantized}},
        {.name = "u8_plio", .type = numeric_type::uint8, .axes = {37, 23}, .options = {.compression = plio}},
        {.name = "i32_cube_rice", .type = numeric_type::int32, .axes = {5, 4, 3}, .options = {.compression = rice}},
    };
}

INSTANTIATE_TEST_SUITE_P(every_kind, streamed_images, testing::ValuesIn(image_cases()),
                         [](const testing::TestParamInfo<image_case>& instance) { return instance.param.name; });

// A save asks for runs of whole rows, as many as fit in about 256 KiB of values, and parts of a row larger than that.
TEST(streamed_image, the_function_fills_runs_of_whole_rows)
{
    // Rows of 8 KiB of floats, 32 of them a run.
    const std::vector<float> wide(std::size_t{2048} * 64, 1.5F);
    std::vector<fill_call> calls;
    writer rows;
    (void)rows.add_image(image_of({2048, 64}), numeric_type::float32, filling(wide, &calls));
    openfits::callback_sink discard([](std::span<const std::byte>) {});
    rows.save(discard);
    ASSERT_EQ(calls.size(), 2U);
    EXPECT_EQ(calls[0].first, 0U);
    EXPECT_EQ(calls[0].count, std::uint64_t{32} * 2048);
    EXPECT_EQ(calls[1].first, std::uint64_t{32} * 2048);

    // Rows of more than 256 KiB of values come in parts of 256 KiB.
    const std::vector<float> long_rows(std::size_t{70000} * 2, 2.5F);
    calls.clear();
    writer parts;
    (void)parts.add_image(image_of({70000, 2}), numeric_type::float32, filling(long_rows, &calls));
    parts.save(discard);
    ASSERT_EQ(calls.size(), 3U);
    EXPECT_EQ(calls[0].count, std::uint64_t{1} << 16U);
    EXPECT_EQ(calls[1].first, std::uint64_t{1} << 16U);
    EXPECT_EQ(calls[2].first + calls[2].count, std::uint64_t{140000});
}

// A compressed output is written from an image already in a file, read row by row as the stream asks for them: the way
// an image filled in place in a reserved unit is compressed afterwards, since compressed tiles cannot be filled in
// place.
TEST(streamed_image, compresses_an_image_read_from_a_file_row_by_row)
{
    const std::vector<std::uint16_t> values = pattern_of<std::uint16_t>(std::size_t{300} * 200);
    writer plain;
    (void)plain.add_image(image_of({300, 200}), std::span<const std::uint16_t>(values));
    const reader input = open(written(plain));
    const openfits::image_write_options rice{.compression =
                                                 compression_options{.type = compression::rice, .tile = {300, 16}}};
    writer borrowed;
    (void)borrowed.add_image(image_of({300, 200}), std::span<const std::uint16_t>(values), rice);
    writer streamed;
    (void)streamed.add_image<std::uint16_t>(
        image_of({300, 200}),
        [&input](std::uint64_t first, std::span<std::uint16_t> rows) {
            // Whole rows, the first axis of the image.
            ASSERT_EQ(first % 300, 0U);
            ASSERT_EQ(rows.size() % 300, 0U);
            (void)input.read_pixels(
                0, rows, {.region = openfits::region{.start = {0, first / 300}, .size = {300, rows.size() / 300}}});
        },
        rice);
    EXPECT_EQ(written(streamed), written(borrowed));
}

// The typed form of the function takes values of its type.
TEST(streamed_image, takes_a_function_of_typed_values)
{
    writer output;
    (void)output.add_image<std::int16_t>(image_of({4, 2}), [](std::uint64_t first, std::span<std::int16_t> values) {
        for (std::size_t i = 0; i < values.size(); ++i) {
            values[i] = static_cast<std::int16_t>(-100 + static_cast<int>(first + i));
        }
    });
    EXPECT_EQ(open(written(output)).read_pixels<std::int16_t>(0),
              (std::vector<std::int16_t>{-100, -99, -98, -97, -96, -95, -94, -93}));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] {
        (void)output.add_image(image_of({2}), numeric_type::uint8, openfits::image_fill{});
    }));
}

// Standard §4.4.2.7: the checksums of streamed data are known once they are written, and completed in the header
// in place, which a sink that cannot rewrite refuses before it receives anything; compressed data are held in memory
// until written, and summed there.
TEST(streamed_image, checksums_need_a_sink_that_can_rewrite)
{
    const std::vector<std::uint16_t> values = pattern_of<std::uint16_t>(100);
    writer output;
    (void)output.add_image(image_of({10, 10}), numeric_type::uint16, filling(values));
    std::uint64_t received = 0;
    openfits::callback_sink append_only([&received](std::span<const std::byte> bytes) { received += bytes.size(); });
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { output.save(append_only, {.checksums = true}); }));
    EXPECT_EQ(received, 0U);
    // Without checksums, or compressed, it takes them.
    EXPECT_NO_THROW(output.save(append_only));
    writer compressed;
    (void)compressed.add_image(image_of({10, 10}), numeric_type::uint16, filling(values),
                               {.compression = compression_options{.type = compression::rice}});
    const std::vector<std::byte> file = written(compressed, {.checksums = true}, sink_kind::append_only);
    const reader input = open(file);
    EXPECT_EQ(input.verify(1).checksum, openfits::checksum_state::valid);
    EXPECT_EQ(input.read_pixels<std::uint16_t>(1), values);
}

// A failure of the function ends the save with its exception, and a save to a path leaves the file as it was.
TEST(streamed_image, a_failed_save_leaves_the_file_as_it_was)
{
    const openfits::test::temp_directory directory;
    const std::string path = directory.file("image.fits");
    const std::vector<std::uint8_t> values(std::size_t{8} << 17, 7);
    writer good;
    (void)good.add_image(image_of({8, 1 << 17}), std::span<const std::uint8_t>(values));
    good.save(path);
    const std::string before = openfits::test::read_file(directory.path() / "image.fits");
    writer failing;
    (void)failing.add_image(image_of({8, 1 << 17}), numeric_type::uint8, [](std::uint64_t first, std::span<std::byte>) {
        if (first > 0) {
            throw std::runtime_error("the camera was unplugged");
        }
    });
    EXPECT_THROW(failing.save(path, {.checksums = true}), std::runtime_error);
    EXPECT_EQ(openfits::test::read_file(directory.path() / "image.fits"), before);
    EXPECT_EQ(directory.entries(), 1U);
    // The progress counts the bytes as they are written, and can cancel the save there too.
    EXPECT_TRUE(throws<openfits::cancelled_error>(errc::cancelled, [&] {
        failing.save(path, {.progress = [](std::uint64_t done, std::uint64_t) { return done < 2880; }});
    }));
    EXPECT_EQ(directory.entries(), 1U);
}

table_info streamed_table(std::uint64_t rows)
{
    return table_info{.kind = hdu_kind::bintable,
                      .rows = rows,
                      .columns = {{.name = "ID", .format = column_format::parse_binary("1J")},
                                  {.name = "NAME", .format = column_format::parse_binary("8A")},
                                  {.name = "FLUX", .format = column_format::parse_binary("1D"), .unit = "Jy"},
                                  {.name = "SEEN", .format = column_format::parse_binary("1L")}}};
}

// The rows of a catalogue, given in chunks of chunk rows.
struct catalogue
{
    explicit catalogue(std::size_t rows)
    {
        for (std::size_t i = 0; i < rows; ++i) {
            ids.push_back(static_cast<std::int32_t>(i * 7) - 100);
            names.push_back("S" + std::to_string(i));
            fluxes.push_back(static_cast<double>(i) / 3.0);
            seen.push_back(static_cast<std::uint8_t>(i % 2));
        }
    }

    [[nodiscard]] std::vector<column_values> all() const
    {
        return {std::span(ids), std::span(names), std::span(fluxes), column_values::logicals(std::span(seen))};
    }

    [[nodiscard]] openfits::table_rows rows(std::uint64_t chunk) const
    {
        return [this, chunk](std::uint64_t first) -> std::vector<column_values> {
            if (first >= ids.size()) {
                return {};
            }
            const std::size_t count = std::min<std::size_t>(chunk, ids.size() - first);
            return {std::span(ids).subspan(first, count), std::span(names).subspan(first, count),
                    std::span(fluxes).subspan(first, count),
                    column_values::logicals(std::span(seen).subspan(first, count))};
        };
    }

    std::vector<std::int32_t> ids;
    std::vector<std::string> names;
    std::vector<double> fluxes;
    std::vector<std::uint8_t> seen;
};

// A table whose rows a function gives, their number known or not, gives the file of the same rows in memory; one of
// unknown rows has its NAXIS2 written once they are, which a sink that can rewrite takes.
TEST(streamed_table, gives_the_file_of_its_rows_in_memory)
{
    const catalogue rows(1000);
    writer borrowed;
    (void)borrowed.add_table(streamed_table(1000), rows.all());
    for (const bool checksums : {false, true}) {
        const std::vector<std::byte> file = written(borrowed, {.checksums = checksums});
        for (const std::uint64_t chunk : {1U, 7U, 1000U, 5000U}) {
            for (const std::uint64_t counted : {0U, 1000U}) {
                writer streamed;
                EXPECT_EQ(streamed.add_table(streamed_table(counted), rows.rows(chunk)), 1U);
                EXPECT_EQ(written(streamed, {.checksums = checksums}), file)
                    << chunk << " rows a chunk, " << counted << " in the model";
            }
        }
    }
    // Counted rows without checksums take a sink that cannot rewrite; uncounted ones do not.
    writer counted;
    (void)counted.add_table(streamed_table(1000), rows.rows(64));
    EXPECT_EQ(written(counted, {}, sink_kind::append_only), written(borrowed));
    writer uncounted;
    (void)uncounted.add_table(streamed_table(0), rows.rows(64));
    EXPECT_TRUE(
        throws<usage_error>(errc::invalid_argument, [&] { (void)written(uncounted, {}, sink_kind::append_only); }));
    // Its size is known once the rows are written, its records before.
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { (void)uncounted.size(); }));
    EXPECT_EQ(uncounted.header_records(1), borrowed.header_records(1));
    EXPECT_EQ(counted.size(), borrowed.size());
}

TEST(streamed_table, an_ascii_table_streams_as_well)
{
    const std::vector<std::string> names{"M 31", "M 33", "NGC 7000", "IC 434", "M 42"};
    const std::vector<double> distances{0.78, 0.84, 0.8, 0.4, 0.41};
    table_info info{.kind = hdu_kind::table,
                    .columns = {{.name = "NAME", .format = column_format::parse_ascii("A10")},
                                {.name = "DIST", .format = column_format::parse_ascii("F8.3")}}};
    writer borrowed;
    info.rows = 5;
    (void)borrowed.add_table(info, std::vector<column_values>{std::span(names), std::span(distances)});
    info.rows = 0;
    writer streamed;
    (void)streamed.add_table(info, [&](std::uint64_t first) -> std::vector<column_values> {
        if (first >= names.size()) {
            return {};
        }
        const std::size_t count = std::min<std::size_t>(2, names.size() - first);
        return {std::span(names).subspan(first, count), std::span(distances).subspan(first, count)};
    });
    EXPECT_EQ(written(streamed), written(borrowed));
}

// Standard §7.3.5: the heap follows the rows, which are written as zeros first, then in their place once the arrays of
// their chunk are in the heap; the size of the heap and the emax of each column complete the header.
TEST(streamed_table, a_table_of_arrays_writes_its_rows_in_place)
{
    std::vector<std::int16_t> elements;
    std::vector<std::uint64_t> offsets{0};
    std::vector<std::string> notes;
    for (std::size_t row = 0; row < 300; ++row) {
        for (std::size_t k = 0; k < row % 13; ++k) {
            elements.push_back(static_cast<std::int16_t>((row * 31) + k));
        }
        offsets.push_back(elements.size());
        notes.emplace_back(row % 5, static_cast<char>('a' + (row % 26)));
    }
    table_info info{.kind = hdu_kind::bintable,
                    .rows = 300,
                    .columns = {{.name = "SPECTRUM", .format = column_format::parse_binary("1PI")},
                                {.name = "NOTE", .format = column_format::parse_binary("1PA")}}};
    writer borrowed;
    (void)borrowed.add_table(info, std::vector<column_values>{column_values::arrays(std::span(elements), offsets),
                                                              column_values::arrays(std::span(notes))});
    // The offsets of the arrays of each chunk, from its first element, valid until the next chunk.
    const auto stream = [&](std::uint64_t chunk) -> openfits::table_rows {
        auto part = std::make_shared<std::vector<std::uint64_t>>();
        return [&, chunk, part](std::uint64_t first) -> std::vector<column_values> {
            const std::size_t count = std::min<std::size_t>(chunk, 300 - first);
            part->clear();
            for (std::size_t r = first; r <= first + count; ++r) {
                part->push_back(offsets[r] - offsets[first]);
            }
            const std::span<const std::int16_t> arrays =
                std::span(elements).subspan(offsets[first], offsets[first + count] - offsets[first]);
            return {column_values::arrays(arrays, *part),
                    column_values::arrays(std::span(notes).subspan(first, count))};
        };
    };
    // In one chunk, the heap is that of the rows in memory; in several, each chunk's arrays come in its turn.
    writer whole;
    (void)whole.add_table(info, stream(300));
    for (const bool checksums : {false, true}) {
        EXPECT_EQ(written(whole, {.checksums = checksums}), written(borrowed, {.checksums = checksums}));
    }
    writer chunked;
    (void)chunked.add_table(info, stream(17));
    const std::vector<std::byte> file = written(chunked, {.checksums = true});
    const reader input(std::make_unique<openfits::memory_source>(std::vector<std::byte>(file)), {.strict = true});
    const openfits::column_arrays<std::int16_t> arrays = input.read_arrays<std::int16_t>(1, 0);
    EXPECT_EQ(arrays.values, elements);
    EXPECT_EQ(arrays.offsets, offsets);
    EXPECT_EQ(input.read_strings(1, 1), notes);
    EXPECT_EQ(input.hdu(1).header().value<std::string>("TFORM1"), "1PI(12)");
    EXPECT_EQ(input.verify(1).datasum, openfits::checksum_state::valid);
    // The heap: the elements, and the characters of the notes, an empty one a NUL byte.
    std::uint64_t heap = elements.size() * 2;
    for (const std::string& note : notes) {
        heap += std::max<std::size_t>(note.size(), 1);
    }
    EXPECT_EQ(input.hdu(1).header().value<std::uint64_t>("PCOUNT"), heap);
    // A sink that cannot rewrite cannot take it, and the number of its rows must be given.
    EXPECT_TRUE(
        throws<usage_error>(errc::invalid_argument, [&] { (void)written(chunked, {}, sink_kind::append_only); }));
    info.rows = 0;
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { (void)writer().add_table(info, stream(17)); }));
}

TEST(streamed_table, refuses_rows_that_do_not_fit_its_model)
{
    const catalogue rows(10);
    // More rows, or fewer, than the model counts.
    for (const std::uint64_t counted : {5U, 20U}) {
        writer output;
        (void)output.add_table(streamed_table(counted), rows.rows(4));
        EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { (void)written(output); })) << counted;
    }
    // Columns of another number, or values of another kind than their format.
    writer columns;
    (void)columns.add_table(streamed_table(0), [](std::uint64_t first) -> std::vector<column_values> {
        static const std::vector<std::int32_t> one{1};
        return first == 0 ? std::vector<column_values>{std::span(one)} : std::vector<column_values>{};
    });
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { (void)written(columns); }));
    // A column without a format, whose header comes before its rows; no function.
    table_info unformatted = streamed_table(0);
    unformatted.columns[1].format.reset();
    writer output;
    EXPECT_TRUE(
        throws<usage_error>(errc::invalid_argument, [&] { (void)output.add_table(unformatted, rows.rows(4)); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument,
                                    [&] { (void)output.add_table(streamed_table(0), openfits::table_rows{}); }));
    EXPECT_EQ(output.hdu_count(), 0U);
}

// The progress of a table whose size is known as it is written: the total grows with it.
TEST(streamed_table, the_total_of_the_progress_grows_as_the_rows_are_written)
{
    const catalogue rows(5000);
    writer output;
    (void)output.add_table(streamed_table(0), rows.rows(100));
    std::uint64_t last_done = 0;
    std::uint64_t last_total = 0;
    const std::vector<std::byte> file = written(output, {.progress = [&](std::uint64_t done, std::uint64_t total) {
                                                    EXPECT_GE(done, last_done);
                                                    EXPECT_LE(done, total);
                                                    last_done = done;
                                                    last_total = total;
                                                    return true;
                                                }});
    EXPECT_EQ(last_done, file.size());
    EXPECT_EQ(last_total, file.size());
}

// Reserved images: data of zeros or of undefined values, which an editor fills later, uncompressed whatever the
// options of the save say.
TEST(reserved_image, holds_zeros_or_undefined_values)
{
    image_info zeros = image_of({300, 200});
    zeros.bitpix = bitpix::int16;
    zeros.scaling = {.scale = 0.5, .zero = 100.0};
    image_info blanks = zeros;
    blanks.blank = -32768;
    image_info nans = image_of({300, 200});
    nans.bitpix = bitpix::float32;
    writer output;
    EXPECT_EQ(output.add_reserved_image(zeros, {.reserved_cards = 10}), 0U);
    EXPECT_EQ(output.add_reserved_image(blanks, {.fill = openfits::reserved_fill::undefined_values}), 1U);
    EXPECT_EQ(output.add_reserved_image(nans, {.fill = openfits::reserved_fill::undefined_values}), 2U);
    for (const sink_kind kind : {sink_kind::rewritable, sink_kind::append_only}) {
        const std::vector<std::byte> file =
            written(output, {.checksums = true, .compression = compression_options{}}, kind);
        const reader input(std::make_unique<openfits::memory_source>(std::vector<std::byte>(file)), {.strict = true});
        ASSERT_EQ(input.hdu_count(), 3U);
        EXPECT_EQ(input.hdu(0).header().free_cards(), 10U);
        EXPECT_EQ(input.read_pixels<std::int16_t>(0, {.scaling = openfits::scaling_policy::raw}),
                  std::vector<std::int16_t>(60000, 0));
        EXPECT_EQ(input.read_pixels<double>(0)[17], 100.0);
        const std::vector<float> undefined = input.read_pixels<float>(1);
        EXPECT_TRUE(std::ranges::all_of(undefined, [](float value) { return std::isnan(value); }));
        EXPECT_EQ(input.read_pixels<std::int16_t>(1, {.scaling = openfits::scaling_policy::raw})[59999], -32768);
        const std::vector<float> nan = input.read_pixels<float>(2);
        EXPECT_TRUE(std::ranges::all_of(nan, [](float value) { return std::isnan(value); }));
        EXPECT_EQ(input.hdu(2).kind(), hdu_kind::image);
    }
    // Undefined values of integer data need BLANK; a reserved image needs its BITPIX.
    image_info unmarked = image_of({3});
    unmarked.bitpix = bitpix::int32;
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] {
        (void)output.add_reserved_image(unmarked, {.fill = openfits::reserved_fill::undefined_values});
    }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { (void)output.add_reserved_image(image_of({3})); }));
    EXPECT_EQ(output.hdu_count(), 3U);
}

// The zeros of a reserved image reach a file as a file made longer, which the system fills with zeros.
TEST(reserved_image, makes_a_file_longer_for_its_zeros)
{
    const openfits::test::temp_directory directory;
    image_info frame = image_of({2000, 1500});
    frame.bitpix = bitpix::float32;
    writer output;
    (void)output.add_reserved_image(frame);
    output.save(directory.file("reserved.fits"), {.checksums = true});
    const reader input(directory.file("reserved.fits"));
    EXPECT_EQ(input.size(), 2880U + ((std::uint64_t{3000000} * 4 + 2879) / 2880 * 2880));
    const openfits::checksum_report sums = input.verify(0);
    EXPECT_EQ(sums.datasum, openfits::checksum_state::valid);
    EXPECT_EQ(sums.data_sum, 0U);
    std::vector<float> corner(16);
    (void)input.read_pixels(0, std::span<float>(corner),
                            {.region = openfits::region{.start = {1996, 1496}, .size = {4, 4}}});
    EXPECT_EQ(corner, std::vector<float>(16, 0.0F));
}

} // namespace
