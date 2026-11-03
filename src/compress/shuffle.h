// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <cstddef>
#include <span>

// The byte shuffle of GZIP_2 (standard §10.4.2): n values of s bytes each are stored as s runs of n bytes, the most
// significant byte of every big-endian value first, then the next byte of every value, and so on. The bytes after the
// last whole value follow unchanged.

namespace openfits::detail {

/// Writes the shuffled bytes of input to output, which has the same size and does not overlap it. A value size of 0 or
/// 1, or one larger than the input, copies the input unchanged.
/// @throws usage_error (errc::invalid_argument) when the sizes differ.
void shuffle_bytes(std::span<const std::byte> input, std::span<std::byte> output, std::size_t value_size);

/// Reverses shuffle_bytes(): writes the unshuffled bytes of input to output, under the same conditions.
void unshuffle_bytes(std::span<const std::byte> input, std::span<std::byte> output, std::size_t value_size);

} // namespace openfits::detail
