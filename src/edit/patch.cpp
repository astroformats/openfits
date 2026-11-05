// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "edit/patch.h"

#include <openfits/error.h>

#include "checksum/checksum.h"
#include "core/checked_math.h"
#include "core/endian.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string>

namespace openfits::detail {

namespace {

// Runs that lie less than this apart are read at once, as the reads of regions are.
constexpr std::uint64_t coalescing_gap = std::uint64_t{64} << 10;

std::uint64_t magnitude(std::int64_t step) noexcept
{
    return step < 0 ? 0 - static_cast<std::uint64_t>(step) : static_cast<std::uint64_t>(step);
}

// The runs of a region in the order of the data, one for each row along the first axis, each with the index of its row
// among the values given: the rows over the other axes, the second fastest, each axis walked in the direction of the
// data, so that the runs come in increasing order of their offsets wherever a reversed axis puts their values.
template <typename Visit>
void for_each_row(const region_selection& selection, std::span<const std::uint64_t> axes, Visit visit)
{
    const axis_selection& first = selection.axes.front();
    const std::uint64_t step = magnitude(first.step);
    const std::uint64_t low = first.step > 0 ? first.first : first.first - ((first.count - 1) * step);
    const std::uint64_t rows = selection.elements / first.count;
    std::vector<std::uint64_t> positions(axes.size(), 0);
    for (std::uint64_t r = 0; r < rows; ++r) {
        std::uint64_t element = low;
        std::uint64_t source_row = 0;
        std::uint64_t stride = axes[0];
        std::uint64_t row_stride = 1;
        for (std::size_t a = 1; a < axes.size(); ++a) {
            const axis_selection& axis = selection.axes[a];
            const std::uint64_t axis_step = magnitude(axis.step);
            const std::uint64_t axis_low = axis.step > 0 ? axis.first : axis.first - ((axis.count - 1) * axis_step);
            element += (axis_low + (positions[a] * axis_step)) * stride;
            source_row += (axis.step > 0 ? positions[a] : axis.count - 1 - positions[a]) * row_stride;
            stride *= axes[a];
            row_stride *= axis.count;
        }
        visit(element, source_row);
        for (std::size_t a = 1; a < axes.size(); ++a) {
            if (++positions[a] < selection.axes[a].count) {
                break;
            }
            positions[a] = 0;
        }
    }
}

// The bytes of the stored values of a region, within the most that they may take.
std::size_t stored_bytes(const region_selection& selection, std::size_t width, std::uint64_t most)
{
    const auto bytes = checked_multiply<std::uint64_t>(selection.elements, width);
    if (most != 0 && bytes > most) {
        throw limit_error(errc::allocation_too_large, "the " + std::to_string(bytes) +
                                                          " bytes of the stored values of the write take more than "
                                                          "limits::max_allocation, " +
                                                          std::to_string(most) + " bytes");
    }
    return checked_cast<std::size_t>(bytes);
}

// Reverses the order of count elements of width bytes.
void reverse_elements(std::byte* data, std::size_t count, std::size_t width)
{
    for (std::size_t i = 0, j = count - 1; i < j; ++i, --j) {
        std::swap_ranges(data + (i * width), data + ((i + 1) * width), data + (j * width));
    }
}

// Writes the elements of run into span, which holds the bytes of the data from the offset of the run on.
void place(const patch_run& run, std::span<const std::byte> elements, std::span<std::byte> span)
{
    if (run.stride == run.width) {
        std::ranges::copy(elements, span.begin());
        return;
    }
    for (std::uint64_t e = 0; e < run.count; ++e) {
        std::copy_n(elements.begin() + static_cast<std::ptrdiff_t>(e * run.width),
                    static_cast<std::ptrdiff_t>(run.width), span.begin() + static_cast<std::ptrdiff_t>(e * run.stride));
    }
}

// A part of a run, with its elements.
struct run_piece
{
    patch_run run{};
    std::span<const std::byte> elements{};
};

} // namespace

data_patch region_patch(const region_selection& selection, std::span<const std::uint64_t> axes, std::uint64_t pitch,
                        const write_conversion& conversion, numeric_type type, std::span<const std::byte> values,
                        std::uint64_t most)
{
    data_patch patch;
    if (selection.elements == 0) {
        return patch;
    }
    const std::size_t width = stored_size(conversion.to);
    const std::size_t value_size = size_of(type);
    const axis_selection& first = selection.axes.front();
    const std::uint64_t count = first.count;
    const std::uint64_t row_pitch = pitch == 0 ? count : pitch;
    patch.bytes.resize(stored_bytes(selection, width, most));
    patch.runs.reserve(checked_cast<std::size_t>(selection.elements / count));
    std::size_t out = 0;
    for_each_row(selection, axes, [&](std::uint64_t element, std::uint64_t source_row) {
        patch.runs.push_back(
            {.offset = element * width, .count = count, .width = width, .stride = magnitude(first.step) * width});
        std::byte* target = patch.bytes.data() + out;
        (void)convert_write(conversion, values.data() + (source_row * row_pitch * value_size), target,
                            checked_cast<std::size_t>(count));
        if (first.step < 0) {
            reverse_elements(target, checked_cast<std::size_t>(count), width);
        }
        out += checked_cast<std::size_t>(count) * width;
    });
    return patch;
}

data_patch blank_patch(const region_selection& selection, std::span<const std::uint64_t> axes, bitpix stored,
                       std::int64_t blank, std::uint64_t most)
{
    data_patch patch;
    if (selection.elements == 0) {
        return patch;
    }
    const std::size_t width = stored_size(stored);
    // The undefined value as the file stores it: BLANK in integer data, the quiet NaN of IEEE 754 in floating-point
    // data, as the writer fills reserved images.
    std::array<std::byte, 8> value{};
    switch (stored) {
    case bitpix::uint8:
        value[0] = static_cast<std::byte>(static_cast<std::uint8_t>(blank));
        break;
    case bitpix::int16:
        store_big_endian(std::span(value).first<2>(), static_cast<std::uint16_t>(blank));
        break;
    case bitpix::int32:
        store_big_endian(std::span(value).first<4>(), static_cast<std::uint32_t>(blank));
        break;
    case bitpix::int64:
        store_big_endian(std::span(value).first<8>(), static_cast<std::uint64_t>(blank));
        break;
    case bitpix::float32:
        store_big_endian(std::span(value).first<4>(), std::uint32_t{0x7FC00000U});
        break;
    case bitpix::float64:
        store_big_endian(std::span(value).first<8>(), std::uint64_t{0x7FF8000000000000U});
        break;
    }
    patch.bytes.resize(stored_bytes(selection, width, most));
    for (std::size_t i = 0; i < patch.bytes.size(); i += width) {
        std::copy_n(value.begin(), width, patch.bytes.begin() + static_cast<std::ptrdiff_t>(i));
    }
    const axis_selection& first = selection.axes.front();
    for_each_row(selection, axes, [&](std::uint64_t element, std::uint64_t /*source_row*/) {
        patch.runs.push_back(
            {.offset = element * width, .count = first.count, .width = width, .stride = magnitude(first.step) * width});
    });
    return patch;
}

data_patch raw_patch(std::uint64_t offset, std::span<const std::byte> bytes)
{
    // Elements of one byte, so that a long write is read and summed in pieces.
    data_patch patch;
    if (!bytes.empty()) {
        patch.runs.push_back({.offset = offset, .count = bytes.size(), .width = 1, .stride = 1});
        patch.bytes.assign(bytes.begin(), bytes.end());
    }
    return patch;
}

std::uint32_t sum_at(std::span<const std::byte> bytes, std::uint64_t offset) noexcept
{
    checksum_accumulator sum;
    sum.add_zeros(offset % 4U);
    sum.add(bytes);
    return sum.sum();
}

void apply_patch(const data_patch& patch, byte_overlay& overlay, const data_reader& read, sum_change* change,
                 std::size_t transfer)
{
    transfer = std::max<std::size_t>(transfer, 1);
    // The runs in pieces whose bytes, from the first to the last, are at most transfer, or one element.
    std::vector<run_piece> pieces;
    pieces.reserve(patch.runs.size());
    std::size_t consumed = 0;
    for (const patch_run& run : patch.runs) {
        const std::uint64_t per_piece =
            run.count <= 1 || transfer < run.width ? 1 : ((transfer - run.width) / run.stride) + 1;
        for (std::uint64_t first = 0; first < run.count; first += per_piece) {
            const std::uint64_t count = std::min(per_piece, run.count - first);
            const auto bytes = checked_cast<std::size_t>(count * run.width);
            pieces.push_back({.run = {.offset = run.offset + (first * run.stride),
                                      .count = count,
                                      .width = run.width,
                                      .stride = run.stride},
                              .elements = std::span(patch.bytes).subspan(consumed, bytes)});
            consumed += bytes;
        }
    }

    // The bytes of each piece, read before anything is laid over the overlay, so that a read that fails leaves the
    // overlay and the sums as they were.
    struct pending_write
    {
        std::uint64_t offset = 0;
        std::span<const std::byte> view{};
        std::vector<std::byte> owned{};
    };
    std::vector<pending_write> pending;
    pending.reserve(pieces.size());
    sum_change local;
    std::vector<std::byte> staging;
    std::size_t i = 0;
    while (i < pieces.size()) {
        const patch_run& run = pieces[i].run;
        const bool contiguous = run.stride == run.width || run.count == 1;
        if (contiguous && change == nullptr) {
            // Nothing between its elements to keep, and nothing to sum: written as it is.
            pending.push_back({.offset = run.offset, .view = pieces[i].elements});
            ++i;
            continue;
        }
        // The pieces that lie close are read at once; those of a patch never overlap, so that one written does not
        // change the bytes of another.
        const std::uint64_t start = run.offset;
        std::uint64_t end = run.end();
        std::size_t j = i + 1;
        while (j < pieces.size()) {
            const patch_run& next = pieces[j].run;
            const bool next_contiguous = next.stride == next.width || next.count == 1;
            if ((next_contiguous && change == nullptr) || next.offset - end >= coalescing_gap ||
                next.end() - start > transfer) {
                break;
            }
            end = next.end();
            ++j;
        }
        staging.resize(checked_cast<std::size_t>(end - start));
        read(start, staging);
        overlay.apply(start, staging);
        for (std::size_t k = i; k < j; ++k) {
            const patch_run& piece = pieces[k].run;
            const std::span<std::byte> span = std::span(staging).subspan(
                checked_cast<std::size_t>(piece.offset - start), checked_cast<std::size_t>(piece.end() - piece.offset));
            local.before = add_sums(local.before, sum_at(span, piece.offset));
            place(piece, pieces[k].elements, span);
            local.after = add_sums(local.after, sum_at(span, piece.offset));
            pending.push_back({.offset = piece.offset, .owned = std::vector<std::byte>(span.begin(), span.end())});
        }
        i = j;
    }
    for (pending_write& write : pending) {
        overlay.write(write.offset, write.owned.empty() ? write.view : std::span<const std::byte>(write.owned));
    }
    if (change != nullptr) {
        change->before = add_sums(change->before, local.before);
        change->after = add_sums(change->after, local.after);
    }
}

std::uint32_t overlaid_sum(const data_reader& read, const byte_overlay& overlay, std::uint64_t size,
                           std::size_t transfer)
{
    transfer = std::max<std::size_t>(transfer, 1);
    checksum_accumulator sum;
    std::vector<std::byte> chunk(checked_cast<std::size_t>(std::min<std::uint64_t>(size, transfer)));
    for (std::uint64_t done = 0; done < size;) {
        const std::span<std::byte> piece =
            std::span(chunk).first(checked_cast<std::size_t>(std::min<std::uint64_t>(size - done, transfer)));
        read(done, piece);
        overlay.apply(done, piece);
        sum.add(piece);
        done += piece.size();
    }
    return sum.sum();
}

std::optional<std::uint32_t> changed_sum(std::uint32_t before, const sum_change& change) noexcept
{
    // Bytes that sum as they did leave the sum as it was, a zero of either form included.
    if (change.before == change.after) {
        return before;
    }
    // The difference added as the complement of what is taken away (standard Appendix J.4).
    const std::uint32_t result = add_sums(add_sums(before, ~change.before), change.after);
    if (result == 0 || result == std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return result;
}

} // namespace openfits::detail
