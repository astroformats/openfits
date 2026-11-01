// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The images of the samples of other software: each decodes to what the catalogue says it holds, within one unit of the
// last place of the format it is stored in, and to what astropy reads of it, value for value (the .pixels file next to
// each sample, which tests/data/scripts/dump_pixels.py writes).

#include <openfits/image.h>
#include <openfits/reader.h>
#include <openfits/types.h>

#include "samples/sample_catalog.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using openfits::image_info;
using openfits::numeric_type;
using openfits::reader;
using openfits::test::image_content;
using openfits::test::sample;
using openfits::test::sample_unit;

constexpr std::uint64_t width = 37;
constexpr std::uint64_t height = 23;

class samples_images : public testing::TestWithParam<sample>
{};

// value as To, without a cast where it is of that type, which GCC reports as useless.
template <typename To, typename From> To as(From value)
{
    if constexpr (std::same_as<To, From>) {
        return value;
    } else {
        return static_cast<To>(value);
    }
}

// The integer nearest numerator x maximum / denominator, halves up, exactly, without the product, which can exceed
// 64 bits: numerator x (maximum / denominator) + the rounded share of the remainder.
std::uint64_t scaled_exactly(std::uint64_t numerator, std::uint64_t denominator, std::uint64_t maximum)
{
    const std::uint64_t whole = maximum / denominator;
    const std::uint64_t rest = maximum % denominator;
    return (numerator * whole) + (((2 * numerator * rest) + denominator) / (2 * denominator));
}

// The share of the range of a type that element e of the pattern holds, as a fraction numerator / denominator: plane 0
// (x + 37 y) / 850, plane 1 (850 - x - 37 y) / 850, plane 2 x / 36.
std::pair<std::uint64_t, std::uint64_t> share_of(std::uint64_t element)
{
    const std::uint64_t pixel = element % (width * height);
    switch (element / (width * height)) {
    case 0:
        return {pixel, 850};
    case 1:
        return {850 - pixel, 850};
    default:
        return {pixel % width, 36};
    }
}

// The value of the pattern at element e in the range of the integer type T: that of the unsigned type of its size,
// minus half that range for a signed type.
template <typename T> T integer_pattern(std::uint64_t element)
{
    using unsigned_type = std::make_unsigned_t<T>;
    const auto [numerator, denominator] = share_of(element);
    const std::uint64_t value = scaled_exactly(numerator, denominator, std::numeric_limits<unsigned_type>::max());
    if constexpr (std::is_signed_v<T>) {
        // value - 2^(n - 1), in unsigned arithmetic, which wraps into the signed range.
        const std::uint64_t half = std::uint64_t{1} << (sizeof(T) * 8 - 1);
        return as<T>(static_cast<std::int64_t>(value - half));
    } else {
        return as<T>(value);
    }
}

double real_pattern(std::uint64_t element)
{
    const auto [numerator, denominator] = share_of(element);
    return static_cast<double>(numerator) / static_cast<double>(denominator);
}

// The value that element e of a unit holds, as a double, and the tolerance of the comparison.
template <typename T> void check_values(const sample_unit& expected, const std::vector<T>& values)
{
    for (std::size_t e = 0; e < values.size(); ++e) {
        const T value = values[e];
        const auto special = std::ranges::find(expected.specials, e, &openfits::test::special_value::index);
        if (special != expected.specials.end()) {
            if (std::isnan(special->value)) {
                EXPECT_TRUE(std::isnan(as<double>(value))) << "element " << e;
            } else {
                EXPECT_EQ(as<double>(value), special->value) << "element " << e;
                EXPECT_EQ(std::signbit(as<double>(value)), std::signbit(special->value)) << "element " << e;
            }
            continue;
        }
        switch (expected.content) {
        case image_content::pattern:
            if constexpr (std::floating_point<T>) {
                const double tolerance = std::same_as<T, float> ? 1e-6 : 1e-12;
                EXPECT_NEAR(as<double>(value), real_pattern(e), tolerance) << "element " << e;
            } else {
                const T exact = integer_pattern<T>(e);
                const auto difference = value > exact ? value - exact : exact - value;
                EXPECT_LE(difference, T{1}) << "element " << e << ": " << +value << " for " << +exact;
            }
            break;
        case image_content::scaled_pattern:
            EXPECT_NEAR(as<double>(value), 100.0 + (0.01 * static_cast<double>(integer_pattern<std::int16_t>(e))), 1e-4)
                << "element " << e;
            break;
        case image_content::index:
            EXPECT_EQ(as<double>(value), static_cast<double>(e)) << "element " << e;
            break;
        case image_content::mask:
            EXPECT_EQ(as<double>(value), static_cast<double>(e % 256)) << "element " << e;
            break;
        case image_content::none:
            break;
        }
    }
}

