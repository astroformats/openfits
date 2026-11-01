// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/error.h>
#include <openfits/types.h>

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace openfits::test {

/// What the values of the image of a unit hold, which the tests compute without the reader.
enum class image_content
{
    none,           ///< Nothing that the tests compute: no image, or data such as an ICC profile.
    pattern,        ///< The pattern image: (x + 37 y) / 850 in a plane of 37 x 23 pixels, then 1 - (x + 37 y) / 850 and
                    ///< x / 36 in a second and a third plane, in the range of the type of the values.
    scaled_pattern, ///< The pattern of 16-bit signed integers, scaled by BSCALE 0.01 and BZERO 100.
    index,          ///< Each value is its index in the data.
    mask,           ///< (x + 11 y) mod 256, in an image 11 pixels wide.
};

/// A value of an image that its content does not give: NaN for an undefined value, an infinity, or a negative zero.
struct special_value
{
    std::uint64_t index = 0;
    double value = 0.0;
};

/// A header and data unit of a sample, as its producer wrote it.
struct sample_unit
{
    hdu_kind kind = hdu_kind::primary;
    /// EXTNAME, or else HDUNAME; empty without either.
    std::string_view name{};
    std::uint64_t data_size = 0;
    /// What its image holds.
    image_content content = image_content::none;
    /// The type of the values of its image, image_info::value_type(), and its axes.
    numeric_type value_type = numeric_type::uint8;
    std::vector<std::uint64_t> axes{};
    /// The values that its content does not give.
    std::vector<special_value> specials{};
};

/// A sample file of tests/data, written by other software, and what is known about it independently of the reader.
struct sample
{
    /// The id of the sample: A4, D1, E7a, ...
    std::string_view id{};
    /// The path of the file under tests/data.
    std::string_view file{};
    /// What the file holds.
    std::string_view content{};
    /// The program and its version that wrote it.
    std::string_view producer{};
    /// The script or the command that made it.
    std::string_view procedure{};
    /// Values set by hand, if any.
    std::string_view set_by_hand{};
    /// Its units, in order.
    std::vector<sample_unit> units{};
    /// The codes of the departures from the standard that the reader reports in its headers, each a warning or an
    /// info; any other diagnostic is a failure.
    std::vector<errc> departures{};
};

/// Every sample.
[[nodiscard]] const std::vector<sample>& all_samples();

/// The sample with the given id. Throws std::logic_error when there is none.
[[nodiscard]] const sample& sample_by_id(std::string_view id);

/// The UTF-8 path of the file of a sample.
[[nodiscard]] std::string sample_path(const sample& unit);

/// GoogleTest prints the parameter in messages, and CTest names the test after it: the id. The name is fixed by
/// GoogleTest.
// NOLINTNEXTLINE(readability-identifier-naming)
void PrintTo(const sample& unit, std::ostream* output);

} // namespace openfits::test
