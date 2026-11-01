// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/hdu.h>
#include <openfits/types.h>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The images of units and the sections of file specifications.

namespace openfits::detail {

/// The axes of the image of a unit, NAXISn, or ZNAXISn for a compressed image; nothing when the unit holds no image,
/// for NAXIS = 0 or an axis of length 0, or when the axes cannot be read.
[[nodiscard]] std::optional<std::vector<std::uint64_t>> image_axes(const hdu_info& unit);

/// The region of the image of unit that the ranges of an image section select, the axes after the last range whole.
/// @throws usage_error (errc::invalid_file_specification) when the unit holds no image, or the ranges are more than
///         its axes or lie beyond them.
[[nodiscard]] region section_region(const hdu_info& unit, std::span<const section_range> ranges);

} // namespace openfits::detail
