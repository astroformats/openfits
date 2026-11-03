// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "data/image_read.h"

#include <openfits/error.h>

#include "checksum/checksum.h"
#include "core/scratch_buffer.h"
#include "data/conversion.h"
#include "data/image_description.h"
#include "io/range_check.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace openfits::detail {

namespace {

// Runs of a region separated by less than this are read in one read, through the staging chunk: a read of a few KiB
// from a solid-state disk costs about what a sequential read of 64 KiB costs.
constexpr std::uint64_t coalescing_gap = std::uint64_t{64} << 10;

// A region whose runs hold at least a quarter of the bytes from its first byte to its last is read in chunks of all
// of them, whatever its gaps, which a rotating disk reads faster in one stretch.
constexpr std::uint64_t dense_share = 4;

[[noreturn]] void refuse(const std::string& message)
{
    throw usage_error(errc::invalid_argument, message);
}

std::uint64_t magnitude(std::int64_t step) noexcept
{
    return step < 0 ? 0 - static_cast<std::uint64_t>(step) : static_cast<std::uint64_t>(step);
}

// The progress of a read, reported to the progress function of its options, which cancels it by returning false.
class read_progress
{
public:
    read_progress(const progress_function& function, std::uint64_t total, std::uint64_t hdu)
        : function_(function), total_(total), hdu_(hdu)
    {}

    void advance(std::uint64_t bytes)
    {
        done_ += bytes;
        report();
    }

    void report() const
    {
        if (function_ && !function_(done_, total_)) {
            throw cancelled_error(errc::cancelled,
                                  "the read of the values of unit " + std::to_string(hdu_) + " was cancelled",
                                  {.hdu = hdu_});
        }
    }

private:
    const progress_function& function_;
    std::uint64_t total_;
    std::uint64_t done_ = 0;
    std::uint64_t hdu_;
};

// What a read needs to convert the values of its runs.
struct pixel_job
{
    const thread_safe_source& source;
    // The offset of the data of the unit in the source.
    std::uint64_t data_offset = 0;
    std::size_t stored_size = 0;
    std::size_t value_size = 0;
    const read_conversion& conversion;
    // The step of the region along the first axis, from one value of a run to the next.
    std::int64_t conversion_step = 1;
    std::span<std::byte> destination;
    // One byte for each element of the destination, with blank_policy::mask; null otherwise.
    std::uint8_t* mask = nullptr;
    std::size_t transfer = pixel_transfer;
    read_progress& progress;
    std::uint64_t clamped = 0;
    // The sum of the stored bytes in the order of the data, for DATASUM; null when the read does not verify it.
    checksum_accumulator* sum = nullptr;

    // Adds the stored bytes just read to the sum.
    void add_to_sum(std::span<const std::byte> bytes) const noexcept
    {
        if (sum != nullptr) {
            sum->add(bytes);
        }
    }

    [[nodiscard]] std::byte* values_at(std::uint64_t element) const noexcept
    {
        return destination.data() + (element * value_size);
    }

