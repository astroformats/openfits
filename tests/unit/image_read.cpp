// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "data/image_read.h"

#include <openfits/error.h>
#include <openfits/image.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/types.h>

#include "core/scratch_buffer.h"
#include "data/image_description.h"
#include "io/thread_safe_source.h"
#include "support/counting_source.h"
#include "support/fixture_builder.h"
#include "support/throws.h"
#include "support/virtual_source.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <ostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using openfits::bitpix;
using openfits::blank_policy;
using openfits::cancelled_error;
using openfits::errc;
using openfits::image_info;
using openfits::invalid_data_error;
using openfits::io_error;
using openfits::limit_error;
using openfits::numeric_type;
using openfits::pixel_read_options;
using openfits::reader;
using openfits::region;
using openfits::scaling_policy;
using openfits::usage_error;
using openfits::test::big_endian;
using openfits::test::card_record;
using openfits::test::counting_source;
using openfits::test::extension_records;
using openfits::test::fixture;
using openfits::test::primary_records;
using openfits::test::served_read;
using openfits::test::throws;
using openfits::test::with;

// How the reader gets the bytes: from memory, where it reads in place, or through read(), in runs.
enum class source_kind
{
    memory,
    streamed,
};

// NOLINTNEXTLINE(readability-identifier-naming)
void PrintTo(source_kind kind, std::ostream* output)
{
    *output << (kind == source_kind::memory ? "memory" : "streamed");
}

// The source owns a copy of the bytes, so that a temporary fixture can be opened.
reader open(const fixture& file, source_kind kind = source_kind::memory, const openfits::read_options& options = {})
{
    if (kind == source_kind::memory) {
        return reader(std::make_unique<openfits::memory_source>(std::vector<std::byte>(file.bytes())), options);
    }
    return reader(std::make_unique<counting_source>(file.bytes()), options);
}

// A primary array of 16-bit values, value i the i-th of the data, with the records given after the mandatory ones.
fixture int16_image(const std::vector<std::uint64_t>& axes, const std::vector<std::int16_t>& values,
                    const std::vector<std::string>& more = {})
{
    return fixture().header(with(primary_records(16, axes), more)).data(big_endian(values));
}

// The values of an image of the given axes: -100, -97, -94, ...: distinct, so that a value from a wrong element shows.
std::vector<std::int16_t> ramp(const std::vector<std::uint64_t>& axes)
{
    const std::uint64_t count = std::accumulate(axes.begin(), axes.end(), std::uint64_t{1}, std::multiplies<>());
    std::vector<std::int16_t> values(count);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = static_cast<std::int16_t>((static_cast<int>(i) * 3) - 100);
    }
    return values;
}

// The values of the region of an image, as the plainest gather gives them: the indices of each axis in the order of the
// region, the rows pitch elements apart, fill between them.
template <typename T>
std::vector<T> gather(const std::vector<T>& image, const std::vector<std::uint64_t>& axes, const region& box,
                      std::uint64_t pitch = 0, T fill = T{})
{
    std::vector<std::vector<std::uint64_t>> picks(axes.size());
    for (std::size_t a = 0; a < axes.size(); ++a) {
        const std::uint64_t start = box.start.empty() ? 0 : box.start[a];
        const std::uint64_t size = box.size.empty() ? axes[a] - start : box.size[a];
        const std::int64_t step = box.step.empty() ? 1 : box.step[a];
        const auto stride = static_cast<std::uint64_t>(step < 0 ? -step : step);
        for (std::uint64_t k = 0; k * stride < size; ++k) {
            picks[a].push_back(step > 0 ? start + (k * stride) : start + size - 1 - (k * stride));
        }
    }
    const std::uint64_t row = picks[0].size();
    std::uint64_t rows = 1;
    for (std::size_t a = 1; a < axes.size(); ++a) {
        rows *= picks[a].size();
    }
    const std::uint64_t step = pitch == 0 ? row : pitch;
    std::vector<T> out(((rows - 1) * step) + row, fill);
    std::vector<std::uint64_t> position(axes.size(), 0);
    for (std::uint64_t r = 0; r < rows; ++r) {
        for (std::uint64_t k = 0; k < row; ++k) {
            std::uint64_t index = picks[0][k];
            std::uint64_t stride = axes[0];
            for (std::size_t a = 1; a < axes.size(); ++a) {
                index += picks[a][position[a]] * stride;
                stride *= axes[a];
            }
            out[(r * step) + k] = image[index];
        }
        for (std::size_t a = 1; a < axes.size(); ++a) {
            if (++position[a] < picks[a].size()) {
                break;
            }
            position[a] = 0;
        }
    }
    return out;
}

