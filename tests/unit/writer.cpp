// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/checksum.h>
#include <openfits/error.h>
#include <openfits/header.h>
#include <openfits/image.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/table.h>
#include <openfits/types.h>
#include <openfits/writer.h>

#include "data/conversion.h"
#include "data/image_write.h"
#include "support/bytes.h"
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
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

using openfits::bitpix;
using openfits::cancelled_error;
using openfits::errc;
using openfits::header;
using openfits::image_info;
using openfits::invalid_data_error;
using openfits::io_error;
using openfits::numeric_type;
using openfits::reader;
using openfits::usage_error;
using openfits::validation_error;
using openfits::write_options;
using openfits::writer;
using openfits::test::sink_kind;
using openfits::test::temp_directory;
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

// The keywords of a header, in their order.
std::vector<std::string> keywords_of(const header& cards)
{
    std::vector<std::string> names;
    for (const openfits::card& value : cards) {
        names.emplace_back(value.keyword());
    }
    return names;
}

// A record in the fixed format: the name in bytes 1 to 8, "= ", a value right-justified to byte 30, or a string from
// byte 11, then " / " and the comment.
std::string fixed(std::string_view name, std::string_view value, std::string_view comment)
{
    std::string text(name);
    text.resize(8, ' ');
    text += "= ";
    std::string field(value);
    if (!value.starts_with('\'')) {
        field.insert(0, 20 - std::min<std::size_t>(20, value.size()), ' ');
    } else {
        field.resize(std::max<std::size_t>(20, field.size()), ' ');
    }
    text += field;
    text += " / ";
    text += comment;
    text.resize(80, ' ');
    return text;
}

// Half the largest value of T, without a cast where the division keeps the type, which GCC reports as useless.
template <typename T> T half_of_max()
{
    const auto half = std::numeric_limits<T>::max() / 2;
    if constexpr (std::same_as<std::remove_const_t<decltype(half)>, T>) {
        return half;
    } else {
        return static_cast<T>(half);
    }
}

// Values of every type, its extremes among them.
template <typename T> std::vector<T> values_of()
{
    if constexpr (std::floating_point<T>) {
        return {std::numeric_limits<T>::lowest(),
                T{-1.5},
                T{-0.0},
                T{0.0},
                T{1.0},
                std::numeric_limits<T>::max(),
                std::numeric_limits<T>::infinity(),
                std::numeric_limits<T>::denorm_min()};
    } else {
        return {std::numeric_limits<T>::min(), T{0}, T{1}, half_of_max<T>(), std::numeric_limits<T>::max(), T{3}};
    }
}

template <typename T> void check_round_trip(bitpix expected_bitpix, double expected_zero)
{
    const std::vector<T> values = values_of<T>();
    writer output;
    image_info info = image_of({values.size() / 2, 2});
    info.header.set("OBJECT", "the values of a type");
    EXPECT_EQ(output.add_image(info, std::span<const T>(values)), 0U);
    const std::vector<std::byte> file = written(output);
    ASSERT_EQ(file.size() % 2880, 0U);

    const reader input = open(file);
    const image_info image = input.image(0);
    EXPECT_EQ(image.bitpix, expected_bitpix);
    EXPECT_EQ(image.scaling.zero, expected_zero);
    EXPECT_EQ(image.value_type(), openfits::numeric_type_of<T>());
    const std::vector<T> read = input.read_pixels<T>(0);
    ASSERT_EQ(read.size(), values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        if constexpr (std::floating_point<T>) {
            EXPECT_EQ(std::signbit(read[i]), std::signbit(values[i])) << i;
        }
        EXPECT_EQ(read[i], values[i]) << i;
    }
}

// Each type of the application in its own type, with the conventions of standard §5.2.5 for those stored with an
// offset.
TEST(writer, writes_every_type_in_its_own_type)
{
    check_round_trip<std::int8_t>(bitpix::uint8, -128.0);
    check_round_trip<std::uint8_t>(bitpix::uint8, 0.0);
    check_round_trip<std::int16_t>(bitpix::int16, 0.0);
    check_round_trip<std::uint16_t>(bitpix::int16, 32768.0);
    check_round_trip<std::int32_t>(bitpix::int32, 0.0);
    check_round_trip<std::uint32_t>(bitpix::int32, 2147483648.0);
    check_round_trip<std::int64_t>(bitpix::int64, 0.0);
    check_round_trip<std::uint64_t>(bitpix::int64, 9223372036854775808.0);
    check_round_trip<float>(bitpix::float32, 0.0);
    check_round_trip<double>(bitpix::float64, 0.0);
}

