// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// PLIO_1 (standard §10.4.3): the line lists of IRAF's pixel lists, 16-bit instructions that zero pixels, repeat the
// current high value and change it (Table 38), after a header of seven words. A tile is one line, its pixels in the
// order of the data. The lists are those of D. Tody's encoder as CFITSIO writes them, word for word, but where a run of
// zeros of a multiple of 4095 pixels comes before a single pixel: CFITSIO's encoder then writes a word that its
// decoder, and every other, reads as one pixel without the last 4095 zeros, so that the encoder of this library writes
// that pixel in a word of its own.
//
// The values are integers from 0 to 2^24 - 1. A decoder reads every list that CFITSIO writes, the defective word
// included as CFITSIO reads it, and the old header of three words; it never reads beyond its input, writes exactly its
// output, the pixels that the list leaves out as 0 as CFITSIO does, and throws integrity_error with
// errc::corrupt_compressed_data for a list that is malformed or ends early; messages have no context, which the caller
// adds.

namespace openfits::detail {

/// The largest value that PLIO_1 stores: 2^24 - 1 (standard §10.4.3).
inline constexpr std::int32_t plio_max_value = (1 << 24) - 1;

/// The most pixels of a tile that plio_encode() takes: its list must have fewer than 2^30 words, whose number the
/// header holds in two words of 15 bits.
inline constexpr std::uint64_t plio_max_pixels = ((std::uint64_t{1} << 30) - 8) / 3;

/// The most words of the list of count values, the worst case of the instruction set: the header, and three words for
/// each value, so that an encoder needs no larger buffer.
[[nodiscard]] std::uint64_t plio_max_words(std::uint64_t count) noexcept;

/// Appends the line list of values to output, each word in the byte order of the host.
/// @throws usage_error (errc::invalid_argument) for a value below 0 or above plio_max_value, or more values than
///         plio_max_pixels.
void plio_encode(std::span<const std::int32_t> values, std::vector<std::int16_t>& output);

/// Decodes the line list input, words in the byte order of the host, into the output.size() values of output. Words
/// after the end of the list that its header gives are ignored.
void plio_decode(std::span<const std::int16_t> input, std::span<std::int32_t> output);

} // namespace openfits::detail