template <typename T, typename From> std::vector<T> converted(const std::vector<From>& values)
{
    std::vector<T> out;
    out.reserve(values.size());
    for (const From value : values) {
        out.push_back(static_cast<T>(value));
    }
    return out;
}

TEST(image_read, describes_the_image_of_a_primary_hdu)
{
    const fixture file = int16_image({3, 2}, ramp({3, 2}),
                                     {card_record("BSCALE", "2.5"), card_record("BZERO", "-1.0"),
                                      card_record("BLANK", "-32768"), card_record("OBJECT", "'M 31'")});
    const reader input = open(file);
    const image_info image = input.image(0);
    EXPECT_EQ(image.bitpix, bitpix::int16);
    EXPECT_EQ(image.geometry.axes, (std::vector<std::uint64_t>{3, 2}));
    EXPECT_EQ(image.scaling, (openfits::scaling{.scale = 2.5, .zero = -1.0}));
    EXPECT_EQ(image.blank, -32768);
    EXPECT_EQ(image.header, input.hdu(0).header());
    EXPECT_EQ(image.value_type(), numeric_type::float32);
    EXPECT_EQ(image.geometry.element_count(), 6U);
}

TEST(image_read, an_image_of_naxis_0_has_no_axes_and_no_values)
{
    const reader input = open(fixture().primary());
    const image_info image = input.image(0);
    EXPECT_TRUE(image.geometry.axes.empty());
    EXPECT_EQ(image.geometry.element_count(), 0U);
    EXPECT_TRUE(input.read_pixels<float>(0).empty());
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] {
        (void)input.read_pixels<float>(0, {.region = region{.start = {0}, .size = {1}}});
    }));
}

TEST(image_read, units_of_other_kinds_hold_no_image)
{
    fixture file;
    file.primary();
    file.header(with(extension_records("BINTABLE", 8, {4, 2}), {card_record("TFIELDS", "0")})).data(8);
    file.header(with(extension_records("FOREIGN", 8, {16}), {})).data(16);
    file.header(
            with(extension_records("BINTABLE", 8, {4, 2}), {card_record("TFIELDS", "0"), card_record("ZIMAGE", "T")}))
        .data(8);
    const reader input = open(file);
    EXPECT_TRUE(throws<usage_error>(errc::not_an_image, [&] { (void)input.image(1); }));
    EXPECT_TRUE(throws<usage_error>(errc::not_an_image, [&] { (void)input.image(2); }));
    // A compressed image is an image, and this one, without the keywords of standard §10.1.1, describes none.
    EXPECT_TRUE(throws<invalid_data_error>(errc::invalid_compression, [&] { (void)input.image(3); }));
    EXPECT_TRUE(throws<usage_error>(errc::not_an_image, [&] { (void)input.read_pixels<std::uint8_t>(1); }));

    const fixture groups =
        fixture()
            .header(with(primary_records(16, {0, 3}),
                         {card_record("GROUPS", "T"), card_record("PCOUNT", "0"), card_record("GCOUNT", "1")}))
            .data(6);
    EXPECT_TRUE(throws<usage_error>(errc::not_an_image, [&] { (void)open(groups).image(0); }));
}

TEST(image_read, a_bitpix_outside_its_values_describes_no_image)
{
    const reader input = open(fixture().header(primary_records(12, {4})).data(6));
    EXPECT_TRUE(throws<invalid_data_error>(errc::invalid_structure, [&] { (void)input.image(0); }));
}

class region_read : public testing::TestWithParam<source_kind>
{};

INSTANTIATE_TEST_SUITE_P(sources, region_read, testing::Values(source_kind::memory, source_kind::streamed),
                         [](const testing::TestParamInfo<source_kind>& instance) {
                             return instance.param == source_kind::memory ? "memory" : "streamed";
                         });