template <typename T> void check_unit(const reader& input, std::size_t index, const sample_unit& expected)
{
    const std::vector<T> values = input.read_pixels<T>(index);
    std::uint64_t count = 1;
    for (const std::uint64_t axis : expected.axes) {
        count *= axis;
    }
    ASSERT_EQ(values.size(), count);
    check_values(expected, values);
}

void check_unit(const reader& input, std::size_t index, const sample_unit& expected)
{
    switch (expected.value_type) {
    case numeric_type::int8:
        check_unit<std::int8_t>(input, index, expected);
        return;
    case numeric_type::uint8:
        check_unit<std::uint8_t>(input, index, expected);
        return;
    case numeric_type::int16:
        check_unit<std::int16_t>(input, index, expected);
        return;
    case numeric_type::uint16:
        check_unit<std::uint16_t>(input, index, expected);
        return;
    case numeric_type::int32:
        check_unit<std::int32_t>(input, index, expected);
        return;
    case numeric_type::uint32:
        check_unit<std::uint32_t>(input, index, expected);
        return;
    case numeric_type::int64:
        check_unit<std::int64_t>(input, index, expected);
        return;
    case numeric_type::uint64:
        check_unit<std::uint64_t>(input, index, expected);
        return;
    case numeric_type::float32:
        check_unit<float>(input, index, expected);
        return;
    case numeric_type::float64:
        check_unit<double>(input, index, expected);
        return;
    }
}

TEST_P(samples_images, decode_to_what_the_catalogue_says_they_hold)
{
    const sample& entry = GetParam();
    const reader input(openfits::test::sample_path(entry));
    for (std::size_t i = 0; i < entry.units.size(); ++i) {
        const sample_unit& expected = entry.units[i];
        if (expected.content == image_content::none) {
            continue;
        }
        SCOPED_TRACE(testing::Message() << "unit " << i);
        const image_info image = input.image(i);
        EXPECT_EQ(image.geometry.axes, expected.axes);
        EXPECT_EQ(image.value_type(), expected.value_type);
        check_unit(input, i, expected);
    }
}

// The values of each image unit as astropy reads them: its index, the type of its values, and their text.
struct astropy_image
{
    std::string type{};
    std::vector<std::string> values{};
};

std::map<std::size_t, astropy_image> astropy_pixels(const sample& entry)
{
    std::string path = openfits::test::sample_path(entry);
    path.replace(path.rfind(".fit"), path.size(), ".pixels");
    std::ifstream file(path, std::ios::binary);
    std::map<std::size_t, astropy_image> images;
    astropy_image* current = nullptr;
    for (std::string line; std::getline(file, line);) {
        const std::size_t tab = line.find('\t');
        if (tab != std::string::npos) {
            const std::size_t second = line.find('\t', tab + 1);
            current = &images[std::stoul(line.substr(0, tab))];
            current->type = line.substr(tab + 1, second - tab - 1);
            current->values.reserve(std::stoul(line.substr(second + 1)));
        } else if (current != nullptr) {
            current->values.push_back(line);
        }
    }
    return images;
}

