// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/card.h>
#include <openfits/checksum.h>
#include <openfits/compression.h>
#include <openfits/error.h>
#include <openfits/header.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/table.h>
#include <openfits/writer.h>

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
#include <string>
#include <vector>

// The options of the writer in every combination, each file read back strictly by the reader, which verifies its
// checksums, and recorded for the other readers that CI runs over the files of the tests (astropy, fitsverify,
// funpack), as the comparison harness reads the same combinations with CFITSIO.

namespace {

using openfits::column_format;
using openfits::column_values;
using openfits::compression;
using openfits::compression_options;
using openfits::hdu_kind;
using openfits::image_info;
using openfits::reader;
using openfits::table_info;
using openfits::write_options;
using openfits::writer;
using openfits::test::sink_kind;

struct option_case
{
    bool checksums = false;
    // HIERARCH keywords, LONGSTRN and comments cut.
    bool conventions = false;
    // Every image and binary table compressed by the options of the save.
    bool compressed = false;
    // Values and rows given by functions, which take a sink that can rewrite.
    bool streamed = false;
    sink_kind sink = sink_kind::rewritable;
};

std::string name_of(const option_case& value)
{
    return std::string(value.checksums ? "checksums" : "plain") + (value.conventions ? "_conventions" : "") +
           (value.compressed ? "_compressed" : "") + (value.streamed ? "_streamed" : "_borrowed") +
           (value.sink == sink_kind::append_only ? "_append_only" : "");
}

// NOLINTNEXTLINE(readability-identifier-naming)
void PrintTo(const option_case& value, std::ostream* out)
{
    *out << name_of(value);
}

// The values of the units of the model, kept for the saves that borrow them, and a file whose first unit is copied.
struct matrix_model
{
    matrix_model()
    {
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            pixels[i] = static_cast<std::uint16_t>(i * 977);
            floats[i] = static_cast<float>(std::sin(static_cast<double>(i)));
        }
        floats[5] = std::numeric_limits<float>::quiet_NaN();
        for (std::size_t row = 0; row < 6; ++row) {
            ids.push_back(static_cast<std::int32_t>(row) * 1000 - 2500);
            names.emplace_back(row + 1, static_cast<char>('A' + row));
            for (std::size_t k = 0; k < row; ++k) {
                elements.push_back(static_cast<double>(row) + (static_cast<double>(k) / 10.0));
            }
            offsets.push_back(elements.size());
        }
        image_info copied;
        copied.geometry.axes = {4, 3};
        copied.header.set("EXTNAME", "COPIED");
        writer source;
        (void)source.add_image(copied, std::span<const std::uint16_t>(pixels).first(12));
        openfits::memory_sink sink;
        source.save(sink);
        copy_source = std::make_unique<reader>(std::make_unique<openfits::memory_source>(sink.release()));
    }

    // The writer of the case, its units in the order of read_back().
    [[nodiscard]] writer make(const option_case& options) const
    {
        image_info frame;
        frame.geometry.axes = {37, 23};
        frame.header.set("OBJECT", "the pattern", "the target");
        // A long string, with LONGSTRN, whose absence fitsverify warns of.
        frame.header.set("NOTE", note_of(options), "a string longer than a record with the conventions");
        if (options.conventions) {
            frame.header.append(openfits::card::with_value_text("ESO DET DIT", "1.50E+00", "[s] integration"));
            frame.header.set("CUT", 1, std::string(90, 'c'));
        }
        image_info nan = frame;
        nan.header.set("EXTNAME", "FLOATS");
        image_info reserved;
        reserved.geometry.axes = {10, 10};
        reserved.bitpix = openfits::bitpix::int16;
        reserved.blank = -32768;
        table_info binary{.kind = hdu_kind::bintable,
                          .rows = 6,
                          .columns = {{.name = "ID", .format = column_format::parse_binary("1J")},
                                      {.name = "NAME", .format = column_format::parse_binary("6A")},
                                      {.name = "SPECTRUM", .format = column_format::parse_binary("1PD")}}};
        table_info ascii{.kind = hdu_kind::table,
                         .rows = 6,
                         .columns = {{.name = "NAME", .format = column_format::parse_ascii("A8")},
                                     {.name = "ID", .format = column_format::parse_ascii("I8")}}};

        writer output;
        if (options.streamed) {
            (void)output.add_image(frame, openfits::numeric_type::uint16, fill_of(pixels));
            (void)output.add_image(nan, openfits::numeric_type::float32, fill_of(floats));
        } else {
            (void)output.add_image(frame, std::span<const std::uint16_t>(pixels));
            (void)output.add_image(nan, std::span<const float>(floats));
        }
        (void)output.add_reserved_image(reserved, {.fill = openfits::reserved_fill::undefined_values});
        const std::vector<column_values> binary_values{std::span(ids), std::span(names),
                                                       column_values::arrays(std::span(elements), offsets)};
        const std::vector<column_values> ascii_values{std::span(names), std::span(ids)};
        if (options.streamed) {
            (void)output.add_table(binary, [given = binary_values](std::uint64_t first) {
                return first == 0 ? given : std::vector<column_values>{};
            });
            ascii.rows = 0;
            (void)output.add_table(ascii, [given = ascii_values](std::uint64_t first) {
                return first == 0 ? given : std::vector<column_values>{};
            });
        } else {
            (void)output.add_table(binary, binary_values);
            (void)output.add_table(ascii, ascii_values);
        }
        (void)output.add_hdu(*copy_source, 0);
        return output;
    }