TEST(writer, a_header_of_default_options_holds_the_mandatory_keywords_extend_and_the_cards_given)
{
    const std::vector<std::uint8_t> bytes{1, 2, 3};
    writer output;
    image_info info = image_of({3});
    info.header.set("OBSERVER", "Nobody");
    (void)output.add_image(info, std::span<const std::uint8_t>(bytes));
    const std::vector<std::uint16_t> words{1, 2};
    (void)output.add_image(image_of({2}), std::span<const std::uint16_t>(words));
    const reader input = open(written(output));
    EXPECT_EQ(keywords_of(input.hdu(0).header()),
              (std::vector<std::string>{"SIMPLE", "BITPIX", "NAXIS", "NAXIS1", "EXTEND", "OBSERVER"}));
    EXPECT_EQ(keywords_of(input.hdu(1).header()),
              (std::vector<std::string>{"XTENSION", "BITPIX", "NAXIS", "NAXIS1", "PCOUNT", "GCOUNT", "BZERO"}));
    EXPECT_TRUE(input.hdu(0).diagnostics().empty());
    EXPECT_TRUE(input.hdu(1).diagnostics().empty());
    EXPECT_TRUE(input.diagnostics().empty());
}

// The same model gives the same bytes on every host: these.
TEST(writer, writes_a_model_byte_for_byte)
{
    const std::vector<std::uint16_t> values{0, 1, 65534, 65535};
    writer output;
    image_info info = image_of({2, 2});
    info.header.set("OBJECT", "M 31", "the target");
    (void)output.add_image(info, std::span<const std::uint16_t>(values));
    const std::vector<std::byte> file = written(output);

    std::string expected =
        fixed("SIMPLE", "T", "conforms to the FITS standard") + fixed("BITPIX", "16", "16-bit signed integers") +
        fixed("NAXIS", "2", "number of axes") + fixed("NAXIS1", "2", "length of axis 1") +
        fixed("NAXIS2", "2", "length of axis 2") + fixed("EXTEND", "T", "extensions may follow") +
        fixed("BZERO", "32768", "unsigned 16-bit integers") + fixed("OBJECT", "'M 31    '", "the target");
    expected += std::string("END").append(77, ' ');
    expected.resize(2880, ' ');
    for (const unsigned byte : {0x80U, 0x00U, 0x80U, 0x01U, 0x7FU, 0xFEU, 0x7FU, 0xFFU}) {
        expected += static_cast<char>(byte);
    }
    expected.resize(std::size_t{2} * 2880, '\0');
    EXPECT_EQ(openfits::test::text(file), expected);
}

TEST(writer, converts_values_into_an_explicit_bitpix_with_its_scaling_and_blank)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::vector<float> values{100.0F, 100.004F, 100.006F, -227.67F, nan, 427.67F};
    writer output;
    image_info info = image_of({6});
    info.bitpix = bitpix::int16;
    info.scaling = {.scale = 0.01, .zero = 100.0};
    info.blank = -32768;
    (void)output.add_image(info, std::span<const float>(values));
    const reader input = open(written(output));
    const image_info image = input.image(0);
    EXPECT_EQ(image.bitpix, bitpix::int16);
    EXPECT_EQ(image.scaling, (openfits::scaling{.scale = 0.01, .zero = 100.0}));
    EXPECT_EQ(image.blank, -32768);
    // (value - BZERO) / BSCALE, rounded half away from zero.
    EXPECT_EQ(input.read_pixels<std::int16_t>(0, {.scaling = openfits::scaling_policy::raw}),
              (std::vector<std::int16_t>{0, 0, 1, -32767, -32768, 32767}));
    const std::vector<double> physical = input.read_pixels<double>(0);
    EXPECT_NEAR(physical[2], 100.01, 1e-12);
    EXPECT_TRUE(std::isnan(physical[4]));
}

TEST(writer, writes_stored_values_as_they_are_under_the_scaling_of_the_model)
{
    // The codes of a frame written back with the BSCALE and BZERO of its file, which a read applies.
    const std::vector<std::int16_t> codes{-32768, -1, 0, 1, 32767};
    image_info info = image_of({5});
    info.bitpix = bitpix::int16;
    info.scaling = {.scale = 0.01, .zero = 100.0};
    writer output;
    (void)output.add_image(info, std::span<const std::int16_t>(codes), {.scaling = openfits::scaling_policy::raw});
    const reader input = open(written(output));
    EXPECT_EQ(input.image(0).scaling, info.scaling);
    EXPECT_EQ(input.read_pixels<std::int16_t>(0, {.scaling = openfits::scaling_policy::raw}), codes);
    EXPECT_EQ(input.read_pixels<double>(0)[1], 100.0 - 0.01);

    // 64-bit codes, which no conversion through double keeps, under the offset of the unsigned convention.
    const std::vector<std::int64_t> wide{std::numeric_limits<std::int64_t>::min(), -1, 0,
                                         std::numeric_limits<std::int64_t>::max() - 1};
    image_info unsigned_info = image_of({4});
    unsigned_info.bitpix = bitpix::int64;
    unsigned_info.scaling = {.zero = 9223372036854775808.0};
    writer unsigned_output;
    (void)unsigned_output.add_image(unsigned_info, std::span<const std::int64_t>(wide),
                                    {.scaling = openfits::scaling_policy::raw});
    const reader unsigned_input = open(written(unsigned_output));
    EXPECT_EQ(unsigned_input.read_pixels<std::int64_t>(0, {.scaling = openfits::scaling_policy::raw}), wide);
    EXPECT_EQ(unsigned_input.read_pixels<std::uint64_t>(0),
              (std::vector<std::uint64_t>{0, (std::uint64_t{1} << 63U) - 1, std::uint64_t{1} << 63U,
                                          std::numeric_limits<std::uint64_t>::max() - 1}));

    // The stored values convert into the type of BITPIX alone, with its range.
    const std::vector<std::int32_t> beyond{70000};
    image_info one = info;
    one.geometry.axes = {1};
    writer refusing;
    (void)refusing.add_image(one, std::span<const std::int32_t>(beyond), {.scaling = openfits::scaling_policy::raw});
    EXPECT_TRUE(throws<invalid_data_error>(errc::value_out_of_range, [&] { (void)written(refusing); }));
    writer without_bitpix;
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] {
        (void)without_bitpix.add_image(image_of({5}), std::span<const std::int16_t>(codes),
                                       {.scaling = openfits::scaling_policy::raw});
    }));
}

