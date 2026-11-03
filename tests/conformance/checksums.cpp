// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Standard §4.4.2.7 and Appendix J: DATASUM and CHECKSUM verified, in constructed fixtures whose sums the tests compute
// themselves, integer by integer.

#include "support/checksums.h"

#include <openfits/checksum.h>
#include <openfits/error.h>
#include <openfits/io.h>
#include <openfits/reader.h>

#include "support/counting_source.h"
#include "support/fixture_builder.h"
#include "support/table_fixtures.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using openfits::checksum_report;
using openfits::checksum_state;
using openfits::errc;
using openfits::reader;
using openfits::test::big_endian;
using openfits::test::card_record;
using openfits::test::checked_unit;
using openfits::test::extension_records;
using openfits::test::primary_records;
using openfits::test::with;

constexpr std::uint32_t negative_zero = 0xFFFFFFFFU;

reader open(std::vector<std::byte> bytes, bool strict = false)
{
    return reader(std::make_unique<openfits::memory_source>(std::move(bytes)), {.strict = strict});
}

// A source without its bytes in memory, read through read() alone.
reader open_by_reads(std::vector<std::byte> bytes)
{
    return reader(std::make_unique<openfits::test::counting_source>(std::move(bytes)));
}

std::vector<std::byte> joined(std::vector<std::byte> first, const std::vector<std::byte>& second)
{
    first.insert(first.end(), second.begin(), second.end());
    return first;
}

// The values of a 37 x 3 image of 16-bit integers, small enough for every destination type.
std::vector<std::int16_t> values()
{
    std::vector<std::int16_t> result(std::size_t{37} * 3);
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i] = static_cast<std::int16_t>(i);
    }
    return result;
}

// A primary array of values(), with checksums.
std::vector<std::byte> checked_image(const std::vector<std::string>& more = {})
{
    return checked_unit(with(primary_records(16, {37, 3}), more), big_endian(values()));
}

constexpr std::size_t data_offset = 2880;

testing::AssertionResult states(const checksum_report& report, checksum_state datasum, checksum_state checksum)
{
    if (report.datasum == datasum && report.checksum == checksum) {
        return testing::AssertionSuccess();
    }
    return testing::AssertionFailure() << "DATASUM " << static_cast<int>(report.datasum) << ", CHECKSUM "
                                       << static_cast<int>(report.checksum);
}

TEST(conformance_checksums, a_unit_matches_its_checksums)
{
    const std::vector<std::byte> file = checked_image();
    const checksum_report report = open(file).verify(0);
    EXPECT_TRUE(states(report, checksum_state::valid, checksum_state::valid));
    EXPECT_EQ(report.data_sum, openfits::test::reference_sum(std::span(file).subspan(data_offset)));
    // CHECKSUM makes the sum of the whole unit negative zero (standard §4.4.2.7).
    EXPECT_EQ(report.hdu_sum, negative_zero);
    EXPECT_EQ(openfits::test::reference_sum(file), negative_zero);
    EXPECT_TRUE(open(file).hdu(0).diagnostics().empty());
}

TEST(conformance_checksums, a_change_of_the_data_fails_both_sums)
{
    std::vector<std::byte> file = checked_image();
    file[data_offset + 7] ^= std::byte{0x10};
    EXPECT_TRUE(states(open(file).verify(0), checksum_state::invalid, checksum_state::invalid));
    // The fill of the last block is part of the data records.
    file = checked_image();
    file.back() = std::byte{1};
    EXPECT_TRUE(states(open(file).verify(0), checksum_state::invalid, checksum_state::invalid));
}

// The header part of CHECKSUM: with DATASUM, a change of the header is found when the unit is walked, as a warning.
TEST(conformance_checksums, a_change_of_the_header_fails_checksum_alone)
{
    std::vector<std::byte> file = checked_image({card_record("OBJECT", "'M 31'")});
    const std::size_t object = (5 * 80) + 13;
    ASSERT_EQ(file[object], std::byte{'3'});
    file[object] = std::byte{'2'};
    const reader input = open(file);
    EXPECT_TRUE(states(input.verify(0), checksum_state::valid, checksum_state::invalid));
    const std::vector<openfits::diagnostic>& found = input.hdu(0).diagnostics();
    ASSERT_EQ(found.size(), 1U);
    EXPECT_EQ(found[0].severity, openfits::severity::warning);
    EXPECT_EQ(found[0].code, errc::checksum_mismatch);
    EXPECT_EQ(found[0].context.keyword, "CHECKSUM");
    EXPECT_EQ(found[0].context.offset, 6U * 80);
    // Blank fill after END is part of the header too.
    file = checked_image();
    file[data_offset - 1] = std::byte{'x'};
    EXPECT_TRUE(states(open(file).verify(0), checksum_state::valid, checksum_state::invalid));
}