// Regions of a 4-dimensional cube in every dimension, with steps forwards and backwards, read as the stored type and
// as float, which converts through the staging chunk.
TEST_P(region_read, reads_regions_in_every_dimension)
{
    const std::vector<std::uint64_t> axes{5, 4, 3, 2};
    const std::vector<std::int16_t> values = ramp(axes);
    const reader input = open(int16_image(axes, values), GetParam());
    const std::vector<region> regions{
        {},
        {.start = {0, 0, 0, 0}, .size = {5, 4, 3, 2}},
        {.start = {1, 1, 1, 0}, .size = {3, 2, 2, 2}},
        {.step = {2, 1, 2, 1}},
        {.step = {1, -1, 1, 1}},
        {.step = {-2, 1, 1, 1}},
        {.step = {-1, -1, -1, -1}},
        {.start = {1, 0, 1, 1}, .size = {4, 4, 2, 1}, .step = {-3, 2, -1, 1}},
        {.start = {4, 3, 2, 1}, .size = {1, 1, 1, 1}},
        {.start = {2, 0, 0, 0}, .size = {1, 4, 3, 2}},
        {.start = {0, 0, 2, 0}, .size = {5, 4, 1, 2}},
    };
    for (const region& box : regions) {
        SCOPED_TRACE(testing::Message() << "region " << testing::PrintToString(box.start) << " "
                                        << testing::PrintToString(box.size) << " " << testing::PrintToString(box.step));
        const std::vector<std::int16_t> expected = gather(values, axes, box);
        EXPECT_EQ(input.read_pixels<std::int16_t>(0, {.region = box}), expected);
        EXPECT_EQ(input.read_pixels<float>(0, {.region = box}), converted<float>(expected));
    }
}

TEST_P(region_read, a_whole_image_upside_down_is_the_flipped_array)
{
    const std::vector<std::uint64_t> axes{4, 3};
    const std::vector<std::int16_t> values = ramp(axes);
    const reader input = open(int16_image(axes, values), GetParam());
    const std::vector<std::int16_t> flipped = input.read_pixels<std::int16_t>(0, {.region = region{.step = {1, -1}}});
    const std::vector<std::int16_t> expected{values[8], values[9], values[10], values[11], values[4], values[5],
                                             values[6], values[7], values[0],  values[1],  values[2], values[3]};
    EXPECT_EQ(flipped, expected);
}

TEST_P(region_read, lands_in_rows_a_pitch_apart_and_leaves_the_elements_between)
{
    const std::vector<std::uint64_t> axes{6, 5, 2};
    const std::vector<std::int16_t> values = ramp(axes);
    const reader input = open(int16_image(axes, values), GetParam());
    const region box{.start = {1, 1, 0}, .size = {3, 3, 2}, .step = {1, -1, 1}};
    constexpr std::int16_t fill = 12345;
    std::vector<std::int16_t> destination(((6 - 1) * 7) + 3, fill);
    (void)input.read_pixels(0, std::span<std::int16_t>(destination), {.region = box, .pitch = 7});
    EXPECT_EQ(destination, gather(values, axes, box, 7, fill));

    // A larger buffer than the rows span is accepted, and its end left as it is.
    std::vector<float> wide(64, -1.0F);
    (void)input.read_pixels(0, std::span<float>(wide), {.region = box, .pitch = 8});
    const std::vector<float> rows = converted<float>(gather(values, axes, box, 8, std::int16_t{-1}));
    EXPECT_TRUE(std::equal(rows.begin(), rows.end(), wide.begin()));
    EXPECT_TRUE(std::all_of(wide.begin() + static_cast<std::ptrdiff_t>(rows.size()), wide.end(),
                            [](float value) { return value == -1.0F; }));

    // The vector of the library has zeros between the rows.
    EXPECT_EQ(input.read_pixels<std::int16_t>(0, {.region = box, .pitch = 7}), gather(values, axes, box, 7));
}

TEST_P(region_read, a_mask_has_the_layout_of_the_destination)
{
    const std::vector<std::uint64_t> axes{3, 2};
    const std::vector<std::int16_t> values{1, -32768, 3, 4, 5, -32768};
    const reader input = open(int16_image(axes, values, {card_record("BLANK", "-32768")}), GetParam());
    std::vector<float> destination(9, 7.0F);
    std::vector<std::uint8_t> mask(9, 9);
    (void)input.read_pixels(0, std::span<float>(destination),
                            {.pitch = 5, .blank = blank_policy::mask, .mask = std::span<std::uint8_t>(mask)});
    EXPECT_EQ(mask, (std::vector<std::uint8_t>{0, 1, 0, 9, 9, 0, 0, 1, 9}));
    EXPECT_EQ(destination[0], 1.0F);
    EXPECT_TRUE(std::isnan(destination[1]));
    EXPECT_EQ(destination[3], 7.0F);
    EXPECT_EQ(destination[6], 5.0F);
}

