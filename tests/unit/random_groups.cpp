// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "data/random_groups.h"

#include <openfits/error.h>
#include <openfits/image.h>
#include <openfits/io.h>
#include <openfits/reader.h>

#include "io/thread_safe_source.h"
#include "support/fixture_builder.h"
#include "support/table_fixtures.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using openfits::errc;
using openfits::groups_info;
using openfits::reader;
using openfits::row_range;
using openfits::scaling_policy;
using openfits::usage_error;
using openfits::test::byte_writer;
using openfits::test::card_record;
using openfits::test::fixture;
using openfits::test::throws;

reader open(const fixture& file)
{
    return reader(std::make_unique<openfits::memory_source>(std::vector<std::byte>(file.bytes())));
}

// Random groups of 16-bit values (standard §6): three groups of four parameters, U, V and a date in two parts, and a
// 3 x 2 array; parameter p of group g is 10 g + p, and element e of its array 100 g + e.
fixture uv_data(const std::vector<std::string>& more = {})
{
    std::vector<std::string> records{
        card_record("SIMPLE", "T"),      card_record("BITPIX", "16"),        card_record("NAXIS", "3"),
        card_record("NAXIS1", "0"),      card_record("NAXIS2", "3"),         card_record("NAXIS3", "2"),
        card_record("GROUPS", "T"),      card_record("PCOUNT", "4"),         card_record("GCOUNT", "3"),
        card_record("PTYPE1", "'UU'"),   card_record("PSCAL1", "0.5"),       card_record("PTYPE2", "'VV      '"),
        card_record("PTYPE3", "'DATE'"), card_record("PZERO3", "2400000.5"), card_record("PTYPE4", "'date'"),
        card_record("PSCAL4", "0.001"),  card_record("BSCALE", "2"),         card_record("BZERO", "1")};
    records.insert(records.end(), more.begin(), more.end());
    byte_writer data;
    for (std::int16_t g = 0; g < 3; ++g) {
        for (std::int16_t p = 0; p < 4; ++p) {
            data.value<std::int16_t>(static_cast<std::int16_t>((10 * g) + p));
        }
        for (std::int16_t e = 0; e < 6; ++e) {
            data.value<std::int16_t>(static_cast<std::int16_t>((100 * g) + e));
        }
    }
    return fixture().header(records).data(data.data());
}

TEST(random_groups, describes_the_groups)
{
    const reader input = open(uv_data());
    EXPECT_EQ(input.hdu(0).kind(), openfits::hdu_kind::random_groups);
    EXPECT_EQ(input.hdu(0).data_size(), 3U * (4 + 6) * 2);
    const groups_info groups = input.groups(0);
    EXPECT_EQ(groups.bitpix, openfits::bitpix::int16);
    EXPECT_EQ(groups.geometry.axes, (std::vector<std::uint64_t>{3, 2}));
    EXPECT_EQ(groups.scaling, (openfits::scaling{.scale = 2.0, .zero = 1.0}));
    EXPECT_EQ(groups.count, 3U);
    ASSERT_EQ(groups.parameters.size(), 4U);
    EXPECT_EQ(groups.parameters[0].name, "UU");
    EXPECT_EQ(groups.parameters[0].scaling, (openfits::scaling{.scale = 0.5}));
    EXPECT_EQ(groups.parameters[1].name, "VV");
    EXPECT_EQ(groups.parameters[2].scaling, (openfits::scaling{.zero = 2400000.5}));
    EXPECT_EQ(groups.header, input.hdu(0).header());
    EXPECT_TRUE(throws<usage_error>(errc::not_an_image, [&] { (void)input.image(0); }));
}

TEST(random_groups, reads_a_parameter_of_every_group)
{
    // Standard §6.1.2, Eq. 5: physical = PZEROn + PSCALn x parameter.
    const reader input = open(uv_data());
    EXPECT_EQ(input.read_parameter<double>(0, 0), (std::vector<double>{0.0, 5.0, 10.0}));
    EXPECT_EQ(input.read_parameter<std::int16_t>(0, 1), (std::vector<std::int16_t>{1, 11, 21}));
    EXPECT_EQ(input.read_parameter<std::int16_t>(0, 0, {.scaling = scaling_policy::raw}),
              (std::vector<std::int16_t>{0, 10, 20}));
    EXPECT_EQ(input.read_parameter<double>(0, 0, {.groups = row_range{.first = 1, .count = 1}}),
              (std::vector<double>{5.0}));
    EXPECT_EQ(input.read_parameter<double>(0, "vv"), (std::vector<double>{1.0, 11.0, 21.0}));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { (void)input.read_parameter<double>(0, 4); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { (void)input.read_parameter<double>(0, "WW"); }));
}

TEST(random_groups, sums_the_parameters_of_one_name)
{
    // Standard §6.1.2 and §6.3: the value of a parameter is the sum of the physical values of the parameters of its
    // name, which gives it more precision than the stored type: the date as a day and its fraction.
    const reader input = open(uv_data());
    const std::vector<double> dates = input.read_parameter<double>(0, "DATE");
    ASSERT_EQ(dates.size(), 3U);
    for (std::size_t g = 0; g < 3; ++g) {
        const double day = 2400000.5 + (10.0 * static_cast<double>(g)) + 2.0;
        const double fraction = 0.001 * ((10.0 * static_cast<double>(g)) + 3.0);
        EXPECT_EQ(dates[g], day + fraction) << g;
    }
    EXPECT_EQ(input.read_parameter<std::int64_t>(0, "DATE", {.rounding = openfits::rounding::nearest}).front(),
              2400003);
}