// Without DATASUM, the walk cannot tell a change of the header from one of the data, and says nothing.
TEST(conformance_checksums, checksum_alone_is_verified_with_the_data)
{
    std::vector<std::byte> file = checked_unit(primary_records(16, {37, 3}), big_endian(values()), '\0',
                                               openfits::test::checksum_keywords::checksum_alone);
    EXPECT_TRUE(states(open(file).verify(0), checksum_state::absent, checksum_state::valid));
    file[data_offset + 3] ^= std::byte{1};
    const reader input = open(file);
    EXPECT_TRUE(states(input.verify(0), checksum_state::absent, checksum_state::invalid));
    EXPECT_TRUE(input.hdu(0).diagnostics().empty());
    // A read of the whole image verifies DATASUM alone.
    EXPECT_EQ(input.read_pixels<std::int16_t>(0).size(), 37U * 3);
}

TEST(conformance_checksums, datasum_alone_is_verified)
{
    std::vector<std::byte> records(big_endian(values()));
    records.resize(2880);
    const std::string sum = std::to_string(openfits::test::reference_sum(records));
    const openfits::test::fixture file =
        openfits::test::fixture()
            .header(with(primary_records(16, {37, 3}), {card_record("DATASUM", "'" + sum + "'")}))
            .data(big_endian(values()));
    EXPECT_TRUE(states(open(file.bytes()).verify(0), checksum_state::valid, checksum_state::absent));
}

// Standard §4.4.2.7: a value of blanks is undefined or unknown, and asserts nothing; the null string and an undefined
// value say no more.
TEST(conformance_checksums, blank_keywords_assert_nothing)
{
    for (const auto& [checksum, datasum] :
         {std::pair{"'                '", "'          '"}, std::pair{"''", "''"}, std::pair{"", ""}}) {
        const openfits::test::fixture file =
            openfits::test::fixture()
                .header(with(primary_records(16, {37, 3}),
                             {card_record("CHECKSUM", checksum), card_record("DATASUM", datasum)}))
                .data(big_endian(values()));
        const reader input = open(file.bytes(), true);
        EXPECT_TRUE(states(input.verify(0), checksum_state::absent, checksum_state::absent)) << checksum;
        EXPECT_EQ(input.read_pixels<std::int16_t>(0), values());
        EXPECT_TRUE(input.hdu(0).diagnostics().empty());
    }
}

// Standard §4.4.2.7: DATASUM may be omitted in a unit without data, and preferably holds 0.
TEST(conformance_checksums, a_unit_without_data_sums_to_0)
{
    const std::vector<std::byte> file = checked_unit(primary_records(), {});
    ASSERT_EQ(file.size(), 2880U);
    const checksum_report report = open(file).verify(0);
    EXPECT_TRUE(states(report, checksum_state::valid, checksum_state::valid));
    EXPECT_EQ(report.data_sum, 0U);
    const openfits::test::fixture other =
        openfits::test::fixture().header(with(primary_records(), {card_record("DATASUM", "'1'")}));
    EXPECT_TRUE(states(open(other.bytes()).verify(0), checksum_state::invalid, checksum_state::absent));
}

// The integers of the sum run over the data records whatever the size of the data: a sum of each size modulo 4.
TEST(conformance_checksums, units_of_every_size_modulo_4)
{
    for (const std::uint64_t size : {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 2879U, 2880U, 2881U}) {
        std::vector<std::byte> data(size);
        for (std::size_t i = 0; i < data.size(); ++i) {
            data[i] = static_cast<std::byte>(i % 253 + 1);
        }
        std::vector<std::byte> file = checked_unit(primary_records(8, {size}), data);
        EXPECT_TRUE(states(open(file).verify(0), checksum_state::valid, checksum_state::valid)) << size;
        EXPECT_EQ(open(file).read_pixels<std::uint8_t>(0).size(), size);
        file[data_offset + size - 1] ^= std::byte{0x80};
        EXPECT_TRUE(states(open(file).verify(0), checksum_state::invalid, checksum_state::invalid)) << size;
    }
}

