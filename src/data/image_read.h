// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/hdu.h>
#include <openfits/image.h>
#include <openfits/reader.h>
#include <openfits/types.h>

#include "data/conversion.h"
#include "io/thread_safe_source.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The reads of the values of images (standard §3.3.2, §7.1): a region of the array, from the data of a unit, converted
// into the type of the application, in one pass over the stored bytes.

namespace openfits::detail {

/// The most bytes that a read of values takes from the source at once: the staging chunk that values are converted
/// from, and the pieces of a read into the destination itself between the calls of the progress function.
inline constexpr std::size_t pixel_transfer = std::size_t{4} << 20;

/// One axis of a region: the number of its elements, the index of the first, and the step from one to the next,
/// negative for an axis read in reverse.
struct axis_selection
{
    std::uint64_t first = 0;
    std::uint64_t count = 0;
    std::int64_t step = 1;
};

/// A region on every axis of an array, and its number of elements.
struct region_selection
{
    std::vector<axis_selection> axes{};
    std::uint64_t elements = 0;
};

/// The selection of a region of an array of the given axes, or of the whole array when region is empty. An empty start
/// of the region is 0 on every axis, and an empty size the rest of each axis.
/// @throws usage_error (errc::invalid_argument) when the region does not lie within the array: a start, a size or a
///         step that has entries but not one for each axis, a start beyond an axis, a size of 0 or beyond the end of an
///         axis, a step of 0.
[[nodiscard]] region_selection select_region(const std::optional<region>& value, const geometry& array);

/// The elements of a destination that the values of selection span, in rows pitch elements apart (0 for rows that
/// follow each other): from the first element of the first row to the last of the last.
/// @throws usage_error (errc::invalid_argument) when pitch is shorter than a row, and (errc::arithmetic_overflow)
///         when the span does not fit in 64 bits.
[[nodiscard]] std::uint64_t destination_span(const region_selection& selection, std::uint64_t pitch);

/// A read of values, once checked: its region, and the conversion of its values, none when the region has no element.
struct pixel_read_plan
{
    region_selection selection{};
    std::optional<read_conversion> conversion{};
};

/// Checks a read of the values of image, the image of unit, into destination as values of type, and prepares the
/// conversion of its values: the exceptions of reader::read_pixels() for its arguments, for a unit whose header gives
/// no size of its data, and for its scaling.
[[nodiscard]] pixel_read_plan plan_pixel_read(const hdu_info& unit, const image_info& image, numeric_type type,
                                              std::span<std::byte> destination, const pixel_read_options& options);

/// Reads the values of image, the image of unit, that options select from source into destination, as values of
/// type, with at most transfer bytes taken from the source at once: the exceptions of reader::read_pixels(). The array
/// starts array_offset bytes after the start of the data of the unit, as that of a random group does.
pixel_read_result read_pixels(const thread_safe_source& source, const hdu_info& unit, const image_info& image,
                              numeric_type type, std::span<std::byte> destination, const pixel_read_options& options,
                              std::size_t transfer = pixel_transfer, std::uint64_t array_offset = 0);

} // namespace openfits::detail
