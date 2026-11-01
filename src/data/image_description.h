// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/hdu.h>
#include <openfits/image.h>

// The image of a unit from the keywords of its header (standard §4.4.1, §4.4.2.5), and the error of a unit whose data
// are unavailable.

namespace openfits::detail {

/// The image of a primary HDU or an IMAGE extension: BITPIX, NAXISn, BSCALE and BZERO with their defaults, BLANK for
/// integer data, and the header unless with_header is false, as for a read, which needs no copy of it. A BLANK of
/// floating-point data is left out, as the walk reports it.
/// @throws usage_error (errc::not_an_image) for a unit of another kind; unsupported_error
///         (errc::unsupported_compression) for a compressed image; invalid_data_error when BITPIX is not a value of
///         standard §4.4.1.1, or NAXIS, an NAXISn, BSCALE, BZERO or BLANK does not convert as the description needs.
[[nodiscard]] image_info describe_image(const hdu_info& unit, bool with_header = true);

/// Throws the error that leaves the data of unit unavailable: the first error among the diagnostics of its header, as
/// invalid_data_error, or a generic one.
[[noreturn]] void throw_unavailable(const hdu_info& unit);

} // namespace openfits::detail