// Ones' complement arithmetic finds every change of one bit: none leaves both sums valid, wherever it is, and one that
// leaves the unit unreadable is found too.
TEST(conformance_checksums, every_change_of_one_bit_is_found)
{
    const std::vector<std::byte> original =
        checked_unit(primary_records(8, {100}), std::vector<std::byte>(100, std::byte{7}));
    ASSERT_EQ(original.size(), 2U * 2880);
    std::size_t walked = 0;
    std::size_t unreadable = 0;
    for (std::size_t bit = 0; bit < original.size() * 8; ++bit) {
        std::vector<std::byte> file = original;
        file[bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
        try {
            const checksum_report report = open(std::move(file)).verify(0);
            ASSERT_FALSE(report.datasum == checksum_state::valid && report.checksum == checksum_state::valid) << bit;
            ++walked;
        } catch (const openfits::invalid_data_error& failure) {
            // A unit that the change leaves without a size, or a file without a primary header; never a mismatch,
            // which verify() reports.
            EXPECT_NE(failure.code(), errc::checksum_mismatch) << bit;
            ++unreadable;
        }
    }
    // Most changes leave a unit that is walked and verified.
    EXPECT_EQ(walked + unreadable, original.size() * 8);
    EXPECT_GT(walked, original.size() * 6);
}

TEST(conformance_checksums, each_unit_has_sums_of_its_own)
{
    std::vector<std::byte> file =
        joined(joined(checked_unit(primary_records(), {}),
                      checked_unit(extension_records("IMAGE", 16, {37, 3}), big_endian(values()))),
               checked_unit(extension_records("IMAGE", 8, {5}), std::vector<std::byte>(5, std::byte{9})));
    for (std::size_t unit = 0; unit < 3; ++unit) {
        EXPECT_TRUE(states(open(file).verify(unit), checksum_state::valid, checksum_state::valid)) << unit;
    }
    // Checksums in every unit make the sum of the whole file negative zero (standard §4.4.2.7).
    EXPECT_EQ(openfits::test::reference_sum(file), negative_zero);
    file.back() = std::byte{1};
    const reader input = open(file);
    EXPECT_TRUE(states(input.verify(1), checksum_state::valid, checksum_state::valid));
    EXPECT_TRUE(states(input.verify(2), checksum_state::invalid, checksum_state::invalid));
}

TEST(conformance_checksums, any_kind_of_unit_is_verified)
{
    // A binary table with a heap, an ASCII table whose fill is spaces, and an extension of an unknown type.
    const std::vector<std::byte> rows =
        openfits::test::byte_writer().value(std::int32_t{2}).value(std::int32_t{0}).bytes({0x41, 0x42}).data();
    const std::vector<std::byte> table = checked_unit(openfits::test::bintable_records(8, 1, {"1PB(2)"}, 2), rows);
    const std::string text = "  42 1.5";
    const std::vector<std::byte> ascii = checked_unit(
        openfits::test::ascii_table_records(8, 1, {{.column = 1, .format = "I4"}, {.column = 6, .format = "F3.1"}}),
        std::as_bytes(std::span(text)), ' ');
    const std::vector<std::byte> foreign =
        checked_unit(extension_records("FOREIGN", 8, {10}), std::vector<std::byte>(10, std::byte{3}));
    const std::vector<std::byte> file =
        joined(joined(joined(checked_unit(primary_records(), {}), table), ascii), foreign);
    const reader input = open(file);
    for (std::size_t unit = 1; unit < 4; ++unit) {
        EXPECT_TRUE(states(input.verify(unit), checksum_state::valid, checksum_state::valid)) << unit;
    }
}

// A whole-unit read verifies DATASUM in the same pass, and throws once the destination is filled, through every path
// of a read: the source in memory, the stored values read into the destination and converted there, wider and of the
// same size, and through a staging chunk; a pitch changes nothing.
TEST(conformance_checksums, a_read_of_the_whole_image_verifies_datasum)
{
    std::vector<std::byte> file = checked_image();
    for (const bool in_memory : {true, false}) {
        const reader input = in_memory ? open(file) : open_by_reads(file);
        EXPECT_EQ(input.read_pixels<std::int16_t>(0), values());
        EXPECT_EQ(input.read_pixels<std::int32_t>(0).size(), values().size());
        EXPECT_EQ(input.read_pixels<std::int8_t>(0, {.overflow = openfits::overflow_policy::clamp}).size(),
                  values().size());
        EXPECT_EQ(input.read_pixels<float>(0, {.pitch = 40}).size(), (2U * 40) + 37);
    }
    file[data_offset + 1] = std::byte{100};
    std::vector<std::int16_t> expected = values();
    expected[0] = 100;
    for (const bool in_memory : {true, false}) {
        const reader input = in_memory ? open(file) : open_by_reads(file);
        std::vector<std::int16_t> same(expected.size());
        std::vector<std::int32_t> wider(expected.size());
        std::vector<std::int8_t> narrower(expected.size());
        std::vector<float> pitched((2U * 40) + 37);
        EXPECT_TRUE(openfits::test::throws<openfits::integrity_error>(
            errc::checksum_mismatch, [&] { (void)input.read_pixels(0, std::span(same)); }));
        EXPECT_EQ(same, expected);
        EXPECT_TRUE(openfits::test::throws<openfits::integrity_error>(
            errc::checksum_mismatch, [&] { (void)input.read_pixels(0, std::span(wider)); }));
        EXPECT_EQ(wider[0], 100);
        EXPECT_EQ(wider.back(), expected.back());
        EXPECT_TRUE(openfits::test::throws<openfits::integrity_error>(errc::checksum_mismatch, [&] {
            (void)input.read_pixels(0, std::span(narrower), {.overflow = openfits::overflow_policy::clamp});
        }));
        EXPECT_EQ(narrower[0], 100);
        EXPECT_TRUE(openfits::test::throws<openfits::integrity_error>(
            errc::checksum_mismatch, [&] { (void)input.read_pixels(0, std::span(pitched), {.pitch = 40}); }));
        EXPECT_EQ(pitched[80 + 36], static_cast<float>(expected.back()));
        // The context names the unit and the keyword.
        try {
            (void)input.read_pixels<std::int16_t>(0);
            ADD_FAILURE() << "no mismatch";
        } catch (const openfits::integrity_error& failure) {
            EXPECT_EQ(failure.context().hdu, 0U);
            EXPECT_EQ(failure.context().keyword, "DATASUM");
        }
    }
}

// A damaged unit is read for what it still holds with verification off; a region is never verified.
TEST(conformance_checksums, reads_that_do_not_verify)
{
    std::vector<std::byte> file = checked_image();
    file[data_offset + 1] = std::byte{100};
    const reader input = open(file);
    EXPECT_EQ(input.read_pixels<std::int16_t>(0, {.verify_checksum = false})[0], 100);
    // Even a region of the whole image.
    EXPECT_EQ(input.read_pixels<std::int16_t>(0, {.region = openfits::region{}})[0], 100);
    EXPECT_EQ(input.read_pixels<std::int16_t>(0, {.region = openfits::region{.start = {0, 0}, .size = {37, 3}}}).size(),
              37U * 3);
}

// A random group is a part of its unit, which its reads do not verify; reader::verify() does.
TEST(conformance_checksums, reads_of_random_groups_do_not_verify)
{
    const std::vector<std::string> groups{
        card_record("SIMPLE", "T"), card_record("BITPIX", "16"), card_record("NAXIS", "2"),
        card_record("NAXIS1", "0"), card_record("NAXIS2", "2"),  card_record("GROUPS", "T"),
        card_record("PCOUNT", "1"), card_record("GCOUNT", "2"),  card_record("DATASUM", "'1'")};
    const openfits::test::fixture file =
        openfits::test::fixture().header(groups).data(big_endian(std::vector<std::int16_t>{1, 10, 11, 2, 20, 21}));
    const reader input = open(file.bytes());
    EXPECT_EQ(input.read_group<std::int16_t>(0, 1), (std::vector<std::int16_t>{20, 21}));
    EXPECT_EQ(input.read_parameter<std::int16_t>(0, 0), (std::vector<std::int16_t>{1, 2}));
    EXPECT_EQ(input.verify(0).datasum, checksum_state::invalid);
}

// Strict reading verifies every unit at the open, and fails at the first that does not match.
TEST(conformance_checksums, strict_reading_verifies_every_unit)
{
    std::vector<std::byte> file =
        joined(joined(checked_unit(primary_records(), {}),
                      checked_unit(extension_records("IMAGE", 16, {37, 3}), big_endian(values()))),
               checked_unit(extension_records("IMAGE", 8, {3}), std::vector<std::byte>(3, std::byte{1})));
    EXPECT_EQ(open(file, true).hdu_count(), 3U);
    // The data of the last unit, after the header of the first and the two blocks of the second.
    file[(4 * 2880) + 1] ^= std::byte{1};
    try {
        (void)open(file, true);
        ADD_FAILURE() << "no mismatch";
    } catch (const openfits::integrity_error& failure) {
        EXPECT_EQ(failure.code(), errc::checksum_mismatch);
        EXPECT_EQ(failure.context().hdu, 2U);
    }
    // Without strict reading, the file opens, and a whole read of the unit fails.
    const reader input = open(file);
    EXPECT_THROW((void)input.read_pixels<std::uint8_t>(2), openfits::integrity_error);
}

// A truncated unit has the sums of the bytes the file holds; one whose header gives no size has none.
TEST(conformance_checksums, units_that_the_file_cuts)
{
    std::vector<std::byte> file = checked_image();
    file.resize(data_offset + 100);
    EXPECT_TRUE(states(open(file).verify(0), checksum_state::invalid, checksum_state::invalid));
    const openfits::test::fixture broken = openfits::test::fixture().header(primary_records(24, {3}));
    EXPECT_THROW((void)open(broken.bytes()).verify(0), openfits::invalid_data_error);
}

} // namespace