TEST(image_read, refuses_regions_that_do_not_lie_within_the_image)
{
    const reader input = open(int16_image({4, 3}, ramp({4, 3})));
    const std::vector<region> invalid{
        {.start = {0}},
        {.size = {4, 3, 1}},
        {.step = {1}},
        {.start = {4, 0}},
        {.start = {0, 3}},
        {.size = {0, 3}},
        {.start = {1, 0}, .size = {4, 3}},
        {.step = {1, 0}},
    };
    for (const region& box : invalid) {
        EXPECT_TRUE(throws<usage_error>(errc::invalid_argument,
                                        [&] { (void)input.read_pixels<std::int16_t>(0, {.region = box}); }))
            << testing::PrintToString(box.start) << testing::PrintToString(box.size)
            << testing::PrintToString(box.step);
    }
}

TEST(image_read, refuses_a_destination_or_a_mask_that_does_not_fit)
{
    const reader input = open(int16_image({4, 3}, ramp({4, 3})));
    std::vector<std::int16_t> small(11);
    std::vector<std::int16_t> large(13);
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument,
                                    [&] { (void)input.read_pixels(0, std::span<std::int16_t>(small)); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument,
                                    [&] { (void)input.read_pixels(0, std::span<std::int16_t>(large)); }));
    // With a pitch the destination spans (rows - 1) x pitch + row elements, at least.
    std::vector<std::int16_t> pitched(((3 - 1) * 5) + 3);
    EXPECT_TRUE(throws<usage_error>(
        errc::invalid_argument, [&] { (void)input.read_pixels(0, std::span<std::int16_t>(pitched), {.pitch = 5}); }));
    // A pitch shorter than a row.
    std::vector<std::int16_t> rows(12);
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument,
                                    [&] { (void)input.read_pixels(0, std::span<std::int16_t>(rows), {.pitch = 3}); }));
    std::vector<std::uint8_t> mask(11);
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] {
        (void)input.read_pixels(0, std::span<std::int16_t>(rows),
                                {.blank = blank_policy::mask, .mask = std::span<std::uint8_t>(mask)});
    }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] {
        (void)input.read_pixels(0, std::span<std::int16_t>(rows), {.blank = blank_policy::mask});
    }));
}

// The reads of the counting source after the open: a region costs reads in proportion to its bytes, not its rows.
std::vector<served_read> reads_of(const fixture& file, const pixel_read_options& options, std::size_t elements)
{
    auto source = std::make_unique<counting_source>(file.bytes());
    const counting_source& counter = *source;
    const reader input(std::move(source));
    const std::size_t before = counter.reads().size();
    std::vector<std::int16_t> destination(elements);
    (void)input.read_pixels(0, std::span<std::int16_t>(destination), options);
    const std::vector<served_read> all = counter.reads();
    return {all.begin() + static_cast<std::ptrdiff_t>(before), all.end()};
}

TEST(image_read, a_sparse_region_is_read_run_by_run)
{
    // Rows of 80 KB, of which the region takes 8 bytes each: the runs are further apart than 64 KiB, and a small share
    // of the bytes that span them.
    const std::vector<std::uint64_t> axes{40000, 10};
    const fixture file = int16_image(axes, ramp(axes));
    const std::vector<served_read> reads = reads_of(file, {.region = region{.start = {100, 0}, .size = {4, 10}}}, 40);
    ASSERT_EQ(reads.size(), 10U);
    for (std::size_t r = 0; r < reads.size(); ++r) {
        EXPECT_EQ(reads[r], (served_read{.offset = 2880 + (r * 80000) + 200, .size = 8}));
    }
}