TEST(writer, converts_the_values_of_an_override_by_its_scaling_under_the_scaling_of_the_model)
{
    // Codes of std::uint16_t, whose offset of standard §5.2.5 the override takes off, under a scaling of the caller's
    // that the header gives: the stored value of each is the code with its sign bit flipped.
    const std::vector<std::uint16_t> codes{0, 1, 32767, 32768, 65535};
    image_info info = image_of({5});
    info.bitpix = bitpix::int16;
    info.scaling = {.scale = 2.0, .zero = 65536.0};
    const openfits::image_write_options offset{.scaling = openfits::scaling_policy::override,
                                               .override_scaling = {.zero = 32768.0}};
    writer output;
    (void)output.add_image(info, std::span<const std::uint16_t>(codes), offset);
    const reader input = open(written(output));
    EXPECT_EQ(input.image(0).scaling, info.scaling);
    EXPECT_EQ(input.read_pixels<std::int16_t>(0, {.scaling = openfits::scaling_policy::raw}),
              (std::vector<std::int16_t>{-32768, -32767, -1, 0, 32767}));
    EXPECT_EQ(input.read_pixels<double>(0), (std::vector<double>{0.0, 2.0, 65534.0, 65536.0, 131070.0}));
    EXPECT_EQ(input.read_pixels<std::uint16_t>(
                  0, {.scaling = openfits::scaling_policy::override, .override_scaling = {.zero = 32768.0}}),
              codes);

    // Any scaling: (value - 1) / 0.5, rounded half away from zero into integers, beside no scaling in the header.
    const std::vector<float> values{1.0F, 1.5F, 3.2F};
    image_info plain = image_of({3});
    plain.bitpix = bitpix::int32;
    writer floats;
    (void)floats.add_image(
        plain, std::span<const float>(values),
        {.scaling = openfits::scaling_policy::override, .override_scaling = {.scale = 0.5, .zero = 1.0}});
    const reader floats_input = open(written(floats));
    EXPECT_EQ(floats_input.image(0).scaling, openfits::scaling{});
    EXPECT_EQ(floats_input.read_pixels<std::int32_t>(0), (std::vector<std::int32_t>{0, 1, 4}));

    // Refused without BITPIX, and with a scale of 0 or a value that is not finite.
    writer refusing;
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] {
        (void)refusing.add_image(image_of({5}), std::span<const std::uint16_t>(codes), offset);
    }));
    const double infinity = std::numeric_limits<double>::infinity();
    for (const openfits::scaling& wrong : {openfits::scaling{.scale = 0.0}, openfits::scaling{.scale = infinity},
                                           openfits::scaling{.zero = std::numeric_limits<double>::quiet_NaN()}}) {
        EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] {
            (void)refusing.add_image(info, std::span<const std::uint16_t>(codes),
                                     {.scaling = openfits::scaling_policy::override, .override_scaling = wrong});
        }));
    }
    EXPECT_EQ(refusing.hdu_count(), 0U);
}

TEST(writer, a_value_beyond_the_stored_type_is_refused_or_clamped)
{
    const std::vector<std::int32_t> values{-1, 70000, 5};
    image_info info = image_of({3});
    info.bitpix = bitpix::int16;
    writer refusing;
    (void)refusing.add_image(info, std::span<const std::int32_t>(values));
    EXPECT_TRUE(throws<invalid_data_error>(errc::value_out_of_range, [&] { (void)written(refusing); }));

    writer clamping;
    (void)clamping.add_image(info, std::span<const std::int32_t>(values),
                             {.overflow = openfits::overflow_policy::clamp});
    EXPECT_EQ(open(written(clamping)).read_pixels<std::int32_t>(0), (std::vector<std::int32_t>{-1, 32767, 5}));
}

TEST(writer, a_nan_in_integer_data_needs_blank)
{
    const std::vector<double> values{1.0, std::numeric_limits<double>::quiet_NaN()};
    image_info info = image_of({2});
    info.bitpix = bitpix::int32;
    writer output;
    (void)output.add_image(info, std::span<const double>(values));
    EXPECT_TRUE(throws<invalid_data_error>(errc::undefined_value, [&] { (void)written(output); }));
}