    [[nodiscard]] std::uint8_t* mask_at(std::uint64_t element) const noexcept
    {
        return mask == nullptr ? nullptr : mask + element;
    }
};

// Reads count values from the element first of the data, which follow each other in the data and in the destination,
// from its element destination on: straight into the destination when their types have one size, and converted there
// in place, or through a staging chunk; in pieces of the transfer size, between which the progress is reported.
void read_contiguous(pixel_job& job, std::uint64_t first, std::uint64_t count, std::uint64_t destination)
{
    const std::uint64_t offset = job.data_offset + (first * job.stored_size);
    const std::uint64_t piece = std::max<std::uint64_t>(1, job.transfer / job.stored_size);
    const std::span<const std::byte> held = job.source.bytes();
    if (!held.empty()) {
        check_read_range(job.source.size(), offset, count * job.stored_size, job.source.description());
        for (std::uint64_t done = 0; done < count;) {
            const std::uint64_t values = std::min(piece, count - done);
            const std::span<const std::byte> stored =
                held.subspan(offset + (done * job.stored_size), values * job.stored_size);
            job.add_to_sum(stored);
            job.clamped += convert_read(job.conversion, stored.data(), 1, job.values_at(destination + done),
                                        job.mask_at(destination + done), values);
            done += values;
            job.progress.advance(values * job.stored_size);
        }
        return;
    }
    if (job.stored_size <= job.value_size) {
        // The stored values of a piece are read into the destination: in place for values of their size, and at the end
        // of the part of the piece for wider ones, which convert forwards from its start (see convert_read()).
        for (std::uint64_t done = 0; done < count;) {
            const std::uint64_t values = std::min(piece, count - done);
            std::byte* const part = job.values_at(destination + done);
            const std::span<std::byte> bytes(part + (values * (job.value_size - job.stored_size)),
                                             values * job.stored_size);
            job.source.read(offset + (done * job.stored_size), bytes);
            job.add_to_sum(bytes);
            if (job.stored_size == job.value_size) {
                job.clamped += convert_read_in_place(job.conversion, part, job.mask_at(destination + done), values);
            } else {
                job.clamped +=
                    convert_read(job.conversion, bytes.data(), 1, part, job.mask_at(destination + done), values);
            }
            done += values;
            job.progress.advance(values * job.stored_size);
        }
        return;
    }
    scratch_buffer staging(std::min(piece, count) * job.stored_size);
    for (std::uint64_t done = 0; done < count;) {
        const std::uint64_t values = std::min(piece, count - done);
        const std::span<std::byte> bytes = staging.bytes().first(values * job.stored_size);
        job.source.read(offset + (done * job.stored_size), bytes);
        job.add_to_sum(bytes);
        job.clamped += convert_read(job.conversion, bytes.data(), 1, job.values_at(destination + done),
                                    job.mask_at(destination + done), values);
        done += values;
        job.progress.advance(values * job.stored_size);
    }
}

// A run of a region: values along the first axis of one row, which one kernel call converts.
struct pixel_run
{
    // The bytes of the data that its values lie in, from the first to the last: offset from the start of the data.
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    // Where its first value lies among them: the last of them for an axis read in reverse.
    std::uint64_t start = 0;
    // Its first element in the destination, and its number of values.
    std::uint64_t destination = 0;
    std::uint64_t count = 0;

    [[nodiscard]] std::uint64_t end() const noexcept
    {
        return offset + length;
    }
};

// The runs of a region in the order of the data: the rows of the region, each in pieces of at most the transfer size
// along the first axis. The rows are walked over the other axes, the second fastest, each in the direction of the data,
// so that the runs come in the order of their offsets, wherever a reversed axis puts their values in the destination.
class run_walker
{
public:
    run_walker(const region_selection& selection, std::span<const std::uint64_t> lengths, std::uint64_t pitch,
               std::size_t stored_size, std::size_t transfer)
        : selection_(selection), lengths_(lengths), stored_size_(stored_size), positions_(selection.axes.size(), 0),
          first_(selection.axes.front()), row_pitch_(pitch == 0 ? first_.count : pitch),
          per_piece_(
              std::max<std::uint64_t>(1, transfer / std::max<std::uint64_t>(magnitude(first_.step) * stored_size, 1))),
          pieces_(((first_.count - 1) / per_piece_) + 1)
    {
        place_row();
    }

    [[nodiscard]] bool done() const noexcept
    {
        return done_;
    }