TEST(image_read, runs_close_together_are_read_together)
{
    // Rows of 8 KB: the runs of 8 bytes are less than 64 KiB apart, so that one read takes them all, as many as the
    // staging chunk holds: the 50 rows of 400 KB, and the 100 rows of 800 KB in two reads.
    static_assert(openfits::detail::staging_size == std::size_t{448} << 10);
    const std::vector<std::uint64_t> axes{4000, 100};
    const fixture file = int16_image(axes, ramp(axes));
    EXPECT_EQ(reads_of(file, {.region = region{.start = {10, 0}, .size = {4, 50}}}, 200),
              (std::vector<served_read>{{.offset = 2880 + 20, .size = (49 * 8000) + 8}}));
    // The first read takes the runs that end within 458,752 bytes of the first: 58 rows.
    EXPECT_EQ(reads_of(file, {.region = region{.start = {10, 0}, .size = {4, 100}}}, 400),
              (std::vector<served_read>{{.offset = 2880 + 20, .size = (57 * 8000) + 8},
                                        {.offset = 2880 + (58 * 8000) + 20, .size = (41 * 8000) + 8}}));
}

TEST(image_read, a_dense_region_is_read_in_chunks_of_the_bytes_that_span_it)
{
    // The runs are 80 KB apart, beyond the gap, but they hold most of the bytes that span them: read in chunks of the
    // staging size, five rows of 380 KB each.
    const std::vector<std::uint64_t> axes{40000, 10};
    const fixture file = int16_image(axes, ramp(axes));
    const std::vector<served_read> reads =
        reads_of(file, {.region = region{.start = {0, 0}, .size = {30000, 10}}}, 300000);
    EXPECT_EQ(reads, (std::vector<served_read>{{.offset = 2880, .size = (4 * 80000) + 60000},
                                               {.offset = 2880 + (5 * 80000), .size = (4 * 80000) + 60000}}));
}

TEST(image_read, a_contiguous_region_is_read_into_the_destination_itself)
{
    // Full rows: one stretch of the data, read in one read straight into the destination.
    const std::vector<std::uint64_t> axes{1000, 100};
    const fixture file = int16_image(axes, ramp(axes));
    EXPECT_EQ(reads_of(file, {.region = region{.start = {0, 10}, .size = {1000, 20}}}, 20000),
              (std::vector<served_read>{{.offset = 2880 + 20000, .size = 40000}}));
    EXPECT_EQ(reads_of(file, {}, 100000), (std::vector<served_read>{{.offset = 2880, .size = 200000}}));
}

// A contiguous read in pieces of a few values, through read(): the stored values of each piece land at the end of its
// part of a destination of a wider type, in the part itself for a type of their size, and in a staging chunk for a
// narrower type.
TEST(image_read, a_contiguous_read_in_pieces_converts_each_piece)
{
    const std::vector<std::uint64_t> axes{7, 5};
    std::vector<std::int16_t> values = ramp(axes);
    values[3] = 300;
    values[17] = -32768;
    const fixture file = int16_image(axes, values, {card_record("BLANK", "-32768")});
    const reader input = open(file);
    const openfits::hdu_info& unit = input.hdu(0);
    const image_info image = openfits::detail::describe_image(unit, false);
    const openfits::detail::thread_safe_source source(std::make_unique<counting_source>(file.bytes()));
    const auto read = [&](numeric_type type, std::span<std::byte> destination, const pixel_read_options& options,
                          std::size_t transfer) {
        return openfits::detail::read_pixels(source, unit, image, type, destination, options, transfer).clamped;
    };
    for (const std::size_t transfer : {2U, 6U, 16U, 1000U}) {
        SCOPED_TRACE(testing::Message() << "transfer " << transfer);
        std::vector<float> floats(values.size());
        std::vector<std::uint8_t> mask(values.size(), 9);
        (void)read(numeric_type::float32, std::as_writable_bytes(std::span(floats)),
                   {.blank = blank_policy::mask, .mask = std::span<std::uint8_t>(mask)}, transfer);
        for (std::size_t i = 0; i < values.size(); ++i) {
            EXPECT_EQ(mask[i], i == 17 ? 1 : 0) << "value " << i;
            if (i != 17) {
                EXPECT_EQ(floats[i], static_cast<float>(values[i])) << "value " << i;
            }
        }
        EXPECT_TRUE(std::isnan(floats[17]));

        // BLANK is a value like any other for integer destinations without a policy for undefined values.
        std::vector<std::int64_t> wide(values.size());
        (void)read(numeric_type::int64, std::as_writable_bytes(std::span(wide)), {}, transfer);
        EXPECT_EQ(wide, converted<std::int64_t>(values));

        std::vector<std::int16_t> same(values.size());
        (void)read(numeric_type::int16, std::as_writable_bytes(std::span(same)), {}, transfer);
        EXPECT_EQ(same, values);

        std::vector<std::int8_t> narrow(values.size());
        EXPECT_EQ(read(numeric_type::int8, std::as_writable_bytes(std::span(narrow)),
                       {.overflow = openfits::overflow_policy::clamp}, transfer),
                  2U);
        std::vector<std::int8_t> clamped = converted<std::int8_t>(values);
        clamped[3] = 127;
        clamped[17] = -128;
        EXPECT_EQ(narrow, clamped);
    }
}

