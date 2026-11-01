// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Standard §3.3.2, §4.4.2.5, §5 and §7.1: the arrays of primary HDUs and IMAGE extensions, the representation of their
// values, their scaling and their undefined values, read from constructed fixtures.

#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/types.h>

#include "support/fixture_builder.h"

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

using openfits::reader;
using openfits::region;
using openfits::test::card_record;
using openfits::test::extension_records;
using openfits::test::fixture;
using openfits::test::primary_records;
using openfits::test::with;

reader open(const fixture& file)
{
    return reader(std::make_unique<openfits::memory_source>(std::vector<std::byte>(file.bytes())));
}

std::vector<std::byte> bytes_of(std::initializer_list<unsigned> values)
{
    std::vector<std::byte> bytes;
    for (const unsigned value : values) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

// §3.3.2: the index along axis 1 varies most rapidly, that along axis 2 next.
TEST(conformance_images, the_first_axis_varies_fastest)
{
    const fixture file = fixture().header(primary_records(8, {3, 2})).data(bytes_of({0, 1, 2, 10, 11, 12}));
    const reader input = open(file);
    EXPECT_EQ(input.read_pixels<std::uint8_t>(0, {.region = region{.start = {2, 0}, .size = {1, 2}}}),
              (std::vector<std::uint8_t>{2, 12}));
    EXPECT_EQ(input.read_pixels<std::uint8_t>(0, {.region = region{.start = {0, 1}, .size = {3, 1}}}),
              (std::vector<std::uint8_t>{10, 11, 12}));
}

// §3.3.2: the fill of the last block, set to zero, is not part of the array.
TEST(conformance_images, the_fill_of_the_last_block_is_not_read)
{
    const fixture file = fixture().header(primary_records(16, {3})).data(bytes_of({0, 1, 0, 2, 0, 3}));
    ASSERT_EQ(file.size(), 2U * 2880);
    EXPECT_EQ(open(file).read_pixels<std::int16_t>(0), (std::vector<std::int16_t>{1, 2, 3}));
}

// §4.4.2.5, Eq. 3: physical value = BZERO + BSCALE x array value.
TEST(conformance_images, the_physical_values_of_bscale_and_bzero)
{
    const fixture file =
        fixture()
            .header(with(primary_records(16, {3}), {card_record("BSCALE", "0.5"), card_record("BZERO", "10.0")}))
            .data(openfits::test::big_endian(std::vector<std::int16_t>{-4, 0, 3}));
    EXPECT_EQ(open(file).read_pixels<double>(0), (std::vector<double>{8.0, 10.0, 11.5}));
}

// §4.4.2.5: BLANK is the value in the array before scaling, -32768 for the physical 0 of unsigned 16-bit integers.
TEST(conformance_images, blank_is_the_stored_value_of_undefined_pixels)
{
    const fixture file =
        fixture()
            .header(with(primary_records(16, {2}), {card_record("BZERO", "32768"), card_record("BLANK", "-32768")}))
            .data(bytes_of({0x80, 0x00, 0x80, 0x01}));
    const reader input = open(file);
    const std::vector<float> values = input.read_pixels<float>(0);
    EXPECT_TRUE(std::isnan(values[0]));
    EXPECT_EQ(values[1], 1.0F);
    EXPECT_EQ(input.read_pixels<std::uint16_t>(0), (std::vector<std::uint16_t>{0, 1}));
}

// §5.2.1: 8-bit integers are unsigned.
TEST(conformance_images, eight_bit_integers_are_unsigned)
{
    const fixture file = fixture().header(primary_records(8, {2})).data(bytes_of({200, 255}));
    EXPECT_EQ(open(file).read_pixels<std::int16_t>(0), (std::vector<std::int16_t>{200, 255}));
}

// §5.2.2 to §5.2.4: 16, 32 and 64-bit integers are two's complement, the most significant byte first (§5).
TEST(conformance_images, wider_integers_are_twos_complement_and_big_endian)
{
    const reader input16 = open(fixture().header(primary_records(16, {2})).data(bytes_of({0xFF, 0xFE, 0x01, 0x02})));
    EXPECT_EQ(input16.read_pixels<std::int16_t>(0), (std::vector<std::int16_t>{-2, 258}));
    const reader input32 = open(fixture().header(primary_records(32, {1})).data(bytes_of({0x80, 0x00, 0x00, 0x00})));
    EXPECT_EQ(input32.read_pixels<std::int32_t>(0),
              (std::vector<std::int32_t>{std::numeric_limits<std::int32_t>::min()}));
    const reader input64 = open(
        fixture().header(primary_records(64, {1})).data(bytes_of({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFD})));
    EXPECT_EQ(input64.read_pixels<std::int64_t>(0), (std::vector<std::int64_t>{-3}));
}

// §5.2.5, Table 11: the offsets of BZERO that store unsigned integers in signed ones, and signed bytes in unsigned
// ones; the footnote's example: the stored byte 248 minus 128 is 120.
TEST(conformance_images, the_offsets_of_table_11)
{
    const reader signed_bytes =
        open(fixture().header(with(primary_records(8, {2}), {card_record("BZERO", "-128")})).data(bytes_of({248, 0})));
    EXPECT_EQ(signed_bytes.image(0).value_type(), openfits::numeric_type::int8);
    EXPECT_EQ(signed_bytes.read_pixels<std::int8_t>(0), (std::vector<std::int8_t>{120, -128}));

    const reader unsigned16 = open(fixture()
                                       .header(with(primary_records(16, {2}), {card_record("BZERO", "32768")}))
                                       .data(bytes_of({0x00, 0x00, 0x80, 0x00})));
    EXPECT_EQ(unsigned16.read_pixels<std::uint16_t>(0), (std::vector<std::uint16_t>{32768, 0}));

    const reader unsigned32 = open(fixture()
                                       .header(with(primary_records(32, {1}), {card_record("BZERO", "2147483648")}))
                                       .data(bytes_of({0x7F, 0xFF, 0xFF, 0xFF})));
    EXPECT_EQ(unsigned32.read_pixels<std::uint32_t>(0), (std::vector<std::uint32_t>{4294967295U}));

    const reader unsigned64 =
        open(fixture()
                 .header(with(primary_records(64, {2}), {card_record("BZERO", "9223372036854775808")}))
                 .data(bytes_of({0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x80, 0, 0, 0, 0, 0, 0, 1})));
    EXPECT_EQ(unsigned64.image(0).value_type(), openfits::numeric_type::uint64);
    EXPECT_EQ(unsigned64.read_pixels<std::uint64_t>(0),
              (std::vector<std::uint64_t>{std::numeric_limits<std::uint64_t>::max(), 1}));
}

// §5.3: IEEE 754 values, big-endian, every special value among them, which a read into the same type keeps.
TEST(conformance_images, ieee_values_and_their_special_values_are_kept)
{
    const std::vector<std::uint32_t> bits{0x3F80'0000U, 0x8000'0000U, 0x7F80'0000U,
                                          0xFF80'0000U, 0x7FC0'0042U, 0x0000'0001U};
    std::vector<std::byte> data;
    for (const std::uint32_t value : bits) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            data.push_back(static_cast<std::byte>((value >> static_cast<unsigned>(shift)) & 0xFFU));
        }
    }
    const reader input = open(fixture().header(primary_records(-32, {6})).data(data));
    const std::vector<float> values = input.read_pixels<float>(0);
    for (std::size_t i = 0; i < bits.size(); ++i) {
        EXPECT_EQ(std::bit_cast<std::uint32_t>(values[i]), bits[i]) << i;
    }
    EXPECT_EQ(values[0], 1.0F);
    EXPECT_TRUE(std::signbit(values[1]));
}

// §5.3: BLANK is not used with floating-point data, where NaN is the undefined value.
TEST(conformance_images, blank_of_floating_point_data_marks_nothing)
{
    const reader input = open(
        fixture().header(with(primary_records(-64, {1}), {card_record("BLANK", "0")})).data(std::vector<std::byte>(8)));
    EXPECT_FALSE(input.image(0).blank.has_value());
    const std::vector<double> values = input.read_pixels<double>(0);
    EXPECT_EQ(values, (std::vector<double>{0.0}));
}

// §7.1: an IMAGE extension holds an array as the primary HDU does (§7.1.3).
TEST(conformance_images, an_image_extension_holds_an_array)
{
    fixture file;
    file.primary();
    file.header(with(extension_records("IMAGE", 16, {2, 2}), {card_record("BZERO", "32768")}))
        .data(bytes_of({0x80, 0x00, 0x80, 0x01, 0x80, 0x02, 0x7F, 0xFF}));
    const reader input = open(file);
    EXPECT_EQ(input.image(1).geometry.axes, (std::vector<std::uint64_t>{2, 2}));
    EXPECT_EQ(input.read_pixels<std::uint16_t>(1), (std::vector<std::uint16_t>{0, 1, 2, 65535}));
}

} // namespace