    // The run at the walker.
    [[nodiscard]] pixel_run run() const noexcept
    {
        // The pieces of a row in the order of the data: from its last values first when the axis runs in reverse.
        const std::uint64_t index = first_.step > 0 ? piece_ : pieces_ - 1 - piece_;
        const std::uint64_t begin = index * per_piece_;
        const std::uint64_t count = std::min(per_piece_, first_.count - begin);
        const std::uint64_t step = magnitude(first_.step);
        // The element of the first value of the piece along the first axis, and those of the lowest and highest.
        const std::uint64_t at = first_.step > 0 ? first_.first + (begin * step) : first_.first - (begin * step);
        const std::uint64_t low = first_.step > 0 ? at : at - ((count - 1) * step);
        const std::uint64_t high = first_.step > 0 ? at + ((count - 1) * step) : at;
        return {.offset = (row_source_ + low) * stored_size_,
                .length = (high - low + 1) * stored_size_,
                .start = (at - low) * stored_size_,
                .destination = (row_destination_ * row_pitch_) + begin,
                .count = count};
    }

    void next()
    {
        if (++piece_ < pieces_) {
            return;
        }
        piece_ = 0;
        // The other axes as an odometer, the second fastest.
        for (std::size_t a = 1; a < positions_.size(); ++a) {
            if (++positions_[a] < selection_.axes[a].count) {
                place_row();
                return;
            }
            positions_[a] = 0;
        }
        done_ = true;
    }

private:
    // The element of the data and the row of the destination of the row at the positions, which count along each axis
    // in the direction of the data.
    void place_row() noexcept
    {
        row_source_ = 0;
        row_destination_ = 0;
        std::uint64_t stride = lengths_.empty() ? 1 : lengths_[0];
        std::uint64_t rows = 1;
        for (std::size_t a = 1; a < positions_.size(); ++a) {
            const axis_selection& axis = selection_.axes[a];
            const std::uint64_t step = magnitude(axis.step);
            const std::uint64_t low = axis.step > 0 ? axis.first : axis.first - ((axis.count - 1) * step);
            row_source_ += (low + (positions_[a] * step)) * stride;
            const std::uint64_t k = axis.step > 0 ? positions_[a] : axis.count - 1 - positions_[a];
            row_destination_ += k * rows;
            stride *= lengths_[a];
            rows *= axis.count;
        }
    }

