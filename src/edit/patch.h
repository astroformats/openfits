// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/types.h>

#include "data/conversion.h"
#include "data/image_read.h"
#include "edit/overlay.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

// The writes of the editor into the data of units, kept until a commit: the runs of the elements that each changes,
// with their bytes as the file stores them, laid over the data when the commit writes them.

namespace openfits::detail {

/// A run of the elements that a write changes in the data of a unit: count elements of width bytes, the first at offset
/// from the start of the data, and each next one stride bytes after the one before it.
struct patch_run
{
    std::uint64_t offset = 0;
    std::uint64_t count = 0;
    std::uint64_t width = 0;
    std::uint64_t stride = 0;

    /// The offset after the last byte of the run.
    [[nodiscard]] std::uint64_t end() const noexcept
    {
        return count == 0 ? offset : offset + ((count - 1) * stride) + width;
    }
};

/// A write into the data of a unit: runs in increasing order of their offsets, which do not overlap, and the bytes of
/// their elements, big-endian as the file stores them, the elements of each run one after the other in the order of
/// their offsets.
struct data_patch
{
    std::vector<patch_run> runs{};
    std::vector<std::byte> bytes{};
};

/// The patch of values of type, in the byte order of the host, written into the region of selection of an array of the
/// axes, as conversion stores them: the values in the order of the region, the first axis varying fastest, an axis of
/// a negative step in reverse, in rows pitch values apart (0 for rows that follow each other), as a read of the region
/// gives them. most is the most bytes that the stored values may take (limits::max_allocation, 0 for no limit).
/// @throws limit_error (errc::allocation_too_large) beyond most, and the exceptions of convert_write().
[[nodiscard]] data_patch region_patch(const region_selection& selection, std::span<const std::uint64_t> axes,
                                      std::uint64_t pitch, const write_conversion& conversion, numeric_type type,
                                      std::span<const std::byte> values, std::uint64_t most);

/// The patch of the undefined value of data of BITPIX stored written into every element of the region of selection of
/// an array of the axes: blank in integer data, a quiet NaN in floating-point data (standard §4.4.2.5, §5.3).
/// @throws limit_error (errc::allocation_too_large) beyond most.
[[nodiscard]] data_patch blank_patch(const region_selection& selection, std::span<const std::uint64_t> axes,
                                     bitpix stored, std::int64_t blank, std::uint64_t most);

/// The patch of bytes written at offset.
[[nodiscard]] data_patch raw_patch(std::uint64_t offset, std::span<const std::byte> bytes);

/// Fills destination with the bytes of the data of a unit from offset on, as the file holds them: the bytes beyond its
/// end as zeros.
using data_reader = std::function<void(std::uint64_t offset, std::span<std::byte> destination)>;

/// The sums of standard §4.4.2.7 of the bytes that patches changed, before and after, each byte at its offset in the
/// data, so that the sum of the data records after them is the sum before them, less before, plus after (standard
/// Appendix J.4).
struct sum_change
{
    std::uint32_t before = 0;
    std::uint32_t after = 0;
};

/// The sum of bytes that lie at offset in the data of a unit, among the 32-bit integers of standard §4.4.2.7.
[[nodiscard]] std::uint32_t sum_at(std::span<const std::byte> bytes, std::uint64_t offset) noexcept;

/// Writes the patch over overlay, the runs in their order: a run whose elements follow each other as it is, and one
/// whose elements lie apart with the bytes between them as the data hold them, read through read and overlay. With
/// change, the bytes of each run are read before it is written, and their sums added to change. read is called for
/// runs that lie less than 64 KiB apart at once, in pieces of at most transfer bytes.
/// @throws the exceptions of read.
void apply_patch(const data_patch& patch, byte_overlay& overlay, const data_reader& read, sum_change* change,
                 std::size_t transfer = std::size_t{4} << 20);

/// The sum of standard §4.4.2.7 of the size bytes of the data of a unit from 0, as read gives them with overlay laid
/// over them, read in pieces of at most transfer bytes.
/// @throws the exceptions of read.
[[nodiscard]] std::uint32_t overlaid_sum(const data_reader& read, const byte_overlay& overlay, std::uint64_t size,
                                         std::size_t transfer = std::size_t{4} << 20);

/// The sum of the data after changes, from the sum before them: before, less the sum of the bytes changed before, plus
/// their sum after, in ones' complement arithmetic. Nothing when the result is a zero of ones' complement, 0 or
/// 0xFFFFFFFF, which the sum of the bytes tells apart (0 for zeros alone): only a pass over the data says which.
[[nodiscard]] std::optional<std::uint32_t> changed_sum(std::uint32_t before, const sum_change& change) noexcept;

} // namespace openfits::detail