// An image of 6.5 GB, through a virtual source: the offsets beyond 4 GiB of a region at its end.
TEST(image_read, reads_a_region_beyond_4_gib)
{
    const std::vector<std::uint64_t> axes{65536, 50000};
    const fixture header = fixture().header(primary_records(16, axes));
    const std::uint64_t data = std::uint64_t{65536} * 50000 * 2;
    const std::uint64_t size = header.size() + ((data + 2879) / 2880 * 2880);
    const std::uint64_t data_offset = header.size();
    // Value e of the data is the low 16 bits of 7e + 3.
    const auto value = [](std::uint64_t element) { return static_cast<std::uint16_t>((element * 7) + 3); };
    auto source =
        std::make_unique<openfits::test::virtual_source>(header.bytes(), size, [&](std::uint64_t offset) -> std::byte {
            if (offset - data_offset >= data) {
                return std::byte{0};
            }
            const std::uint64_t element = (offset - data_offset) / 2;
            const std::uint16_t bits = value(element);
            return static_cast<std::byte>((offset - data_offset) % 2 == 0 ? bits >> 8U : bits & 0xFFU);
        });
    const reader input(std::move(source));
    const std::vector<std::int16_t> corner =
        input.read_pixels<std::int16_t>(0, {.region = region{.start = {65533, 49997}, .size = {3, 3}}});
    std::vector<std::int16_t> expected;
    for (std::uint64_t y = 49997; y < 50000; ++y) {
        for (std::uint64_t x = 65533; x < 65536; ++x) {
            expected.push_back(static_cast<std::int16_t>(value((y * 65536) + x)));
        }
    }
    EXPECT_EQ(corner, expected);
}

TEST(image_read, reports_progress_and_can_be_cancelled)
{
    const std::vector<std::uint64_t> axes{100, 100};
    const fixture file = int16_image(axes, ramp(axes));
    for (const source_kind kind : {source_kind::memory, source_kind::streamed}) {
        const reader input = open(file, kind);
        std::vector<std::pair<std::uint64_t, std::uint64_t>> calls;
        const std::vector<float> values =
            input.read_pixels<float>(0, {.progress = [&](std::uint64_t done, std::uint64_t total) {
                                         calls.emplace_back(done, total);
                                         return true;
                                     }});
        ASSERT_GE(calls.size(), 2U);
        EXPECT_EQ(calls.front(), (std::pair<std::uint64_t, std::uint64_t>{0, 20000}));
        EXPECT_EQ(calls.back(), (std::pair<std::uint64_t, std::uint64_t>{20000, 20000}));
        EXPECT_TRUE(std::ranges::is_sorted(calls));

        EXPECT_TRUE(throws<cancelled_error>(errc::cancelled, [&] {
            (void)input.read_pixels<float>(0, {.progress = [](std::uint64_t, std::uint64_t) { return false; }});
        }));
        EXPECT_TRUE(throws<cancelled_error>(errc::cancelled, [&] {
            (void)input.read_pixels<float>(0,
                                           {.progress = [](std::uint64_t done, std::uint64_t) { return done == 0; }});
        }));
    }
}

