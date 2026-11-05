// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "edit/layout.h"

#include <openfits/error.h>

#include "checksum/checksum.h"
#include "core/checked_math.h"

#include <algorithm>
#include <limits>
#include <span>
#include <utility>

namespace openfits::detail {

namespace {

constexpr std::uint64_t block_size = 2880;

// The bytes of a piece whose size is known: all but a unit that writes as much as it writes.
std::uint64_t known_size(const layout_piece& piece) noexcept
{
    return piece.type == layout_piece::kind::unit && !piece.size_known ? 0 : piece.size;
}

// Writes the bytes of a piece into sink, in pieces of at most transfer bytes, summed into sum when it is given.
void write_piece(output_sink& sink, const layout_piece& piece, const thread_safe_device& file,
                 const layout_progress& advance, std::size_t transfer, checksum_accumulator* sum)
{
    switch (piece.type) {
    case layout_piece::kind::bytes: {
        const std::span<const std::byte> bytes(*piece.bytes);
        sink.write(bytes);
        if (sum != nullptr) {
            sum->add(bytes);
        }
        advance(bytes.size());
        return;
    }
    case layout_piece::kind::unit:
        piece.write(sink, advance);
        return;
    case layout_piece::kind::fill:
        if (piece.fill == std::byte{0} && (piece.overlay == nullptr || piece.overlay->empty())) {
            // Zeros, which a file of the system holds without writing them, and which add nothing to a sum.
            sink.write_zeros(piece.size);
            if (sum != nullptr) {
                sum->add_zeros(piece.size);
            }
            advance(piece.size);
            return;
        }
        break;
    case layout_piece::kind::file:
        break;
    }
    std::vector<std::byte> chunk(checked_cast<std::size_t>(std::min<std::uint64_t>(piece.size, transfer)));
    for (std::uint64_t done = 0; done < piece.size;) {
        const std::span<std::byte> part =
            std::span(chunk).first(checked_cast<std::size_t>(std::min<std::uint64_t>(piece.size - done, transfer)));
        if (piece.type == layout_piece::kind::file) {
            file.read(piece.source + done, part);
        } else {
            std::ranges::fill(part, piece.fill);
        }
        if (piece.overlay != nullptr) {
            piece.overlay->apply(piece.data_offset + done, part);
        }
        sink.write(part);
        if (sum != nullptr) {
            sum->add(part);
        }
        advance(part.size());
        done += part.size();
    }
}

// A sink that writes into a device from an offset on, at the offsets of the device: the units that the writer writes
// into a file moved in place. The bytes from zeros_from on hold zeros, which write_zeros() then only steps over; with
// grow, a write beyond the end makes the device longer.
class device_sink final : public output_sink
{
public:
    device_sink(thread_safe_device& file, std::uint64_t start, std::uint64_t zeros_from, bool grow) noexcept
        : file_(file), position_(start), zeros_from_(zeros_from), grow_(grow)
    {}

    void write(std::span<const std::byte> data) override
    {
        reserve(checked_add<std::uint64_t>(position_, data.size()));
        file_.write(position_, data);
        position_ += data.size();
    }

    void write_zeros(std::uint64_t count) override
    {
        const std::uint64_t end = checked_add(position_, count);
        reserve(end);
        // Up to zeros_from as written zeros, in pieces of 1 MiB; the zeros after it are there.
        const std::vector<std::byte> zeros(checked_cast<std::size_t>(
            std::min<std::uint64_t>(std::uint64_t{1} << 20, std::max(zeros_from_, position_) - position_)));
        while (position_ < std::min(end, zeros_from_)) {
            const std::uint64_t part = std::min<std::uint64_t>(zeros.size(), std::min(end, zeros_from_) - position_);
            file_.write(position_, std::span(zeros).first(checked_cast<std::size_t>(part)));
            position_ += part;
        }
        position_ = end;
    }

    [[nodiscard]] std::uint64_t position() const override
    {
        return position_;
    }

    [[nodiscard]] bool can_rewrite() const override
    {
        return true;
    }

    void rewrite(std::uint64_t offset, std::span<const std::byte> data) override
    {
        file_.write(offset, data);
    }

private:
    // Makes the device at least end bytes long when it grows, by half its size at least, which the commit cuts back.
    void reserve(std::uint64_t end)
    {
        if (end <= file_.size()) {
            return;
        }
        if (!grow_) {
            throw usage_error(errc::invalid_argument, "a unit writes beyond the size that the commit computed for it");
        }
        const std::uint64_t size = file_.size();
        const std::uint64_t step = std::max<std::uint64_t>(size / 2, std::uint64_t{64} << 20);
        file_.resize(std::max(end, size > std::numeric_limits<std::uint64_t>::max() - step ? end : size + step));
    }