TEST(writer, never_changes_the_values_it_writes)
{
    const std::vector<std::uint32_t> original{1, 2, 0x8000'0000U, 0xFFFF'FFFFU};
    std::vector<std::uint32_t> values = original;
    writer output;
    (void)output.add_image(image_of({4}), std::span<std::uint32_t>(values));
    (void)written(output);
    (void)written(output, {}, sink_kind::append_only);
    EXPECT_EQ(values, original);
}

TEST(writer, a_primary_of_naxis_0_and_image_extensions)
{
    header primary;
    primary.set("ORIGIN", "the tests");
    const std::vector<float> first{1.0F, 2.0F};
    const std::vector<std::int16_t> second{-1, -2, -3};
    writer output;
    EXPECT_EQ(output.add_header(primary), 0U);
    image_info science = image_of({2});
    science.header.set("EXTNAME", "SCI");
    EXPECT_EQ(output.add_image(science, std::span<const float>(first)), 1U);
    image_info mask = image_of({3});
    mask.header.set("EXTNAME", "MASK");
    EXPECT_EQ(output.add_image(mask, std::span<const std::int16_t>(second)), 2U);
    EXPECT_EQ(output.hdu_count(), 3U);

    const reader input = open(written(output, {}, sink_kind::append_only));
    ASSERT_EQ(input.hdu_count(), 3U);
    EXPECT_EQ(input.hdu(0).kind(), openfits::hdu_kind::primary);
    EXPECT_EQ(input.hdu(0).data_size(), 0U);
    EXPECT_EQ(input.hdu(0).header().value<std::string>("ORIGIN"), "the tests");
    EXPECT_EQ(input.hdu(0).header().value<bool>("EXTEND"), true);
    EXPECT_EQ(input.hdu(1).kind(), openfits::hdu_kind::image);
    EXPECT_EQ(input.hdu("SCI").index(), 1U);
    EXPECT_EQ(input.read_pixels<float>(1), first);
    EXPECT_EQ(input.read_pixels<std::int16_t>(input.hdu("MASK").index()), second);
}

// Standard §4.4.2.4: blank cards before END, which later changes take.
TEST(writer, reserves_blank_cards_before_end)
{
    const std::vector<std::uint8_t> values{7};
    writer output;
    (void)output.add_image(image_of({1}), std::span<const std::uint8_t>(values), {.reserved_cards = 40});
    (void)output.add_header({}, 3);
    const reader input = open(written(output));
    EXPECT_EQ(input.hdu(0).header().free_cards(), 40U);
    EXPECT_EQ(input.hdu(0).header_size(), 2U * 2880);
    EXPECT_EQ(input.hdu(1).header().free_cards(), 3U);
    EXPECT_EQ(input.read_pixels<std::uint8_t>(0), values);
}

TEST(writer, refuses_structural_keywords_in_the_header_before_it_writes_anything)
{
    const std::vector<std::uint8_t> values{1};
    writer output;
    (void)output.add_image(image_of({1}), std::span<const std::uint8_t>(values));
    image_info info = image_of({1});
    info.header.set("BZERO", 1.0);
    (void)output.add_image(info, std::span<const std::uint8_t>(values));
    openfits::memory_sink sink;
    try {
        output.save(sink);
        FAIL() << "saved";
    } catch (const validation_error& failure) {
        EXPECT_EQ(failure.code(), errc::structural_keyword);
        EXPECT_EQ(failure.context().hdu, 1U);
        EXPECT_EQ(failure.context().keyword, "BZERO");
    }
    EXPECT_TRUE(sink.data().empty());
}

TEST(writer, writes_the_conventions_outside_the_standard_only_when_asked)
{
    const std::vector<std::uint8_t> values{1};
    writer output;
    image_info info = image_of({1});
    info.header.set("ESO DET DIT", 1.5);
    (void)output.add_image(info, std::span<const std::uint8_t>(values));
    EXPECT_TRUE(throws<validation_error>(errc::invalid_keyword, [&] { (void)written(output); }));
    const reader input = open(written(output, {.hierarch = true}));
    EXPECT_EQ(input.hdu(0).header().value<double>("ESO DET DIT"), 1.5);

    // The long string of a reserved keyword, which standard §4.2.1.2 forbids and CFITSIO writes.
    const std::string object(150, 'M');
    writer long_object;
    image_info object_info = image_of({1});
    object_info.header.set("OBJECT", object);
    (void)long_object.add_image(object_info, std::span<const std::uint8_t>(values));
    EXPECT_TRUE(throws<validation_error>(errc::continued_reserved_keyword, [&] { (void)written(long_object); }));
    // A file that departs from standard §4.2.1.2 on purpose, which written() would refuse.
    openfits::memory_sink continued;
    long_object.save(continued, {.continue_reserved_keywords = true});
    const reader object_input = open(continued.release());
    EXPECT_EQ(object_input.hdu(0).header().value<std::string>("OBJECT"), object);
    EXPECT_EQ(object_input.hdu(0).header().count("CONTINUE"), 2U);
}

