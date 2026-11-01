// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "hdu/section.h"

#include <openfits/error.h>

#include <algorithm>
#include <limits>
#include <string>

namespace openfits::detail {

std::optional<std::vector<std::uint64_t>> image_axes(const hdu_info& unit)
{
    const std::string root = unit.kind() == hdu_kind::compressed_image ? "ZNAXIS" : "NAXIS";
    try {
        const auto naxis = unit.header().value<std::uint64_t>(root);
        if (naxis == 0 || naxis > 999) {
            return std::nullopt;
        }
        std::vector<std::uint64_t> axes = unit.header().values<std::uint64_t>(root, 1, naxis);
        if (std::ranges::find(axes, std::uint64_t{0}) != axes.end()) {
            return std::nullopt;
        }
        return axes;
    } catch (const invalid_data_error&) {
        return std::nullopt;
    }
}

region section_region(const hdu_info& unit, std::span<const section_range> ranges)
{
    const std::optional<std::vector<std::uint64_t>> axes = image_axes(unit);
    const error_context where{.hdu = unit.index()};
    if (!axes) {
        throw usage_error(
            errc::invalid_file_specification,
            "the image section applies to unit " + std::to_string(unit.index()) + ", which holds no image", where);
    }
    if (ranges.size() > axes->size()) {
        throw usage_error(errc::invalid_file_specification,
                          "the image section has " + std::to_string(ranges.size()) + " axes, and the image of unit " +
                              std::to_string(unit.index()) + " " + std::to_string(axes->size()),
                          where);
    }
    region result;
    for (std::size_t i = 0; i < axes->size(); ++i) {
        const std::uint64_t length = (*axes)[i];
        const section_range range = i < ranges.size() ? ranges[i] : section_range{};
        // Nothing stands for the last pixel of the axis.
        const std::uint64_t first = range.first.value_or(length);
        const std::uint64_t last = range.last.value_or(length);
        if (first == 0 || last == 0 || first > length || last > length || range.step == 0 ||
            range.step > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            throw usage_error(errc::invalid_file_specification,
                              "the range " + std::to_string(first) + ":" + std::to_string(last) + " of axis " +
                                  std::to_string(i + 1) + " of the image section does not lie within the " +
                                  std::to_string(length) + " pixels of the axis",
                              where);
        }
        result.start.push_back(std::min(first, last) - 1);
        result.size.push_back((first < last ? last - first : first - last) + 1);
        const auto step = static_cast<std::int64_t>(range.step);
        result.step.push_back(first <= last ? step : -step);
    }
    return result;
}

} // namespace openfits::detail