    static std::string note_of(const option_case& options)
    {
        return options.conventions ? std::string(100, 'n') : std::string("short");
    }

    template <typename T> static openfits::image_fill fill_of(const std::vector<T>& values)
    {
        return [&values](std::uint64_t first, std::span<std::byte> chunk) {
            std::memcpy(chunk.data(), values.data() + first, chunk.size());
        };
    }

    std::vector<std::uint16_t> pixels = std::vector<std::uint16_t>(std::size_t{37} * 23);
    std::vector<float> floats = std::vector<float>(std::size_t{37} * 23);
    std::vector<std::int32_t> ids;
    std::vector<std::string> names;
    std::vector<double> elements;
    std::vector<std::uint64_t> offsets{0};
    std::unique_ptr<reader> copy_source;
};

class write_matrix : public testing::TestWithParam<option_case>
{};

TEST_P(write_matrix, every_unit_reads_back)
{
    const option_case& parameter = GetParam();
    const matrix_model model;
    const writer output = model.make(parameter);
    write_options options{.hierarch = parameter.conventions,
                          .longstrn = parameter.conventions,
                          .truncate_comments = parameter.conventions,
                          .checksums = parameter.checksums};
    if (parameter.compressed) {
        options.compression = compression_options{.type = compression::automatic};
    }
    const std::vector<std::byte> file = openfits::test::written(output, options, parameter.sink);
    // A strict open verifies every checksum, and fails at a unit it cannot read.
    const reader input(std::make_unique<openfits::memory_source>(std::vector<std::byte>(file)), {.strict = true});
    // A first image that the options compress comes after a primary HDU without data.
    const std::size_t first = parameter.compressed ? 1U : 0U;
    ASSERT_EQ(input.hdu_count(), first + 6);
    EXPECT_EQ(input.read_pixels<std::uint16_t>(first), model.pixels);
    const std::vector<float> floats = input.read_pixels<float>(first + 1);
    for (std::size_t i = 0; i < floats.size(); ++i) {
        EXPECT_TRUE(std::isnan(model.floats[i]) ? std::isnan(floats[i]) : floats[i] == model.floats[i]) << i;
    }
    const std::vector<float> reserved = input.read_pixels<float>(first + 2);
    EXPECT_TRUE(std::ranges::all_of(reserved, [](float value) { return std::isnan(value); }));
    EXPECT_EQ(input.read_column<std::int32_t>(first + 3, 0), model.ids);
    EXPECT_EQ(input.read_strings(first + 3, 1, {.trim_strings = true}), model.names);
    EXPECT_EQ(input.read_arrays<double>(first + 3, 2).values, model.elements);
    EXPECT_EQ(input.read_strings(first + 4, 0, {.trim_strings = true}), model.names);
    EXPECT_EQ(input.read_column<std::int32_t>(first + 4, 1), model.ids);
    EXPECT_EQ(input.read_pixels<std::uint16_t>(first + 5),
              std::vector<std::uint16_t>(model.pixels.begin(), model.pixels.begin() + 12));
    const openfits::header cards = input.image(first).header;
    EXPECT_EQ(cards.value<std::string>("NOTE"), matrix_model::note_of(parameter));
    EXPECT_EQ(cards.contains("LONGSTRN"), parameter.conventions);
    if (parameter.conventions) {
        EXPECT_EQ(cards.value<double>("ESO DET DIT"), 1.5);
        EXPECT_EQ(cards.comment("CUT"), std::string(47, 'c'));
    }
    // Every unit has the checksums of the save, the copy too, whose source has none.
    const openfits::checksum_state expected =
        parameter.checksums ? openfits::checksum_state::valid : openfits::checksum_state::absent;
    for (std::size_t i = 0; i < input.hdu_count(); ++i) {
        const openfits::checksum_report sums = input.verify(i);
        EXPECT_EQ(sums.checksum, expected) << i;
        EXPECT_EQ(sums.datasum, expected) << i;
    }
}

std::vector<option_case> option_cases()
{
    std::vector<option_case> cases;
    for (const bool checksums : {false, true}) {
        for (const bool conventions : {false, true}) {
            for (const bool compressed : {false, true}) {
                cases.push_back({.checksums = checksums,
                                 .conventions = conventions,
                                 .compressed = compressed,
                                 .streamed = false,
                                 .sink = sink_kind::rewritable});
                cases.push_back({.checksums = checksums,
                                 .conventions = conventions,
                                 .compressed = compressed,
                                 .streamed = false,
                                 .sink = sink_kind::append_only});
                cases.push_back({.checksums = checksums,
                                 .conventions = conventions,
                                 .compressed = compressed,
                                 .streamed = true,
                                 .sink = sink_kind::rewritable});
            }
        }
    }
    return cases;
}

INSTANTIATE_TEST_SUITE_P(every_combination, write_matrix, testing::ValuesIn(option_cases()),
                         [](const testing::TestParamInfo<option_case>& instance) { return name_of(instance.param); });

} // namespace