TEST(writer, refuses_models_that_the_standard_forbids)
{
    const std::vector<std::uint8_t> three{1, 2, 3};
    writer output;
    EXPECT_TRUE(throws<usage_error>(
        errc::invalid_argument, [&] { (void)output.add_image(image_of({2}), std::span<const std::uint8_t>(three)); }));
    image_info scaled = image_of({3});
    scaled.scaling.scale = 2.0;
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument,
                                    [&] { (void)output.add_image(scaled, std::span<const std::uint8_t>(three)); }));
    scaled.bitpix = bitpix::int16;
    scaled.scaling.scale = 0.0;
    EXPECT_TRUE(throws<validation_error>(
        errc::reserved_keyword, [&] { (void)output.add_image(scaled, std::span<const std::uint8_t>(three)); }));
    image_info floating = image_of({3});
    floating.bitpix = bitpix::float32;
    floating.blank = 0;
    EXPECT_TRUE(throws<validation_error>(
        errc::reserved_keyword, [&] { (void)output.add_image(floating, std::span<const std::uint8_t>(three)); }));
    image_info beyond = image_of({3});
    beyond.bitpix = bitpix::int16;
    beyond.blank = 40000;
    EXPECT_TRUE(throws<validation_error>(
        errc::reserved_keyword, [&] { (void)output.add_image(beyond, std::span<const std::uint8_t>(three)); }));
    const image_info many = image_of(std::vector<std::uint64_t>(1000, 1));
    const std::vector<std::uint8_t> one{1};
    EXPECT_TRUE(throws<validation_error>(errc::invalid_structure,
                                         [&] { (void)output.add_image(many, std::span<const std::uint8_t>(one)); }));
    EXPECT_EQ(output.hdu_count(), 0U);
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { (void)written(output); }));
}

TEST(writer, reports_the_bytes_of_the_file_and_can_be_cancelled)
{
    const std::vector<float> values(3000, 1.0F);
    writer output;
    (void)output.add_image(image_of({3000}), std::span<const float>(values));
    std::uint64_t last = 0;
    std::uint64_t whole = 0;
    const std::vector<std::byte> file = written(output, {.progress = [&](std::uint64_t done, std::uint64_t total) {
                                                    EXPECT_GE(done, last);
                                                    last = done;
                                                    whole = total;
                                                    return true;
                                                }});
    EXPECT_EQ(whole, file.size());
    EXPECT_EQ(last, file.size());
    EXPECT_TRUE(throws<cancelled_error>(errc::cancelled, [&] {
        (void)written(output, {.progress = [](std::uint64_t done, std::uint64_t) { return done < 2880; }});
    }));
}

TEST(writer, gives_the_size_of_the_file_that_a_save_writes)
{
    const std::vector<float> values(1000, 1.0F);
    writer output;
    image_info info = image_of({1000});
    info.header.set("NOTE", std::string(100, 'n'));
    (void)output.add_image(info, std::span<const float>(values));
    for (const write_options& options : {write_options{}, write_options{.longstrn = true}}) {
        openfits::memory_sink sink;
        sink.reserve(output.size(options));
        output.save(sink, options);
        EXPECT_EQ(output.size(options), sink.data().size());
    }
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [] { (void)writer().size(); }));
}

TEST(writer, adds_and_reads_values_of_a_type_known_at_run_time)
{
    const std::vector<std::int32_t> values{-5, 0, 70000};
    writer output;
    (void)output.add_image(image_of({3}), numeric_type::int32, std::as_bytes(std::span(values)));
    const reader input = open(written(output));
    EXPECT_EQ(input.image(0).bitpix, bitpix::int32);
    std::vector<double> read(3);
    (void)input.read_pixels(0, numeric_type::float64, std::as_writable_bytes(std::span(read)));
    EXPECT_EQ(read, (std::vector<double>{-5.0, 0.0, 70000.0}));
    // Whole values at the address of their alignment only.
    const std::span<const std::byte> bytes = std::as_bytes(std::span(values));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] {
        (void)writer().add_image(image_of({2}), numeric_type::int32, bytes.subspan(0, 9));
    }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] {
        (void)writer().add_image(image_of({2}), numeric_type::int32, bytes.subspan(1, 8));
    }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] {
        (void)input.read_pixels(0, numeric_type::float64, std::as_writable_bytes(std::span(read)).subspan(4, 16));
    }));
}

