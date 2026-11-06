// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "hdu/walk.h"

#include "checksum/checksum.h"
#include "hdu/header_reader.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace openfits::detail {

hdu_info hdu_access::make(std::size_t index, header cards, std::uint64_t header_offset, std::uint64_t header_size,
                          unit_structure structure, std::vector<diagnostic> diagnostics)
{
    hdu_info result;
    result.kind_ = structure.kind;
    result.index_ = index;
    result.header_ = std::move(cards);
    result.header_offset_ = header_offset;
    result.header_size_ = header_size;
    result.data_size_ = structure.data_size;
    result.name_ = std::move(structure.name);
    result.version_ = structure.version;
    result.diagnostics_ = std::move(diagnostics);
    return result;
}

namespace {

std::optional<std::uint64_t> add(std::uint64_t a, std::uint64_t b) noexcept
{
    if (b > std::numeric_limits<std::uint64_t>::max() - a) {
        return std::nullopt;
    }
    return a + b;
}

// The end of the block in which the bytes that end at end end, or nothing beyond 64 bits.
std::optional<std::uint64_t> block_end(std::uint64_t end) noexcept
{
    const std::optional<std::uint64_t> padded = add(end, block_size - 1);
    if (!padded) {
        return std::nullopt;
    }
    return *padded / block_size * block_size;
}

// The error that stops the walk at a unit whose header gives no size.
diagnostic first_error(const hdu_info& unit)
{
    const auto found = std::ranges::find(unit.diagnostics(), severity::error, &diagnostic::severity);
    if (found != unit.diagnostics().end()) {
        return *found;
    }
    return {.severity = severity::error,
            .code = errc::invalid_structure,
            .section = "4.4.1",
            .message = "the header of the unit gives no size for its data",
            .context = {.hdu = unit.index()}};
}

} // namespace

unit_walk::unit_walk(std::unique_ptr<input_source> source, const limits& limits, bool strict,
                     std::span<const std::byte> start)
    : source_(std::move(source)), limits_(limits), strict_(strict), log_(strict, limits.max_diagnostics)
{
    if (source_.size() == 0) {
        throw invalid_data_error(errc::not_a_fits_file, "the file is empty, and holds no primary header");
    }
    if (!start.empty() && start.size() <= source_.size()) {
        window_.prime(0, start);
    }
    const std::scoped_lock lock(mutex_);
    walk_next();
    if (strict_) {
        while (walk_next()) {
        }
    }
}

const hdu_info* unit_walk::unit(std::size_t index)
{
    const std::scoped_lock lock(mutex_);
    while (units_.size() <= index && walk_next()) {
    }
    if (index < units_.size()) {
        return &units_[index];
    }
    if (stop_) {
        throw_file_error(stop_->code, stop_->message, stop_->context);
    }
    return nullptr;
}

std::vector<std::reference_wrapper<const hdu_info>> unit_walk::units()
{
    const std::scoped_lock lock(mutex_);
    while (walk_next()) {
    }
    return {units_.begin(), units_.end()};
}

std::optional<std::size_t> unit_walk::find(const std::function<bool(const hdu_info&)>& match, std::size_t from)
{
    const std::scoped_lock lock(mutex_);
    for (std::size_t index = from;; ++index) {
        while (units_.size() <= index && walk_next()) {
        }
        if (index >= units_.size()) {
            if (stop_) {
                throw_file_error(stop_->code, stop_->message, stop_->context);
            }
            return std::nullopt;
        }
        if (match(units_[index])) {
            return index;
        }
    }
}

std::vector<diagnostic> unit_walk::diagnostics() const
{
    const std::scoped_lock lock(mutex_);
    return log_.entries();
}

void unit_walk::stop(errc code, std::string_view section, const std::string& message, error_context context)
{
    ended_ = true;
    stop_ = diagnostic{.severity = severity::error,
                       .code = code,
                       .section = std::string(section),
                       .message = message,
                       .context = context};
    if (units_.empty()) {
        throw_file_error(code, message, std::move(context));
    }
    log_.error(code, section, message, std::move(context));
}

