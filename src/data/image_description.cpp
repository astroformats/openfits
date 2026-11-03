// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "data/image_description.h"

#include <openfits/error.h>

#include "compress/image_tiles.h"
#include "data/conversion.h"

#include <cstdint>
#include <string>
#include <vector>

namespace openfits::detail {

image_info describe_image(const hdu_info& unit, bool with_header)
{
    const error_context where{.hdu = unit.index()};
    switch (unit.kind()) {
    case hdu_kind::primary:
    case hdu_kind::image:
        break;
    case hdu_kind::compressed_image:
        return describe_compressed_image(unit, with_header).image;
    case hdu_kind::table:
    case hdu_kind::bintable:
    case hdu_kind::compressed_table:
    case hdu_kind::random_groups:
    case hdu_kind::unknown:
        throw usage_error(errc::not_an_image, "unit " + std::to_string(unit.index()) + " holds no image", where);
    }
    return describe_array(unit, with_header);
}

image_info describe_array(const hdu_info& unit, bool with_header)
{
    const header& cards = unit.header();
    const auto value = cards.value<std::int64_t>("BITPIX");
    const std::optional<openfits::bitpix> stored = bitpix_of(value);
    if (!stored) {
        throw invalid_data_error(errc::invalid_structure,
                                 "BITPIX is " + std::to_string(value) + ", which is not a value of standard §4.4.1.1",
                                 {.hdu = unit.index(), .keyword = "BITPIX"});
    }
    image_info image;
    image.bitpix = stored;
    const auto naxis = cards.value<std::uint64_t>("NAXIS");
    if (naxis > 999) {
        throw invalid_data_error(errc::invalid_structure,
                                 "NAXIS is " + std::to_string(naxis) + ", beyond the 999 of standard §4.4.1.1",
                                 {.hdu = unit.index(), .keyword = "NAXIS"});
    }
    image.geometry.axes = cards.values<std::uint64_t>("NAXIS", 1, naxis);
    image.scaling.scale = cards.find<double>("BSCALE").value_or(1.0);
    image.scaling.zero = cards.find<double>("BZERO").value_or(0.0);
    if (static_cast<int>(*stored) > 0) {
        image.blank = cards.find<std::int64_t>("BLANK");
    }
    if (with_header) {
        image.header = cards;
    }
    return image;
}

void throw_unavailable(const hdu_info& unit)
{
    for (const diagnostic& found : unit.diagnostics()) {
        if (found.severity == severity::error) {
            throw invalid_data_error(found.code, found.message, found.context);
        }
    }
    throw invalid_data_error(errc::invalid_structure,
                             "the header of unit " + std::to_string(unit.index()) + " gives no size of its data",
                             {.hdu = unit.index()});
}

} // namespace openfits::detail