TEST(writer, saves_to_a_path_replaced_only_once_the_file_is_complete)
{
    const temp_directory directory;
    const std::string path = directory.file("image.fits");
    const std::vector<std::int16_t> values{1, 2, 3};
    writer output;
    (void)output.add_image(image_of({3}), std::span<const std::int16_t>(values));
    output.save(path);
    EXPECT_EQ(reader(path).read_pixels<std::int16_t>(0), values);

    // A cancelled save leaves the file as it was, and no temporary file.
    const std::string before = openfits::test::read_file(directory.path() / "image.fits");
    EXPECT_TRUE(throws<cancelled_error>(errc::cancelled, [&] {
        output.save(path, {.progress = [](std::uint64_t done, std::uint64_t) { return done == 0; }});
    }));
    EXPECT_EQ(openfits::test::read_file(directory.path() / "image.fits"), before);
    EXPECT_EQ(directory.entries(), 1U);

    // A save that may not replace a file refuses it, and one to a new path creates it.
    EXPECT_TRUE(
        throws<io_error>(errc::file_exists, [&] { output.save(path, {.if_exists = openfits::if_exists::fail}); }));
    output.save(directory.file("new.fits"), {.if_exists = openfits::if_exists::fail});
    EXPECT_EQ(reader(directory.file("new.fits")).read_pixels<std::int16_t>(0), values);
}

TEST(writer, a_cancelled_save_to_a_new_path_leaves_no_file)
{
    const temp_directory directory;
    const std::vector<std::uint8_t> values(10000, 3);
    writer output;
    (void)output.add_image(image_of({10000}), std::span<const std::uint8_t>(values));
    EXPECT_TRUE(throws<cancelled_error>(errc::cancelled, [&] {
        output.save(directory.file("image.fits"),
                    {.progress = [](std::uint64_t done, std::uint64_t) { return done < 5000; }});
    }));
    EXPECT_EQ(directory.entries(), 0U);
}

// The data in staging chunks of any size give the same bytes.
TEST(writer, writes_data_in_chunks_of_any_size)
{
    std::vector<std::uint16_t> values(1000);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = static_cast<std::uint16_t>(i * 37);
    }
    const openfits::detail::write_conversion conversion = openfits::detail::make_write_conversion(
        numeric_type::uint16, bitpix::int16, {.zero = 32768.0}, std::nullopt, openfits::overflow_policy::refuse, 0);
    std::vector<std::vector<std::byte>> outputs;
    for (const std::size_t transfer :
         {std::size_t{1}, std::size_t{7}, std::size_t{2000}, openfits::detail::write_transfer}) {
        openfits::memory_sink sink;
        std::uint64_t bytes = 0;
        openfits::detail::write_image_data(
            sink, conversion, numeric_type::uint16, values.size(), 37,
            openfits::detail::borrowed_values(std::as_bytes(std::span(values)), numeric_type::uint16),
            [&bytes](std::uint64_t written_bytes) { bytes += written_bytes; }, transfer);
        EXPECT_EQ(bytes, 2880U) << transfer;
        outputs.emplace_back(sink.data().begin(), sink.data().end());
    }
    for (const std::vector<std::byte>& output : outputs) {
        EXPECT_EQ(output, outputs.front());
    }
}

// Every kind of unit that the writer writes, with values that it borrows: an image, a header alone, a binary table
// with a heap, and an ASCII table, whose fill is spaces.
struct checked_model
{
    checked_model()
    {
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            pixels[i] = static_cast<std::uint16_t>(i * 977);
        }
        image_info image = image_of({37, 3});
        image.header.set("OBJECT", "the pattern");
        (void)output.add_image(image, std::span<const std::uint16_t>(pixels));
        (void)output.add_header({});
        (void)output.add_table({.kind = openfits::hdu_kind::bintable, .rows = 3, .columns = {{.name = "VALUES"}}},
                               std::vector<openfits::column_values>{
                                   openfits::column_values::arrays(std::span<const std::int16_t>(elements), offsets)});
        (void)output.add_table(
            {.kind = openfits::hdu_kind::table, .rows = 3, .columns = {{.name = "NAME"}, {.name = "COUNT"}}},
            std::vector<openfits::column_values>{std::span(names), std::span(counts)});
    }

    std::vector<std::uint16_t> pixels = std::vector<std::uint16_t>(std::size_t{37} * 3);
    std::vector<std::int16_t> elements{1, 2, 3, 4, 5, 6};
    std::vector<std::uint64_t> offsets{0, 3, 3, 6};
    std::vector<std::string> names{"M 31", "NGC 6992", "IC 434"};
    std::vector<std::int32_t> counts{42, -17, 7};
    writer output;
};

// The bytes of the unit at index of a file, its last block included.
std::span<const std::byte> unit_bytes(const reader& input, const std::vector<std::byte>& file, std::size_t index)
{
    const openfits::hdu_info& unit = input.hdu(index);
    const std::uint64_t end = unit.data_offset() + ((unit.data_size().value_or(0) + 2879) / 2880 * 2880);
    return std::span(file).subspan(unit.header_offset(), end - unit.header_offset());
}

