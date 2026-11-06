// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/card.h>
#include <openfits/compression.h>
#include <openfits/error.h>
#include <openfits/header.h>
#include <openfits/io.h>
#include <openfits/table.h>
#include <openfits/writer.h>

#include <gtest/gtest.h>

#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

// The validation of the models of the writer: each rule refused with validation_error, naming the unit and the keyword,
// before anything is written.

namespace {

using openfits::bitpix;
using openfits::card;
using openfits::column_format;
using openfits::column_values;
using openfits::compression;
using openfits::compression_options;
using openfits::errc;
using openfits::hdu_kind;
using openfits::image_info;
using openfits::memory_sink;
using openfits::table_info;
using openfits::write_options;
using openfits::writer;

// A model that the standard forbids, which build adds to a writer and saves into a sink.
struct refused_model
{
    std::string name;
    std::function<void(memory_sink&)> build;
    errc code = errc::invalid_value;
    std::uint64_t hdu = 0;
    std::string keyword;
};

// NOLINTNEXTLINE(readability-identifier-naming)
void PrintTo(const refused_model& value, std::ostream* out)
{
    *out << value.name;
}

class write_validation : public testing::TestWithParam<refused_model>
{};

TEST_P(write_validation, refuses_the_model_before_it_writes_anything)
{
    const refused_model& model = GetParam();
    memory_sink sink;
    try {
        model.build(sink);
        FAIL() << "saved";
    } catch (const openfits::validation_error& failure) {
        EXPECT_EQ(failure.code(), model.code) << failure.what();
        EXPECT_EQ(failure.context().hdu, model.hdu) << failure.what();
        EXPECT_EQ(failure.context().keyword, model.keyword) << failure.what();
    }
    EXPECT_TRUE(sink.data().empty());
}

// An image of three bytes with a header that edit makes, saved with options.
template <typename Edit> std::function<void(memory_sink&)> image_with(Edit edit, write_options options = {})
{
    return [edit = std::move(edit), options = std::move(options)](memory_sink& sink) {
        const std::vector<std::uint8_t> values{1, 2, 3};
        image_info info;
        info.geometry.axes = {3};
        edit(info.header);
        writer output;
        (void)output.add_image(info, std::span<const std::uint8_t>(values));
        output.save(sink, options);
    };
}

// An image of the model of info for values of T, compressed by options when given.
template <typename T>
std::function<void(memory_sink&)> image_of(image_info info, std::vector<T> values,
                                           std::optional<compression_options> compressed = std::nullopt)
{
    return [info = std::move(info), values = std::move(values), compressed = std::move(compressed)](memory_sink& sink) {
        writer output;
        (void)output.add_image(info, std::span<const T>(values), {.compression = compressed});
        output.save(sink);
    };
}

image_info axes(std::vector<std::uint64_t> lengths, std::optional<bitpix> stored = std::nullopt)
{
    image_info info;
    info.geometry.axes = std::move(lengths);
    info.bitpix = stored;
    return info;
}

// A reserved image of the model of info.
std::function<void(memory_sink&)> reserved(image_info info)
{
    return [info = std::move(info)](memory_sink& sink) {
        writer output;
        (void)output.add_reserved_image(info);
        output.save(sink);
    };
}

// A binary table of one column of one row of 32-bit integers, with the model of column.
std::function<void(memory_sink&)> column_of(openfits::column_info column,
                                            std::optional<compression_options> compressed = std::nullopt)
{
    return [column = std::move(column), compressed = std::move(compressed)](memory_sink& sink) {
        static const std::vector<std::int32_t> values{1, 2, 3, 4};
        static const std::vector<std::string> strings{"abc"};
        static const std::vector<std::uint8_t> logicals{1};
        const bool characters = column.format && column.format->type == openfits::column_type::character;
        const bool logical = column.format && column.format->type == openfits::column_type::logical;
        const std::uint64_t rows = characters || logical ? 1 : 4 / (column.format ? column.format->repeat : 1);
        writer output;
        const column_values given = characters ? column_values(std::span(strings))
                                    : logical  ? column_values::logicals(std::span(logicals))
                                               : column_values(std::span(values));
        (void)output.add_table(table_info{.kind = hdu_kind::bintable, .rows = rows, .columns = {column}},
                               std::vector<column_values>{given}, {.compression = compressed});
        output.save(sink);
    };
}

std::vector<refused_model> refused_models()
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    const std::string hierarch_name = "ESO " + std::string(56, 'N');
    std::vector<openfits::column_info> many(1000);
    for (openfits::column_info& column : many) {
        column.format = column_format::parse_binary("1J");
    }
    std::vector<refused_model> table;
    const auto add = [&table](std::string name, std::function<void(memory_sink&)> build, errc code, std::uint64_t hdu,
                              std::string keyword) {
        refused_model model;
        model.name = std::move(name);
        model.build = std::move(build);
        model.code = code;
        model.hdu = hdu;
        model.keyword = std::move(keyword);
        table.push_back(std::move(model));
    };
    // Keyword names, value forms, strings and comments (standard §4.1, §4.2).
    add("a_keyword_name_with_a_space", image_with([](auto& h) { h.set("BAD KEY", 1); }), errc::invalid_keyword, 0,
        "BAD KEY");
    add("a_keyword_name_too_long", image_with([](auto& h) { h.set("LONGKEYWORD", 1); }), errc::invalid_keyword, 0,
        "LONGKEYWORD");
    add("a_real_that_is_not_a_number", image_with([nan](auto& h) { h.set("RATIO", nan); }), errc::invalid_real, 0,
        "RATIO");
    add("a_complex_value_that_is_infinite",
        image_with([infinity](auto& h) { h.set("Z", std::complex<double>(1.0, infinity)); }), errc::invalid_real, 0,
        "Z");
    add("a_value_text_of_no_form", image_with([](auto& h) { h.append(card::with_value_text("GAIN", "1,5")); }),
        errc::invalid_value, 0, "GAIN");
    add("a_value_text_with_a_lowercase_exponent",
        image_with([](auto& h) { h.append(card::with_value_text("GAIN", "1.5e3")); }), errc::invalid_real, 0, "GAIN");
    add("a_value_that_no_record_holds",
        image_with([name = hierarch_name](auto& h) { h.append(card::with_value_text(name, "1.23456789")); },
                   {.hierarch = true}),
        errc::value_too_long, 0, hierarch_name);
    add("a_string_outside_printable_ascii", image_with([](auto& h) { h.set("OBSERVER", "Jos\xE9"); }),
        errc::invalid_character, 0, "OBSERVER");
    add("a_comment_outside_printable_ascii", image_with([](auto& h) {
            h.set("TEMP", -10.5,
                  "[\xB0"
                  "C]");
        }),
        errc::invalid_character, 0, "TEMP");
    add("a_comment_too_long", image_with([](auto& h) { h.set("EXPTIME", 1.0, std::string(80, 'c')); }),
        errc::comment_too_long, 0, "EXPTIME");
    add("a_commentary_text_too_long",
        image_with([](auto& h) { h.append(card::commentary("HISTORY", std::string(73, 'h'))); }),
        errc::comment_too_long, 0, "HISTORY");
    add("a_commentary_record_that_reads_as_a_value",
        image_with([](auto& h) { h.append(card::commentary("TITLE", "= 5")); }), errc::invalid_value_indicator, 0,
        "TITLE");
    add("a_long_string_of_a_reserved_keyword", image_with([](auto& h) { h.set("OBJECT", std::string(150, 'M')); }),
        errc::continued_reserved_keyword, 0, "OBJECT");
    add("a_record_read_with_bytes_outside_printable_ascii",
        image_with([](auto& h) { h.append(card::parse("COMMENT caf\xE9")); }), errc::invalid_character, 0, "COMMENT");
    // Structural keywords, which the writer makes from the model.
    add("a_structural_keyword_in_the_header_of_an_image", image_with([](auto& h) { h.set("BZERO", 1.0); }),
        errc::structural_keyword, 0, "BZERO");
    add(
        "a_structural_keyword_in_the_header_of_a_table",
        [](memory_sink& sink) {
            const std::vector<std::int32_t> values{1};
            table_info info{.kind = hdu_kind::bintable, .rows = 1, .columns = {{.name = "A"}}};
            info.header.set("TFORM1", "1J");
            writer output;
            (void)output.add_table(info, std::vector<column_values>{std::span(values)});
            output.save(sink);
        },
        errc::structural_keyword, 1, "TFORM1");
    // The axes, the size of the data and the scaling (standard §3.3.2, §4.4.1, §4.4.2.5).
    add("more_than_999_axes", image_of<std::uint8_t>(axes(std::vector<std::uint64_t>(1000, 1)), {1}),
        errc::invalid_structure, 0, "NAXIS");
    add("axes_of_more_than_2_to_the_64_values",
        reserved(axes({std::uint64_t{1} << 40U, std::uint64_t{1} << 40U}, bitpix::uint8)), errc::invalid_structure, 0,
        "NAXIS");
    add("data_of_more_than_2_to_the_64_bytes", reserved(axes({std::uint64_t{1} << 62U}, bitpix::float64)),
        errc::invalid_structure, 0, "NAXIS");
    add(
        "a_bscale_of_0",
        [](memory_sink& sink) {
            image_info info = axes({1}, bitpix::int16);
            info.scaling.scale = 0.0;
            reserved(info)(sink);
        },
        errc::reserved_keyword, 0, "BSCALE");
    add(
        "a_bzero_that_is_not_finite",
        [nan](memory_sink& sink) {
            image_info info = axes({1}, bitpix::int16);
            info.scaling.zero = nan;
            reserved(info)(sink);
        },
        errc::reserved_keyword, 0, "BZERO");
    add(
        "blank_with_floating_point_data",
        [](memory_sink& sink) {
            image_info info = axes({1}, bitpix::float32);
            info.blank = 0;
            reserved(info)(sink);
        },
        errc::reserved_keyword, 0, "BLANK");
    add(
        "blank_beyond_the_stored_type",
        [](memory_sink& sink) {
            image_info info = axes({1}, bitpix::int16);
            info.blank = 40000;
            reserved(info)(sink);
        },
        errc::reserved_keyword, 0, "BLANK");
    // The keywords of columns (standard §7.3.2).
    add("tnull_of_a_floating_point_column", column_of({.format = column_format::parse_binary("1E"), .null = 0}),
        errc::reserved_keyword, 1, "TNULL1");
    add("tnull_beyond_the_column", column_of({.format = column_format::parse_binary("1I"), .null = 40000}),
        errc::reserved_keyword, 1, "TNULL1");
    add("tscal_of_characters", column_of({.format = column_format::parse_binary("8A"), .scaling = {.scale = 2.0}}),
        errc::reserved_keyword, 1, "TSCAL1");
    add("tscal_of_0", column_of({.format = column_format::parse_binary("1J"), .scaling = {.scale = 0.0}}),
        errc::reserved_keyword, 1, "TSCAL1");
    add("tdim_larger_than_its_field", column_of({.format = column_format::parse_binary("4J"), .dimensions = {3, 2}}),
        errc::invalid_dimensions, 1, "TDIM1");
    add("a_format_of_the_other_kind_of_table", column_of({.format = column_format::parse_ascii("I8")}),
        errc::invalid_column_format, 1, "TFORM1");
    add(
        "more_than_999_columns",
        [many](memory_sink& sink) {
            static const std::vector<std::int32_t> one{1};
            writer output;
            (void)output.add_table(table_info{.kind = hdu_kind::bintable, .rows = 1, .columns = many},
                                   std::vector<column_values>(1000, column_values(std::span(one))));
            output.save(sink);
        },
        errc::invalid_structure, 1, "TFIELDS");
    // Compression (standard §10), which the data must allow.
    add("rice_of_64_bit_integers_unless_asked_for",
        image_of<std::int64_t>(axes({2}), {1, 2}, compression_options{.type = compression::rice}),
        errc::invalid_compression, 1, "ZCMPTYPE");
    add("rice_of_floating_point_values_without_a_quantization",
        image_of<float>(axes({2}), {1.0F, 2.0F}, compression_options{.type = compression::rice}),
        errc::invalid_compression, 1, "ZCMPTYPE");
    add("plio_of_values_beyond_24_bits",
        image_of<std::int32_t>(axes({2}), {1, 1 << 24}, compression_options{.type = compression::plio}),
        errc::invalid_compression, 1, "ZCMPTYPE");
    // The values of a tile of 1000 are checked whole, from each stored type, with and without the offset of BZERO.
    {
        const auto one_beyond = []<typename T>(std::type_identity<T>, std::size_t position, T value) {
            std::vector<T> values(1000, T{5});
            values[position] = value;
            return values;
        };
        const compression_options plio{.type = compression::plio};
        add("plio_of_a_negative_16_bit_value_at_the_end_of_a_tile",
            image_of<std::int16_t>(axes({1000}), one_beyond(std::type_identity<std::int16_t>{}, 999, std::int16_t{-1}),
                                   plio),
            errc::invalid_compression, 1, "ZCMPTYPE");
        add("plio_of_a_32_bit_value_beyond_24_bits_after_512_values",
            image_of<std::int32_t>(axes({1000}),
                                   one_beyond(std::type_identity<std::int32_t>{}, 700, std::int32_t{1} << 24), plio),
            errc::invalid_compression, 1, "ZCMPTYPE");
        add("plio_of_a_32_bit_value_below_the_offset_of_bzero",
            image_of<std::int32_t>(
                [] {
                    image_info info = axes({1000}, bitpix::int32);
                    info.scaling.zero = 32768.0;
                    return info;
                }(),
                one_beyond(std::type_identity<std::int32_t>{}, 512, std::int32_t{-1}), plio),
            errc::invalid_compression, 1, "ZCMPTYPE");
        add("plio_of_a_64_bit_value_beyond_32_bits",
            image_of<std::int64_t>(axes({1000}),
                                   one_beyond(std::type_identity<std::int64_t>{}, 511, std::int64_t{1} << 40), plio),
            errc::invalid_compression, 1, "ZCMPTYPE");
        add("plio_of_a_64_bit_value_below_the_offset_of_bzero",
            image_of<std::int64_t>(
                [] {
                    image_info info = axes({1000}, bitpix::int64);
                    info.scaling.zero = 32768.0;
                    return info;
                }(),
                one_beyond(std::type_identity<std::int64_t>{}, 0, std::int64_t{-1}), plio),
            errc::invalid_compression, 1, "ZCMPTYPE");
    }
    add("hcompress_of_an_image_of_one_axis",
        image_of<std::int16_t>(axes({8}), std::vector<std::int16_t>(8, 1),
                               compression_options{.type = compression::hcompress}),
        errc::invalid_compression, 1, "ZCMPTYPE");
    add("subtractive_dither_2_with_hcompress",
        image_of<float>(axes({8, 8}), std::vector<float>(64, 1.0F),
                        compression_options{.type = compression::hcompress,
                                            .quantize_level = 4.0,
                                            .quantize_method = openfits::quantization::subtractive_dither_2}),
        errc::invalid_compression, 1, "ZCMPTYPE");
    add("an_algorithm_that_a_column_cannot_take",
        column_of({.format = column_format::parse_binary("1L")}, compression_options{.columns = {compression::rice}}),
        errc::invalid_compression, 1, "ZCTYP1");
    return table;
}