bool unit_walk::walk_next()
{
    if (ended_) {
        return false;
    }
    const std::size_t index = units_.size();
    const std::uint64_t offset = next_offset_;
    if (offset >= source_.size()) {
        ended_ = true;
        return false;
    }
    if (limits_.max_hdus != 0 && index >= limits_.max_hdus) {
        stop(errc::too_many_hdus, "", "the file has more units than the limit of " + std::to_string(limits_.max_hdus),
             {.hdu = index, .offset = offset});
        return false;
    }

    // The diagnostics of the file are limited as a whole: a unit gets what is left, and one at least, which stands
    // for all of its own when nothing is.
    const std::uint64_t used = std::min(diagnostics_, limits_.max_diagnostics);
    diagnostic_log log(strict_,
                       limits_.max_diagnostics == 0 ? 0 : std::max<std::uint64_t>(1, limits_.max_diagnostics - used));
    // What the limit of the headers of the file leaves for this one, which is refused beyond it before it is parsed.
    const std::uint64_t headers_left =
        limits_.max_total_header_bytes == 0
            ? std::numeric_limits<std::uint64_t>::max()
            : limits_.max_total_header_bytes - std::min(header_bytes_, limits_.max_total_header_bytes);
    unit_header read =
        read_header(source_, offset, index, index == 0 ? "SIMPLE" : "XTENSION", limits_, log, window_, headers_left);
    if (read.outcome == header_outcome::not_a_header) {
        if (index == 0) {
            throw invalid_data_error(errc::not_a_fits_file,
                                     "the file does not start with the SIMPLE keyword of a primary header",
                                     {.offset = 0});
        }
        log_.warning(errc::special_records, "3.5",
                     "the " + std::to_string(source_.size() - offset) +
                         " bytes after the last unit are not a header: special records or other data, which are "
                         "ignored",
                     {.offset = offset});
        ended_ = true;
        return false;
    }
    if (read.outcome == header_outcome::too_large) {
        const diagnostic error = log.entries().back();
        stop(error.code, error.section, error.message, error.context);
        return false;
    }
    const std::optional<std::uint64_t> header_bytes = add(header_bytes_, read.size);

    header cards(std::move(read.cards));
    check_repeated_keywords(cards, offset, index, log);
    unit_structure structure = analyze_header(cards, index, offset, log);
    check_checksum_keywords(cards, read.sum, index, offset, log);
    const std::uint64_t data_offset = offset + read.size;
    const std::optional<std::uint64_t> data_size = structure.data_size;
    units_.push_back(hdu_access::make(index, std::move(cards), offset, read.size, std::move(structure), log.release()));
    header_bytes_ = header_bytes.value_or(std::numeric_limits<std::uint64_t>::max());
    diagnostics_ += units_.back().diagnostics().size();

    if (index == 1) {
        // EXTEND = F says that the file has no extensions; it does, and they are read (standard §4.4.2.1).
        const header& primary = units_.front().header();
        const std::optional<std::size_t> extend = primary.index_of("EXTEND");
        if (extend && primary[*extend].value().kind() == value_kind::logical && !primary[*extend].value().get<bool>()) {
            log_.warning(errc::reserved_keyword, "4.4.2.1",
                         "EXTEND is F in the primary header, and the file has extensions, which are read",
                         {.hdu = 0, .keyword = "EXTEND"});
        }
    }
    if (!data_size) {
        stop_ = first_error(units_.back());
        ended_ = true;
        return true;
    }
    const std::optional<std::uint64_t> data_end = add(data_offset, *data_size);
    const std::optional<std::uint64_t> next = data_end ? block_end(*data_end) : std::nullopt;
    if (!next || *next > source_.size()) {
        // A truncated last unit: the data that the file holds are read, and the walk ends.
        if (!data_end || *data_end > source_.size()) {
            log_.warning(errc::truncated_file, "3.1",
                         "the file ends before the end of the data of the unit, of which " +
                             std::to_string(source_.size() - data_offset) + " bytes of " + std::to_string(*data_size) +
                             " are present",
                         {.hdu = index, .offset = source_.size()});
        } else {
            log_.warning(errc::truncated_file, "3.1",
                         "the file ends within the fill of the last block of the data of the unit",
                         {.hdu = index, .offset = source_.size()});
        }
        ended_ = true;
        return true;
    }
    next_offset_ = *next;
    ended_ = next_offset_ == source_.size();
    return true;
}

} // namespace openfits::detail