// Standard §4.4.2.7: DATASUM and CHECKSUM in every unit, after the cards of its header, which a reader verifies and
// whose sum, computed here, is negative zero; the same bytes whether the sink rewrites or the data are summed first.
TEST(writer, writes_checksums_that_every_unit_matches)
{
    const checked_model model;
    const std::vector<std::byte> file = written(model.output, {.checksums = true});
    EXPECT_EQ(written(model.output, {.checksums = true}, sink_kind::append_only), file);
    const reader input = open(file);
    ASSERT_EQ(input.hdu_count(), 4U);
    for (std::size_t i = 0; i < 4; ++i) {
        const openfits::checksum_report report = input.verify(i);
        EXPECT_EQ(report.datasum, openfits::checksum_state::valid) << i;
        EXPECT_EQ(report.checksum, openfits::checksum_state::valid) << i;
        EXPECT_EQ(openfits::test::reference_sum(unit_bytes(input, file, i)), 0xFFFFFFFFU) << i;
        const std::vector<std::string> names = keywords_of(input.hdu(i).header());
        ASSERT_GE(names.size(), 2U);
        EXPECT_EQ(names[names.size() - 2], "CHECKSUM") << i;
        EXPECT_EQ(names.back(), "DATASUM") << i;
        EXPECT_TRUE(input.hdu(i).diagnostics().empty()) << i;
    }
    EXPECT_EQ(input.read_pixels<std::uint16_t>(0), model.pixels);
    // The default writes neither.
    const reader plain = open(written(model.output));
    for (const openfits::hdu_info& unit : plain.hdus()) {
        EXPECT_FALSE(unit.header().contains("CHECKSUM"));
        EXPECT_FALSE(unit.header().contains("DATASUM"));
    }
}

// Standard Appendix J.1: CHECKSUM in the fixed format, its quotes in bytes 11 and 28, and DATASUM the decimal sum of
// the data records; the time of the checksums as their comment, when it is given.
TEST(writer, writes_the_records_of_the_checksums)
{
    const checked_model model;
    for (const bool timed : {false, true}) {
        write_options options{.checksums = true};
        if (timed) {
            options.checksum_time = openfits::date_time{
                .year = 2026, .month = 11, .day = 2, .has_time = true, .hour = 10, .minute = 30, .second = 5};
        }
        const std::vector<std::byte> file = written(model.output, options);
        const reader input = open(file);
        for (std::size_t i = 0; i < input.hdu_count(); ++i) {
            const header& cards = input.hdu(i).header();
            const openfits::card& checksum = cards[cards.size() - 2];
            const std::string_view record = checksum.text();
            EXPECT_EQ(record.substr(0, 11), "CHECKSUM= '") << i;
            EXPECT_EQ(record[27], '\'') << i;
            EXPECT_TRUE(std::ranges::all_of(record.substr(11, 16), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
            })) << record;
            const openfits::card& datasum = cards[cards.size() - 1];
            const std::span<const std::byte> unit = unit_bytes(input, file, i);
            const std::uint32_t data_sum = openfits::test::reference_sum(
                std::span(file).subspan(input.hdu(i).data_offset(), unit.size() - input.hdu(i).header_size()));
            EXPECT_EQ(std::stoull(datasum.value().get<std::string>()), data_sum) << i;
            const std::string_view comment = timed ? "2026-11-02T10:30:05" : "";
            EXPECT_EQ(checksum.comment(), comment) << i;
            EXPECT_EQ(datasum.comment(), comment) << i;
        }
    }
}

TEST(writer, refuses_a_time_of_the_checksums_that_is_no_date)
{
    const checked_model model;
    openfits::memory_sink sink;
    EXPECT_TRUE(throws<usage_error>(errc::invalid_date, [&] {
        model.output.save(sink, {.checksums = true, .checksum_time = openfits::date_time{.month = 13}});
    }));
    EXPECT_EQ(sink.position(), 0U);
}

// A sink that cannot rewrite has the data summed before anything is written, which the progress counts: the file and
// the data once more, a pass that can be cancelled before a byte is written.
TEST(writer, sums_the_data_first_for_a_sink_that_cannot_rewrite)
{
    const checked_model model;
    const std::uint64_t size = model.output.size({.checksums = true});
    std::uint64_t data = 0;
    const reader plain = open(written(model.output));
    for (const openfits::hdu_info& unit : plain.hdus()) {
        data += (unit.data_size().value_or(0) + 2879) / 2880 * 2880;
    }
    for (const sink_kind kind : {sink_kind::rewritable, sink_kind::append_only}) {
        std::uint64_t last_done = 0;
        std::uint64_t last_total = 0;
        (void)written(model.output,
                      {.checksums = true,
                       .progress =
                           [&](std::uint64_t done, std::uint64_t total) {
                               last_done = done;
                               last_total = total;
                               return true;
                           }},
                      kind);
        EXPECT_EQ(last_total, kind == sink_kind::rewritable ? size : size + data);
        EXPECT_EQ(last_done, last_total);
    }
    std::uint64_t written_bytes = 0;
    openfits::callback_sink sink([&written_bytes](std::span<const std::byte> bytes) { written_bytes += bytes.size(); });
    EXPECT_TRUE(throws<cancelled_error>(errc::cancelled, [&] {
        model.output.save(sink,
                          {.checksums = true, .progress = [](std::uint64_t done, std::uint64_t) { return done == 0; }});
    }));
    EXPECT_EQ(written_bytes, 0U);
}