TEST(random_groups, sums_a_block_of_groups_at_a_time)
{
    // Blocks of one group, a block of two and one of one, and a block of two from group 1 give the sums of one read.
    const fixture file = uv_data();
    const reader input = open(file);
    const openfits::detail::thread_safe_source source(
        std::make_unique<openfits::memory_source>(std::vector<std::byte>(file.bytes())));
    const groups_info groups = input.groups(0);
    const std::vector<double> expected = input.read_parameter<double>(0, "DATE");
    struct block_case
    {
        std::size_t transfer = 0;
        std::uint64_t first = 0;
    };
    for (const block_case& block :
         {block_case{.transfer = 1}, block_case{.transfer = 32}, block_case{.transfer = 32, .first = 1}}) {
        std::vector<double> dates(3 - block.first);
        openfits::detail::read_parameters(source, input.hdu(0), groups, 0, "DATE", openfits::numeric_type::float64,
                                          std::as_writable_bytes(std::span<double>(dates)),
                                          {.groups = row_range{.first = block.first}}, block.transfer);
        EXPECT_EQ(dates,
                  std::vector<double>(expected.begin() + static_cast<std::ptrdiff_t>(block.first), expected.end()))
            << block.transfer << " " << block.first;
    }
}

TEST(random_groups, reads_the_array_of_a_group)
{
    const reader input = open(uv_data());
    // The array of group 1, 100 to 105 stored, scaled by BSCALE and BZERO.
    EXPECT_EQ(input.read_group<double>(0, 1), (std::vector<double>{201, 203, 205, 207, 209, 211}));
    EXPECT_EQ(input.read_group<std::int16_t>(0, 2, {.scaling = scaling_policy::raw}),
              (std::vector<std::int16_t>{200, 201, 202, 203, 204, 205}));
    // A region of it, as of an image.
    EXPECT_EQ(input.read_group<std::int16_t>(
                  0, 1, {.region = openfits::region{.start = {1, 0}, .size = {2, 2}}, .scaling = scaling_policy::raw}),
              (std::vector<std::int16_t>{101, 102, 104, 105}));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { (void)input.read_group<double>(0, 3); }));
}

TEST(random_groups, a_naxis_of_1_holds_one_element_in_each_group)
{
    // Eq. 6 with NAXIS = 1: the empty product.
    byte_writer data;
    data.values<float>({1.0F, 2.0F, 3.0F, 4.0F});
    const fixture file = fixture()
                             .header({card_record("SIMPLE", "T"), card_record("BITPIX", "-32"),
                                      card_record("NAXIS", "1"), card_record("NAXIS1", "0"), card_record("GROUPS", "T"),
                                      card_record("PCOUNT", "1"), card_record("GCOUNT", "2")})
                             .data(data.data());
    const reader input = open(file);
    EXPECT_TRUE(input.groups(0).geometry.axes.empty());
    EXPECT_EQ(input.read_parameter<float>(0, 0), (std::vector<float>{1.0F, 3.0F}));
    EXPECT_EQ(input.read_group<float>(0, 1), (std::vector<float>{4.0F}));
}

TEST(random_groups, units_of_other_kinds_hold_no_random_groups)
{
    fixture file;
    file.header(openfits::test::primary_records(8, {4})).data(4);
    const reader input = open(file);
    EXPECT_TRUE(throws<usage_error>(errc::not_random_groups, [&] { (void)input.groups(0); }));
    EXPECT_TRUE(throws<usage_error>(errc::not_random_groups, [&] { (void)input.read_parameter<double>(0, 0); }));
}

TEST(random_groups, the_parameters_are_described_within_the_allocation_limit)
{
    // Without groups, nothing in the unit bounds PCOUNT.
    const auto empty = [](const std::string& pcount) {
        return fixture().header({card_record("SIMPLE", "T"), card_record("BITPIX", "8"), card_record("NAXIS", "2"),
                                 card_record("NAXIS1", "0"), card_record("NAXIS2", "1"), card_record("GROUPS", "T"),
                                 card_record("PCOUNT", pcount), card_record("GCOUNT", "0")});
    };
    const reader huge = open(empty("1000000000000000"));
    EXPECT_TRUE(throws<openfits::limit_error>(errc::allocation_too_large, [&] { (void)huge.groups(0); }));
    EXPECT_TRUE(
        throws<openfits::limit_error>(errc::allocation_too_large, [&] { (void)huge.read_parameter<double>(0, 0); }));
    EXPECT_EQ(open(empty("1000")).groups(0).parameters.size(), 1000U);
    openfits::read_options options;
    options.limits.max_allocation = 1000;
    const reader limited(std::make_unique<openfits::memory_source>(std::vector<std::byte>(empty("1000").bytes())),
                         options);
    EXPECT_TRUE(throws<openfits::limit_error>(errc::allocation_too_large, [&] { (void)limited.groups(0); }));
}

TEST(random_groups, a_pscal_of_0_leaves_its_parameter_unavailable)
{
    const reader input = open(uv_data({card_record("PSCAL2", "0")}));
    EXPECT_TRUE(throws<openfits::invalid_data_error>(errc::reserved_keyword,
                                                     [&] { (void)input.read_parameter<double>(0, 1); }));
    EXPECT_EQ(input.read_parameter<double>(0, 0).size(), 3U);
    bool found = false;
    for (const openfits::diagnostic& entry : input.hdu(0).diagnostics()) {
        found = found || (entry.code == errc::reserved_keyword && entry.severity == openfits::severity::error);
    }
    EXPECT_TRUE(found);
}

} // namespace