    thread_safe_device& file_;
    std::uint64_t position_;
    std::uint64_t zeros_from_;
    bool grow_;
};

// A move of the bytes of the file from source to destination.
struct move
{
    std::uint64_t source = 0;
    std::uint64_t destination = 0;
    std::uint64_t size = 0;
};

// Moves the bytes, in pieces of at most transfer bytes: from the first when they move towards the start of the file,
// and from the last when they move towards its end, so that each piece is read before a piece of the same move lands on
// it.
void execute(thread_safe_device& file, const move& step, const layout_progress& advance, std::vector<std::byte>& chunk,
             std::size_t transfer)
{
    const bool backwards = step.destination > step.source;
    for (std::uint64_t done = 0; done < step.size;) {
        const std::uint64_t part = std::min<std::uint64_t>(step.size - done, transfer);
        const std::uint64_t at = backwards ? step.size - done - part : done;
        chunk.resize(checked_cast<std::size_t>(part));
        file.read(step.source + at, chunk);
        file.write(step.destination + at, chunk);
        advance(part);
        done += part;
    }
}

// Writes the stretches of an overlay that lie within the data of a piece, from data_offset on for size bytes, at their
// places from destination on.
void write_overlay(thread_safe_device& file, const byte_overlay& overlay, std::uint64_t data_offset, std::uint64_t size,
                   std::uint64_t destination, const layout_progress& advance)
{
    const std::uint64_t end = data_offset + size;
    for (const auto& [offset, bytes] : overlay.stretches()) {
        const std::uint64_t stretch_end = offset + bytes.size();
        if (stretch_end <= data_offset) {
            continue;
        }
        if (offset >= end) {
            break;
        }
        const std::uint64_t from = std::max(offset, data_offset);
        const std::uint64_t to = std::min(stretch_end, end);
        file.write(
            destination + (from - data_offset),
            std::span(bytes).subspan(checked_cast<std::size_t>(from - offset), checked_cast<std::size_t>(to - from)));
        advance(to - from);
    }
}

// Writes a piece of fill of size bytes at destination, those from zeros_from on, which hold zeros, left alone when the
// fill is zeros.
void write_fill(thread_safe_device& file, const layout_piece& piece, std::uint64_t destination,
                std::uint64_t zeros_from, const layout_progress& advance, std::size_t transfer)
{
    std::uint64_t size = piece.size;
    if (piece.fill == std::byte{0}) {
        size = destination >= zeros_from ? 0 : std::min(size, zeros_from - destination);
    }
    const std::vector<std::byte> chunk(checked_cast<std::size_t>(std::min<std::uint64_t>(size, transfer)), piece.fill);
    for (std::uint64_t done = 0; done < size;) {
        const std::uint64_t part = std::min<std::uint64_t>(size - done, chunk.size());
        file.write(destination + done, std::span(chunk).first(checked_cast<std::size_t>(part)));
        advance(part);
        done += part;
    }
}

} // namespace

std::uint64_t layout_size(const std::vector<layout_unit>& units) noexcept
{
    std::uint64_t size = 0;
    for (const layout_unit& unit : units) {
        for (const layout_piece& piece : unit.pieces) {
            size += known_size(piece);
        }
    }
    return size;
}

void write_layout(output_sink& sink, const std::vector<layout_unit>& units, const thread_safe_device& file,
                  const layout_progress& advance, std::size_t transfer)
{
    transfer = std::max<std::size_t>(transfer, 1);
    for (const layout_unit& unit : units) {
        const std::uint64_t start = sink.position();
        checksum_accumulator data_sum;
        for (std::size_t p = 0; p < unit.pieces.size(); ++p) {
            write_piece(sink, unit.pieces[p], file, advance, transfer, unit.seal && p > 0 ? &data_sum : nullptr);
        }
        if (unit.seal) {
            const std::vector<std::byte> header = unit.seal(data_sum.sum());
            sink.rewrite(start, header);
        }
    }
}

void shift_layout(thread_safe_device& file, const std::vector<layout_unit>& units, const layout_progress& advance,
                  bool& moved, std::size_t transfer)
{
    transfer = std::max<std::size_t>(transfer, 1);
    // The place of each piece in the new file, the moves of the bytes of the file that lie elsewhere, and the size of
    // the new file before a last unit of unknown size.
    std::vector<std::vector<std::uint64_t>> places(units.size());
    std::vector<move> moves;
    std::uint64_t offset = 0;
    bool open_end = false;
    for (std::size_t u = 0; u < units.size(); ++u) {
        for (const layout_piece& piece : units[u].pieces) {
            if (open_end) {
                throw usage_error(errc::invalid_argument,
                                  "a unit whose size is known once it is written comes before the end of the file, "
                                  "which a commit in place cannot move");
            }
            places[u].push_back(offset);
            if (piece.type == layout_piece::kind::file && piece.source != offset && piece.size > 0) {
                moves.push_back({.source = piece.source, .destination = offset, .size = piece.size});
            }
            open_end = piece.type == layout_piece::kind::unit && !piece.size_known;
            offset = checked_add(offset, known_size(piece));
        }
    }
    const std::uint64_t old_size = file.size();
    if (offset > old_size) {
        file.resize(offset);
    }

    // The bytes that move towards the start of the file, from the first, then those that move towards its end, from
    // the last: each move then lands on bytes that no move left to make still reads.
    std::vector<move> forwards;
    std::vector<move> backwards;
    for (const move& step : moves) {
        (step.destination < step.source ? forwards : backwards).push_back(step);
    }
    std::ranges::sort(forwards, {}, &move::source);
    std::ranges::sort(backwards, std::ranges::greater{}, &move::source);
    std::vector<std::byte> chunk;
    for (const std::vector<move>* group : {&forwards, &backwards}) {
        for (const move& step : *group) {
            moved = true;
            execute(file, step, advance, chunk, transfer);
        }
    }

    // Every other piece, at its place, the bytes of the units that write themselves last of all.
    std::uint64_t end = offset;
    for (std::size_t u = 0; u < units.size(); ++u) {
        const std::vector<layout_piece>& pieces = units[u].pieces;
        for (std::size_t p = 0; p < pieces.size(); ++p) {
            const layout_piece& piece = pieces[p];
            const std::uint64_t place = places[u][p];
            switch (piece.type) {
            case layout_piece::kind::file:
                if (piece.overlay != nullptr) {
                    write_overlay(file, *piece.overlay, piece.data_offset, piece.size, place, advance);
                }
                break;
            case layout_piece::kind::bytes: {
                const std::span<const std::byte> bytes(*piece.bytes);
                if (moves.empty() && piece.previous && piece.previous->size() == bytes.size()) {
                    // Only the blocks that differ from those that the file holds there.
                    for (std::uint64_t at = 0; at < bytes.size(); at += block_size) {
                        const auto length = checked_cast<std::size_t>(std::min(block_size, bytes.size() - at));
                        const std::span<const std::byte> block = bytes.subspan(checked_cast<std::size_t>(at), length);
                        if (!std::ranges::equal(
                                block, std::span(*piece.previous).subspan(checked_cast<std::size_t>(at), length))) {
                            file.write(place + at, block);
                            advance(length);
                        }
                    }
                } else {
                    file.write(place, bytes);
                    advance(bytes.size());
                }
                break;
            }
            case layout_piece::kind::fill:
                write_fill(file, piece, place, old_size, advance, transfer);
                if (piece.overlay != nullptr) {
                    write_overlay(file, *piece.overlay, piece.data_offset, piece.size, place, advance);
                }
                break;
            case layout_piece::kind::unit: {
                device_sink sink(file, place, old_size, !piece.size_known);
                piece.write(sink, advance);
                if (!piece.size_known) {
                    end = sink.position();
                }
                break;
            }
            }
        }
    }

    // The headers completed from the data of their units as they now lie in the file.
    for (std::size_t u = 0; u < units.size(); ++u) {
        const layout_unit& unit = units[u];
        if (!unit.seal || unit.pieces.empty()) {
            continue;
        }
        const std::uint64_t data_start = places[u].front() + unit.pieces.front().size;
        std::uint64_t data_end = data_start;
        for (std::size_t p = 1; p < unit.pieces.size(); ++p) {
            data_end += known_size(unit.pieces[p]);
        }
        checksum_accumulator sum;
        for (std::uint64_t done = data_start; done < data_end;) {
            const std::uint64_t part = std::min<std::uint64_t>(data_end - done, transfer);
            chunk.resize(checked_cast<std::size_t>(part));
            file.read(done, chunk);
            sum.add(chunk);
            done += part;
        }
        const std::vector<std::byte> header = unit.seal(sum.sum());
        file.write(places[u].front(), header);
        advance(header.size());
    }
    if (end != file.size()) {
        file.resize(end);
    }
}

} // namespace openfits::detail