// The two cards of the checksums can take a block of their own, which size() counts.
TEST(writer, size_counts_the_checksum_cards)
{
    const std::vector<std::uint8_t> values{1, 2, 3};
    image_info info = image_of({3});
    // Five structural cards, 29 of the caller and END fill 35 records of a block, which the two cards overflow.
    for (int i = 0; i < 29; ++i) {
        info.header.set("KEY" + std::to_string(i), i);
    }
    writer output;
    (void)output.add_image(info, std::span<const std::uint8_t>(values));
    EXPECT_EQ(output.size(), 2U * 2880);
    EXPECT_EQ(output.size({.checksums = true}), 3U * 2880);
    for (const sink_kind kind : {sink_kind::rewritable, sink_kind::append_only}) {
        const std::vector<std::byte> file = written(output, {.checksums = true}, kind);
        EXPECT_EQ(file.size(), 3U * 2880);
        const openfits::checksum_report report = open(file).verify(0);
        EXPECT_EQ(report.checksum, openfits::checksum_state::valid);
    }
}

TEST(writer, counts_the_records_of_a_header_as_a_save_writes_them)
{
    const std::vector<std::uint8_t> values{1, 2, 3};
    image_info info = image_of({3});
    for (int i = 0; i < 29; ++i) {
        info.header.set("KEY" + std::to_string(i), i);
    }
    writer output;
    (void)output.add_image(info, std::span<const std::uint8_t>(values));
    (void)output.add_image(info, std::span<const std::uint8_t>(values), {.reserved_cards = 10});
    // Five structural cards, the 29 of the caller and END; in the extension, XTENSION, PCOUNT and GCOUNT in place of
    // SIMPLE and EXTEND, and the reserved blank records; CHECKSUM and DATASUM with checksums.
    EXPECT_EQ(output.header_records(0), 35U);
    EXPECT_EQ(output.header_records(1), 46U);
    EXPECT_EQ(output.header_records(0, {.checksums = true}), 37U);
    EXPECT_TRUE(throws<usage_error>(errc::hdu_not_found, [&] { (void)output.header_records(2); }));

    // The long string of a reserved keyword, which a save writes on request alone, over OBJECT and two CONTINUE.
    image_info named = image_of({3});
    named.header.set("OBJECT", std::string(150, 'M'));
    writer continued;
    (void)continued.add_image(named, std::span<const std::uint8_t>(values));
    EXPECT_TRUE(throws<validation_error>(errc::continued_reserved_keyword, [&] { (void)continued.header_records(0); }));
    const write_options continuing{.continue_reserved_keywords = true};
    const std::size_t records = continued.header_records(0, continuing);
    EXPECT_EQ(records, 9U);
    // A file that departs from standard §4.2.1.2 on purpose, which written() would refuse.
    openfits::memory_sink sink;
    continued.save(sink, continuing);
    const std::vector<std::byte> file = sink.release();
    EXPECT_EQ(openfits::test::text(file).substr((records - 1) * 80, 80), std::string("END").append(77, ' '));
}

// A model read from a file is written again in one line, its header without the structural cards that the writer
// makes from the model.
TEST(writer, writes_back_a_model_read_from_a_file_in_one_line)
{
    const checked_model model;
    image_info scaled = image_of({3});
    scaled.bitpix = bitpix::int16;
    scaled.scaling = {.scale = 0.5, .zero = 10.0};
    scaled.blank = -1;
    scaled.header.set("OBJECT", "scaled");
    const std::vector<double> values{10.0, std::numeric_limits<double>::quiet_NaN(), 12.5};
    writer first;
    (void)first.add_image(scaled, std::span<const double>(values));
    (void)first.add_table(
        {.kind = openfits::hdu_kind::table, .rows = 3, .columns = {{.name = "NAME"}, {.name = "COUNT"}}},
        std::vector<openfits::column_values>{std::span(model.names), std::span(model.counts)});
    const reader input = open(written(first, {.checksums = true}));

    const std::vector<double> read = input.read_pixels<double>(0);
    const std::vector<std::string> names = input.read_strings(1, 0, {.trim_strings = true});
    const std::vector<std::int32_t> counts = input.read_column<std::int32_t>(1, 1);
    writer again;
    (void)again.add_image(openfits::without_structural(input.image(0)), std::span<const double>(read));
    (void)again.add_table(openfits::without_structural(input.table(1)),
                          std::vector<openfits::column_values>{std::span(names), std::span(counts)});
    const std::vector<std::byte> file = written(again);
    const reader output = open(file);
    EXPECT_EQ(output.image(0).scaling, scaled.scaling);
    EXPECT_EQ(output.image(0).blank, -1);
    EXPECT_EQ(output.hdu(0).header().value<std::string>("OBJECT"), "scaled");
    EXPECT_FALSE(output.hdu(0).header().contains("CHECKSUM"));
    EXPECT_EQ(output.read_pixels<std::int16_t>(0, {.scaling = openfits::scaling_policy::raw}),
              input.read_pixels<std::int16_t>(0, {.scaling = openfits::scaling_policy::raw}));
    EXPECT_EQ(output.table(1).columns, input.table(1).columns);
    EXPECT_EQ(output.read_strings(1, 0, {.trim_strings = true}), model.names);
}

} // namespace
