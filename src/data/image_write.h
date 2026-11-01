// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/card.h>
#include <openfits/image.h>
#include <openfits/io.h>
#include <openfits/types.h>

#include "data/conversion.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

// The writes of images (standard §3.3.2, §7.1): the model of an image as the writer keeps it, the structural keywords
// of its header, and its data in big-endian order, filled with zeros to the end of their last block.

namespace openfits::detail {

/// The most bytes of data that a write converts at once, in its staging chunk.
inline constexpr std::size_t write_transfer = std::size_t{4} << 20;

/// The model of the image of unit hdu as the writer keeps it, for values of type: info with a BITPIX, that of type
/// with the conventions of standard §5.2.5 when info has none.
/// @throws usage_error and validation_error as writer::add_image() does.
[[nodiscard]] image_info resolve_image(image_info info, numeric_type type, std::uint64_t values, std::uint64_t hdu);

/// The BITPIX of a model that resolve_image() gives, which has one.
/// @throws usage_error (errc::invalid_argument) for a model without one.
[[nodiscard]] bitpix model_bitpix(const image_info& image);

/// The structural cards of a model that resolve_image() gives: those of the primary HDU or of an IMAGE extension, then
/// BSCALE, BZERO and BLANK when they are not the defaults.
[[nodiscard]] std::vector<card> image_cards(const image_info& image, bool primary);

/// The bytes of the data of an image of BITPIX stored and count values, with the zeros of their last block.
[[nodiscard]] std::uint64_t padded_data_size(bitpix stored, std::uint64_t count) noexcept;

/// Writes the values of data, of type, into sink as conversion stores them, through a staging chunk of at most transfer
/// bytes, then zeros to the end of their last block. advance receives the bytes of each write.
/// @throws the exceptions of convert_write() and of the sink, and those of advance.
void write_image_data(output_sink& sink, const write_conversion& conversion, numeric_type type,
                      std::span<const std::byte> data, const std::function<void(std::uint64_t)>& advance,
                      std::size_t transfer = write_transfer);

} // namespace openfits::detail