    const region_selection& selection_;
    std::span<const std::uint64_t> lengths_;
    std::size_t stored_size_;
    std::vector<std::uint64_t> positions_;
    axis_selection first_;
    std::uint64_t row_pitch_ = 0;
    std::uint64_t per_piece_ = 1;
    std::uint64_t pieces_ = 1;
    std::uint64_t piece_ = 0;
    std::uint64_t row_source_ = 0;
    std::uint64_t row_destination_ = 0;
    bool done_ = false;
};

// Converts the values of a run from bytes, which hold its bytes from their first.
void convert_run(pixel_job& job, const pixel_run& run, const std::byte* bytes)
{
    job.clamped += convert_read(job.conversion, bytes + run.start, job.conversion_step, job.values_at(run.destination),
                                job.mask_at(run.destination), run.count);
}

// The lowest element of each axis of the region, or the highest.
std::uint64_t corner(const region_selection& selection, std::span<const std::uint64_t> lengths, bool highest)
{
    std::uint64_t index = 0;
    std::uint64_t stride = 1;
    for (std::size_t a = 0; a < lengths.size(); ++a) {
        const axis_selection& axis = selection.axes[a];
        const std::uint64_t reach = (axis.count - 1) * magnitude(axis.step);
        const bool forwards = axis.step > 0;
        const std::uint64_t low = forwards ? axis.first : axis.first - reach;
        index += (highest ? low + reach : low) * stride;
        stride *= lengths[a];
    }
    return index;
}

// True when the region is one stretch of the data, in order, which lands in one stretch of the destination: the axes
// before one are whole, that one runs forwards or has one element, and those after it have one element; rows that
// follow each other in the data follow each other in the destination too.
bool contiguous(const region_selection& selection, std::span<const std::uint64_t> lengths, std::uint64_t pitch)
{
    std::size_t a = 0;
    while (a < lengths.size() && selection.axes[a].step == 1 && selection.axes[a].count == lengths[a]) {
        ++a;
    }
    if (a < lengths.size() && selection.axes[a].step != 1 && selection.axes[a].count != 1) {
        return false;
    }
    for (std::size_t b = a + 1; b < lengths.size(); ++b) {
        if (selection.axes[b].count != 1) {
            return false;
        }
    }
    const std::uint64_t row = selection.axes.front().count;
    return selection.elements == row || pitch == 0 || pitch == row;
}

// Reads the runs of a region. Without the bytes in memory, the runs that gaps of less than coalescing_gap separate, or
// every run of a dense region, are read together through the staging chunk, as many as it holds; a run read alone goes
// straight into the destination when the values convert there in place.
void read_runs(pixel_job& job, const region_selection& selection, std::span<const std::uint64_t> lengths,
               std::uint64_t pitch)
{
    run_walker walker(selection, lengths, pitch, job.stored_size, job.transfer);
    const std::span<const std::byte> held = job.source.bytes();
    if (!held.empty()) {
        for (; !walker.done(); walker.next()) {
            const pixel_run run = walker.run();
            check_read_range(job.source.size(), job.data_offset + run.offset, run.length, job.source.description());
            job.add_to_sum(held.subspan(job.data_offset + run.offset, run.length));
            convert_run(job, run, held.data() + job.data_offset + run.offset);
            job.progress.advance(run.count * job.stored_size);
        }
        return;
    }

    const axis_selection& first = selection.axes.front();
    const std::uint64_t rows = selection.elements / first.count;
    const std::uint64_t row_bytes = (((first.count - 1) * magnitude(first.step)) + 1) * job.stored_size;
    const std::uint64_t hull =
        (corner(selection, lengths, true) - corner(selection, lengths, false) + 1) * job.stored_size;
    const bool dense = rows * row_bytes >= hull / dense_share;
    // A run is at most the transfer size, or one value.
    const std::uint64_t capacity = std::max<std::uint64_t>(job.transfer, job.stored_size);
    const bool in_place = job.stored_size == job.value_size && job.conversion_step == 1;
    std::optional<scratch_buffer> staging;
    std::vector<pixel_run> group;
    while (!walker.done()) {
        group.clear();
        group.push_back(walker.run());
        walker.next();
        const std::uint64_t start = group.front().offset;
        std::uint64_t end = group.front().end();
        while (!walker.done()) {
            const pixel_run run = walker.run();
            if ((!dense && run.offset - end >= coalescing_gap) || run.end() - start > capacity) {
                break;
            }
            end = run.end();
            group.push_back(run);
            walker.next();
        }
        std::uint64_t values = 0;
        if (group.size() == 1 && in_place) {
            const pixel_run& run = group.front();
            const std::span<std::byte> bytes(job.values_at(run.destination), run.count * job.value_size);
            job.source.read(job.data_offset + run.offset, bytes);
            job.add_to_sum(bytes);
            job.clamped += convert_read_in_place(job.conversion, bytes.data(), job.mask_at(run.destination), run.count);
            values = run.count;
        } else {
            if (!staging) {
                staging.emplace(std::min(capacity, hull));
            }
            const std::span<std::byte> bytes = staging->bytes().first(end - start);
            job.source.read(job.data_offset + start, bytes);
            job.add_to_sum(bytes);
            for (const pixel_run& run : group) {
                convert_run(job, run, bytes.data() + (run.offset - start));
                values += run.count;
            }
        }
        job.progress.advance(values * job.stored_size);
    }
}

} // namespace