double real_of(const std::string& text)
{
    if (text == "nan") {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (text == "inf" || text == "-inf") {
        return text.front() == '-' ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
    }
    // std::strtod() in the C locale of the tests: the std::from_chars() of doubles is missing from libstdc++ 11 on
    // MinGW.
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    EXPECT_EQ(end, text.c_str() + text.size()) << text;
    return value;
}

// scaled tells an image whose physical values astropy computes in float, and zero is its BZERO.
template <typename T>
void compare_with_astropy(const reader& input, std::size_t index, const astropy_image& theirs, bool scaled, double zero)
{
    const std::vector<T> ours = input.read_pixels<T>(index);
    ASSERT_EQ(ours.size(), theirs.values.size());
    for (std::size_t e = 0; e < ours.size(); ++e) {
        const std::string& text = theirs.values[e];
        if constexpr (std::floating_point<T>) {
            const double value = real_of(text);
            if (std::isnan(value)) {
                EXPECT_TRUE(std::isnan(as<double>(ours[e]))) << "element " << e;
            } else if (scaled) {
                // astropy scales in float, the library in double and rounds the result once: they part by the last
                // places of a float of the size of the terms, BZERO among them, which a value near 0 cancels.
                EXPECT_NEAR(as<double>(ours[e]), value, 1e-6 * (std::abs(value) + std::abs(zero) + 1.0))
                    << "element " << e;
            } else {
                EXPECT_EQ(as<double>(ours[e]), value) << "element " << e;
                EXPECT_EQ(std::signbit(as<double>(ours[e])), std::signbit(value)) << "element " << e;
            }
        } else if constexpr (std::is_signed_v<T>) {
            EXPECT_EQ(as<std::int64_t>(ours[e]), std::stoll(text)) << "element " << e;
        } else {
            EXPECT_EQ(as<std::uint64_t>(ours[e]), std::stoull(text)) << "element " << e;
        }
    }
}

// The numpy type of the values that astropy gives, as the type of the library that holds them.
TEST_P(samples_images, decode_to_what_astropy_reads)
{
    const sample& entry = GetParam();
    const reader input(openfits::test::sample_path(entry));
    const std::map<std::size_t, astropy_image> images = astropy_pixels(entry);
    for (const auto& [index, theirs] : images) {
        SCOPED_TRACE(testing::Message() << "unit " << index << " of " << theirs.type);
        const image_info image = input.image(index);
        const bool scaled = image.scaling != openfits::scaling{} && image.value_type() >= numeric_type::float32;
        const std::map<std::string, numeric_type> types{
            {"int8", numeric_type::int8},      {"uint8", numeric_type::uint8},   {"int16", numeric_type::int16},
            {"uint16", numeric_type::uint16},  {"int32", numeric_type::int32},   {"uint32", numeric_type::uint32},
            {"int64", numeric_type::int64},    {"uint64", numeric_type::uint64}, {"float32", numeric_type::float32},
            {"float64", numeric_type::float64}};
        const auto found = types.find(theirs.type);
        ASSERT_NE(found, types.end()) << theirs.type;
        // astropy gives each image in the type that its value_type() is.
        EXPECT_EQ(image.value_type(), found->second);
        switch (found->second) {
        case numeric_type::int8:
            compare_with_astropy<std::int8_t>(input, index, theirs, scaled, image.scaling.zero);
            break;
        case numeric_type::uint8:
            compare_with_astropy<std::uint8_t>(input, index, theirs, scaled, image.scaling.zero);
            break;
        case numeric_type::int16:
            compare_with_astropy<std::int16_t>(input, index, theirs, scaled, image.scaling.zero);
            break;
        case numeric_type::uint16:
            compare_with_astropy<std::uint16_t>(input, index, theirs, scaled, image.scaling.zero);
            break;
        case numeric_type::int32:
            compare_with_astropy<std::int32_t>(input, index, theirs, scaled, image.scaling.zero);
            break;
        case numeric_type::uint32:
            compare_with_astropy<std::uint32_t>(input, index, theirs, scaled, image.scaling.zero);
            break;
        case numeric_type::int64:
            compare_with_astropy<std::int64_t>(input, index, theirs, scaled, image.scaling.zero);
            break;
        case numeric_type::uint64:
            compare_with_astropy<std::uint64_t>(input, index, theirs, scaled, image.scaling.zero);
            break;
        case numeric_type::float32:
            compare_with_astropy<float>(input, index, theirs, scaled, image.scaling.zero);
            break;
        case numeric_type::float64:
            compare_with_astropy<double>(input, index, theirs, scaled, image.scaling.zero);
            break;
        }
    }
}

std::vector<sample> image_samples()
{
    std::vector<sample> images;
    for (const sample& entry : openfits::test::all_samples()) {
        const bool has_image = std::ranges::any_of(
            entry.units, [](const sample_unit& unit) { return unit.content != image_content::none; });
        if (has_image) {
            images.push_back(entry);
        }
    }
    return images;
}

INSTANTIATE_TEST_SUITE_P(catalogue, samples_images, testing::ValuesIn(image_samples()),
                         [](const testing::TestParamInfo<sample>& instance) { return std::string(instance.param.id); });

} // namespace
