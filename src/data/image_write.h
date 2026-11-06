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
#include <optional>
#include <span>
#include <vector>

// The writes of images (standard §3.3.2, §7.1): the model of an image as the writer keeps it, the structural keywords
// of its header, and its data in big-endian order, filled with zeros to the end of their last block.

namespace openfits::detail {

/// The most bytes of data that a write converts at once, in its staging chunk: 1 MiB, with which the frame of a camera
/// writes as fast as with 4 MiB and an image of 1024 x 1024 pixels 1.6 times faster, for the page faults of a chunk of
/// megabytes that the heap of Windows takes from the system at each save; with a chunk of the staging size, small
/// images write faster still, but the frames of 16-bit values a quarter slower.
inline constexpr std::size_t write_transfer = std::size_t{1} << 20;

/// The most bytes of a run of a streamed image: below the size from which the heap of Windows takes fresh pages from
/// the system for each allocation, which the chunks of every save would pay, and in the cache of a core between the
/// function that fills a chunk and the conversion that reads it.
inline constexpr std::size_t stream_transfer = std::size_t{256} << 10;

/// The model of the image of unit hdu as the writer keeps it: info with a BITPIX, that of type with the conventions of
/// standard §5.2.5 when info has none; values is the number of values given, which must be that of the axes, or nothing
/// for an image whose values come from its axes, streamed or reserved. A reserved image has no type, and info must give
/// its BITPIX.
/// @throws usage_error and validation_error as writer::add_image() does: validation_error (errc::invalid_structure,
///         NAXIS) for axes beyond 999, or data beyond 2^64 bytes.
[[nodiscard]] image_info resolve_image(image_info info, std::optional<numeric_type> type,
                                       std::optional<std::uint64_t> values, std::uint64_t hdu);

/// The values of an image as a write takes them, in order: count values of the type of the image from the element
/// first on, as their bytes in the byte order of the host, which stay valid until the next call.
using image_values = std::function<std::span<const std::byte>(std::uint64_t first, std::size_t count)>;

/// The values of data, of type, in memory: subspans of it.
[[nodiscard]] image_values borrowed_values(std::span<const std::byte> data, numeric_type type);

/// The values that fill gives, of type, in a chunk of memory of the largest that a write asks for, which the values
/// share.
[[nodiscard]] image_values streamed_values(const std::function<void(std::uint64_t, std::span<std::byte>)>& fill,
                                           numeric_type type);

/// The values of a run of an image from the element first, count of them, that a write converts at once: at most
/// transfer bytes of the larger of the value size and the stored size, whole rows of row elements when one fits, and
/// at least one value.
[[nodiscard]] std::uint64_t run_length(std::uint64_t first, std::uint64_t count, std::uint64_t row,
                                       std::size_t value_size, std::size_t transfer = write_transfer) noexcept;

/// The BITPIX of a model that resolve_image() gives, which has one.
/// @throws usage_error (errc::invalid_argument) for a model without one.
[[nodiscard]] bitpix model_bitpix(const image_info& image);

/// The structural cards of a model that resolve_image() gives: those of the primary HDU or of an IMAGE extension, then
/// BSCALE, BZERO and BLANK when they are not the defaults.
[[nodiscard]] std::vector<card> image_cards(const image_info& image, bool primary);

/// The bytes of the data of an image of BITPIX stored and count values, with the zeros of their last block.
[[nodiscard]] std::uint64_t padded_data_size(bitpix stored, std::uint64_t count) noexcept;

/// Writes count values of type that values gives into sink, as conversion stores them, in runs of run_length() with
/// rows of row elements, through a staging chunk, then zeros to the end of their last block. advance receives the bytes
/// of each write.
/// @throws the exceptions of convert_write(), of values and of the sink, and those of advance.
void write_image_data(output_sink& sink, const write_conversion& conversion, numeric_type type, std::uint64_t count,
                      std::uint64_t row, const image_values& values, const std::function<void(std::uint64_t)>& advance,
                      std::size_t transfer = write_transfer);

/// The data of a reserved image of count values of BITPIX stored: zeros, through sink.write_zeros(), or each value the
/// undefined one when undefined_values: blank in integer data, a NaN in floating-point data. Then zeros to the end of
/// the last block. advance receives the bytes of each write.
/// @throws the exceptions of the sink and of advance.
void write_reserved_data(output_sink& sink, bitpix stored, std::uint64_t count, bool undefined_values,
                         std::optional<std::int64_t> blank, const std::function<void(std::uint64_t)>& advance,
                         std::size_t transfer = write_transfer);

} // namespace openfits::detail