region_selection select_region(const std::optional<region>& value, const geometry& array)
{
    const std::vector<std::uint64_t>& lengths = array.axes;
    const std::size_t axes = lengths.size();
    const region whole{};
    const region& box = value ? *value : whole;
    const auto check_entries = [axes](std::size_t entries, const char* what) {
        if (entries != 0 && entries != axes) {
            refuse("the " + std::string(what) + " of the region has " + std::to_string(entries) +
                   " entries, and the image " + std::to_string(axes) + " axes");
        }
    };
    check_entries(box.start.size(), "start");
    check_entries(box.size.size(), "size");
    check_entries(box.step.size(), "step");

    region_selection selection;
    selection.axes.resize(axes);
    selection.elements = axes == 0 ? 0 : 1;
    for (std::size_t a = 0; a < axes; ++a) {
        const std::uint64_t length = lengths[a];
        const std::uint64_t start = box.start.empty() ? 0 : box.start[a];
        // An axis of length 0 holds no data, which only the whole image selects.
        if (length == 0 && !value) {
            selection.axes[a] = {.first = 0, .count = 0, .step = 1};
            selection.elements = 0;
            continue;
        }
        if (start >= length) {
            refuse("the region starts at element " + std::to_string(start) + " of axis " + std::to_string(a + 1) +
                   ", which has " + std::to_string(length));
        }
        const std::uint64_t size = box.size.empty() ? length - start : box.size[a];
        if (size == 0) {
            refuse("the region has no elements along axis " + std::to_string(a + 1));
        }
        if (size > length - start) {
            refuse("the region of " + std::to_string(size) + " elements from element " + std::to_string(start) +
                   " goes past the end of axis " + std::to_string(a + 1) + ", which has " + std::to_string(length));
        }
        const std::int64_t step = box.step.empty() ? 1 : box.step[a];
        if (step == 0) {
            refuse("the region has a step of 0 along axis " + std::to_string(a + 1));
        }
        const std::uint64_t count = ((size - 1) / magnitude(step)) + 1;
        selection.axes[a] = {.first = step > 0 ? start : start + size - 1, .count = count, .step = step};
        // Within the elements of the image, whose data size fits in 64 bits.
        selection.elements *= count;
    }
    return selection;
}

std::uint64_t destination_span(const region_selection& selection, std::uint64_t pitch)
{
    if (selection.elements == 0 || pitch == 0) {
        return selection.elements;
    }
    const std::uint64_t row = selection.axes.front().count;
    if (pitch < row) {
        throw usage_error(errc::invalid_argument, "the pitch of " + std::to_string(pitch) +
                                                      " elements is shorter than a row of the region, " +
                                                      std::to_string(row));
    }
    const std::uint64_t rows = selection.elements / row;
    if (rows > 1 && pitch > (std::numeric_limits<std::uint64_t>::max() - row) / (rows - 1)) {
        throw usage_error(errc::arithmetic_overflow, "the rows of the region with a pitch of " + std::to_string(pitch) +
                                                         " elements do not fit in 64 bits");
    }
    return ((rows - 1) * pitch) + row;
}

pixel_read_plan plan_pixel_read(const hdu_info& unit, const image_info& image, numeric_type type,
                                std::span<std::byte> destination, const pixel_read_options& options)
{
    const std::uint64_t hdu = unit.index();
    // describe_image() gives every image its BITPIX.
    if (!image.bitpix) {
        throw usage_error(errc::invalid_argument, "the image of unit " + std::to_string(hdu) + " has no BITPIX",
                          {.hdu = hdu});
    }
    const bitpix stored = *image.bitpix;
    pixel_read_plan plan{.selection = select_region(options.region, image.geometry)};
    const std::uint64_t span = destination_span(plan.selection, options.pitch);
    const std::size_t value_size = size_of(type);
    if (span > std::numeric_limits<std::uint64_t>::max() / value_size) {
        throw usage_error(errc::arithmetic_overflow, "the destination of the region does not fit in 64 bits",
                          {.hdu = hdu});
    }
    // Exactly the values of the region, or with a pitch at least the elements that its rows span.
    const auto fits = [&](std::uint64_t size, std::uint64_t needed) {
        return options.pitch == 0 ? size == needed : size >= needed;
    };
    if (!fits(destination.size(), span * value_size)) {
        throw usage_error(errc::invalid_argument,
                          "the destination has " + std::to_string(destination.size() / value_size) +
                              " values, and the region of unit " + std::to_string(hdu) + " needs " +
                              std::to_string(span),
                          {.hdu = hdu});
    }
    if (options.blank == blank_policy::mask && !fits(options.mask.size(), span)) {
        throw usage_error(errc::invalid_argument,
                          "the mask has " + std::to_string(options.mask.size()) + " bytes, and the region of unit " +
                              std::to_string(hdu) + " needs " + std::to_string(span),
                          {.hdu = hdu});
    }
    if (plan.selection.elements == 0) {
        return plan;
    }
    if (!unit.data_size()) {
        throw_unavailable(unit);
    }

    scaling values{};
    switch (options.scaling) {
    case scaling_policy::apply:
        values = image.scaling;
        if (values.scale == 0.0) {
            throw invalid_data_error(errc::reserved_keyword,
                                     "BSCALE is 0, which leaves the physical values of unit " + std::to_string(hdu) +
                                         " unavailable",
                                     {.hdu = hdu, .keyword = "BSCALE"});
        }
        break;
    case scaling_policy::raw:
        break;
    case scaling_policy::override:
        values = options.override_scaling;
        if (!std::isfinite(values.scale) || values.scale == 0.0 || !std::isfinite(values.zero)) {
            throw usage_error(errc::invalid_argument,
                              "the scaling of the read has a scale of 0, or a value that is "
                              "not finite",
                              {.hdu = hdu});
        }
        break;
    }
    plan.conversion = make_read_conversion(stored, type, values, image.blank, options.blank, options.blank_value,
                                           options.overflow, options.rounding, hdu);
    return plan;
}