INSTANTIATE_TEST_SUITE_P(every_rule, write_validation, testing::ValuesIn(refused_models()),
                         [](const testing::TestParamInfo<refused_model>& instance) { return instance.param.name; });

// Standard §4.1.2.3: bytes outside printable ASCII are refused, unless the save replaces them or keeps them.
TEST(write_validation_options, bytes_outside_printable_ascii_are_replaced_or_kept_on_request)
{
    const std::vector<std::uint8_t> values{1};
    image_info info = axes({1});
    info.header.set("OBSERVER", "Jos\xE9");
    info.header.append(card::parse("COMMENT 20 \xB0"
                                   "C"));
    writer output;
    (void)output.add_image(info, std::span<const std::uint8_t>(values));
    memory_sink replaced;
    output.save(replaced, {.non_ascii = openfits::non_ascii::replace});
    const std::string text(reinterpret_cast<const char*>(replaced.data().data()), 2880);
    EXPECT_NE(text.find("OBSERVER= 'Jos?    '"), std::string::npos);
    EXPECT_NE(text.find("COMMENT 20 ?C"), std::string::npos);
    memory_sink kept;
    output.save(kept, {.non_ascii = openfits::non_ascii::keep});
    const std::string raw(reinterpret_cast<const char*>(kept.data().data()), 2880);
    EXPECT_NE(raw.find("OBSERVER= 'Jos\xE9    '"), std::string::npos);
    EXPECT_NE(raw.find("COMMENT 20 \xB0"
                       "C"),
              std::string::npos);
}

// The cards of value texts take the forms that the options of the save allow: HIERARCH names, comments cut.
TEST(write_validation_options, value_texts_take_the_forms_that_the_options_allow)
{
    const std::vector<std::uint8_t> values{1};
    image_info info = axes({1});
    info.header.append(card::with_value_text("ESO DET DIT", "1.50E+00", "[s] integration time"));
    info.header.append(card::with_value_text("NOTE", "'ok'", std::string(90, 'c')));
    writer output;
    (void)output.add_image(info, std::span<const std::uint8_t>(values));
    memory_sink refused;
    EXPECT_THROW(output.save(refused), openfits::validation_error);
    EXPECT_TRUE(refused.data().empty());
    memory_sink sink;
    output.save(sink, {.hierarch = true, .truncate_comments = true});
    const std::string text(reinterpret_cast<const char*>(sink.data().data()), 2880);
    EXPECT_NE(text.find("HIERARCH ESO DET DIT = 1.50E+00 / [s] integration time"), std::string::npos);
    // A comment too long for byte 34 comes closer to its value, then is cut.
    EXPECT_NE(text.find("NOTE    = 'ok' / " + std::string(63, 'c') + "END"), std::string::npos);
}

} // namespace
