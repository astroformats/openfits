// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>
#include <openfits/header.h>
#include <openfits/image.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/types.h>
#include <openfits/writer.h>

#include "data/conversion.h"
#include "data/image_write.h"
#include "support/bytes.h"
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
            sink, conversion, numeric_type::uint16, std::as_bytes(std::span(values)),
            [&bytes](std::uint64_t written_bytes) { bytes += written_bytes; }, transfer);
        EXPECT_EQ(bytes, 2880U) << transfer;
        outputs.emplace_back(sink.data().begin(), sink.data().end());
    }
    for (const std::vector<std::byte>& output : outputs) {
        EXPECT_EQ(output, outputs.front());
    }
}

} // namespace