pixel_read_result read_pixels(const thread_safe_source& source, const hdu_info& unit, const image_info& image,
                              numeric_type type, std::span<std::byte> destination, const pixel_read_options& options,
                              std::size_t transfer, std::uint64_t array_offset)
{
    const std::uint64_t hdu = unit.index();
    const pixel_read_plan plan = plan_pixel_read(unit, image, type, destination, options);
    const region_selection& selection = plan.selection;
    const bitpix stored = image.bitpix.value_or(bitpix::uint8);
    const std::size_t value_size = size_of(type);
    read_progress progress(options.progress, selection.elements * stored_size(stored), hdu);
    if (!plan.conversion) {
        progress.report();
        return {};
    }
    const read_conversion& conversion = *plan.conversion;

    // A read of the whole image of a unit verifies its DATASUM (standard §4.4.2.7), summing the stored bytes as they
    // are read, then the rest of the data records: the fill of their last block, read from the file. A region does not,
    // nor the array of a random group, which is part of its unit.
    const bool whole = !options.region && (unit.kind() == hdu_kind::primary || unit.kind() == hdu_kind::image);
    const recorded_datasum datasum = whole && options.verify_checksum ? datasum_of(unit.header()) : recorded_datasum{};
    checksum_accumulator sum;

    progress.report();
    pixel_job job{.source = source,
                  .data_offset = unit.data_offset() + array_offset,
                  .stored_size = stored_size(stored),
                  .value_size = value_size,
                  .conversion = conversion,
                  .conversion_step = selection.axes.front().step,
                  .destination = destination,
                  .mask = options.blank == blank_policy::mask ? options.mask.data() : nullptr,
                  .transfer = std::max<std::size_t>(transfer, 1),
                  .progress = progress,
                  .sum = datasum.known ? &sum : nullptr};
    const std::span<const std::uint64_t> lengths = image.geometry.axes;
    if (contiguous(selection, lengths, options.pitch)) {
        read_contiguous(job, corner(selection, lengths, false), selection.elements, 0);
    } else {
        read_runs(job, selection, lengths, options.pitch);
    }
    if (datasum.known) {
        const std::uint64_t read = selection.elements * job.stored_size;
        const std::uint64_t records = data_records(unit.data_size().value_or(0));
        if (records > read) {
            add_source_bytes(sum, source, job.data_offset + read, records - read);
        }
        if (datasum.value != sum.sum()) {
            throw_datasum_mismatch(hdu, sum.sum(), datasum);
        }
    }
    return {.clamped = job.clamped};
}

} // namespace openfits::detail