// A file cut within the data: a region within the bytes present reads, one beyond them fails.
TEST(image_read, a_truncated_image_reads_up_to_the_end_of_the_file)
{
    const std::vector<std::uint64_t> axes{100, 100};
    fixture file = fixture().header(primary_records(8, axes)).data(10000);
    file.truncate(2880 + 5000);
    for (const source_kind kind : {source_kind::memory, source_kind::streamed}) {
        const reader input = open(file, kind);
        EXPECT_EQ(input.read_pixels<std::uint8_t>(0, {.region = region{.start = {0, 0}, .size = {100, 50}}}).size(),
                  5000U);
        EXPECT_EQ(
            input.read_pixels<std::uint8_t>(0, {.region = region{.start = {3, 0}, .size = {4, 50}, .step = {1, 7}}})
                .size(),
            32U);
        EXPECT_TRUE(throws<io_error>(errc::end_of_data, [&] { (void)input.read_pixels<std::uint8_t>(0); }));
        EXPECT_TRUE(throws<io_error>(errc::end_of_data, [&] {
            (void)input.read_pixels<float>(0, {.region = region{.start = {0, 49}, .size = {1, 2}}});
        }));
    }
}

TEST(image_read, bscale_0_leaves_the_physical_values_unavailable_and_the_stored_ones_readable)
{
    const reader input = open(int16_image({3}, {1, 2, 3}, {card_record("BSCALE", "0.0")}));
    EXPECT_TRUE(throws<invalid_data_error>(errc::reserved_keyword, [&] { (void)input.read_pixels<float>(0); }));
    EXPECT_EQ(input.read_pixels<std::int16_t>(0, {.scaling = scaling_policy::raw}),
              (std::vector<std::int16_t>{1, 2, 3}));
}

TEST(image_read, reads_the_stored_values_or_those_of_a_scaling_of_the_caller)
{
    const reader input = open(int16_image({3}, {-32768, 0, 32767}, {card_record("BZERO", "32768")}));
    EXPECT_EQ(input.read_pixels<std::uint16_t>(0), (std::vector<std::uint16_t>{0, 32768, 65535}));
    EXPECT_EQ(input.read_pixels<std::int16_t>(0, {.scaling = scaling_policy::raw}),
              (std::vector<std::int16_t>{-32768, 0, 32767}));
    EXPECT_EQ(input.read_pixels<double>(
                  0, {.scaling = scaling_policy::override, .override_scaling = {.scale = 0.5, .zero = 1.0}}),
              (std::vector<double>{-16383.0, 1.0, 16384.5}));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] {
        (void)input.read_pixels<double>(0, {.scaling = scaling_policy::override, .override_scaling = {.scale = 0.0}});
    }));
}

TEST(image_read, a_value_beyond_the_destination_is_refused_or_clamped_and_counted)
{
    const reader input = open(int16_image({4}, {-5, 0, 300, 7}));
    EXPECT_TRUE(
        throws<invalid_data_error>(errc::value_out_of_range, [&] { (void)input.read_pixels<std::uint8_t>(0); }));
    std::vector<std::uint8_t> clamped(4);
    const openfits::pixel_read_result result =
        input.read_pixels(0, std::span<std::uint8_t>(clamped), {.overflow = openfits::overflow_policy::clamp});
    EXPECT_EQ(result.clamped, 2U);
    EXPECT_EQ(clamped, (std::vector<std::uint8_t>{0, 0, 255, 7}));
}

TEST(image_read, a_read_into_memory_of_the_library_is_within_max_allocation)
{
    const reader input = open(int16_image({20}, ramp({20})), source_kind::memory, {.limits = {.max_allocation = 100}});
    EXPECT_TRUE(throws<limit_error>(errc::allocation_too_large, [&] { (void)input.read_pixels<double>(0); }));
    EXPECT_EQ(input.read_pixels<std::int16_t>(0).size(), 20U);
    std::vector<double> mine(20);
    EXPECT_NO_THROW((void)input.read_pixels(0, std::span<double>(mine)));
}

// IMAGE extensions are read as the primary array is (standard §7.1.3).
TEST(image_read, reads_the_images_of_extensions)
{
    fixture file;
    file.primary();
    file.header(extension_records("IMAGE", 16, {3})).data(big_endian(std::vector<std::int16_t>{4, 5, 6}));
    file.header(with(extension_records("IMAGE", -32, {2}), {card_record("BSCALE", "2.0")}))
        .data(big_endian(std::vector<float>{1.5F, -2.0F}));
    const reader input = open(file);
    EXPECT_EQ(input.read_pixels<std::int32_t>(1), (std::vector<std::int32_t>{4, 5, 6}));
    EXPECT_EQ(input.read_pixels<float>(2), (std::vector<float>{3.0F, -4.0F}));
    EXPECT_EQ(input.image(2).value_type(), numeric_type::float32);
}

} // namespace
