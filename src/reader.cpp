// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/reader.h>

#include "checksum/checksum.h"
#include "compress/image_tiles.h"
#include "compress/table_tiles.h"
#include "core/ascii.h"
#include "core/quote.h"
#include "data/image_description.h"
#include "data/image_read.h"
#include "data/random_groups.h"
#include "hdu/copy.h"
#include "hdu/header_reader.h"
#include "hdu/section.h"
#include "hdu/walk.h"
#include "table/column_read.h"
#include "table/table_description.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace openfits {

namespace {

// Names compared as FITS readers compare the names of units: without regard to case or trailing spaces.
bool same_name(std::string_view left, std::string_view right) noexcept
{
    left = detail::trim_trailing_spaces(left);
    right = detail::trim_trailing_spaces(right);
    return std::ranges::equal(left, right, [](char a, char b) { return detail::to_upper(a) == detail::to_upper(b); });
}

// The string of a keyword of the header, or nothing when it has none.
std::optional<std::string> string_of(const header& cards, std::string_view keyword)
{
    const std::optional<std::size_t> position = cards.index_of(keyword);
    if (!position || cards[*position].value().kind() != value_kind::string) {
        return std::nullopt;
    }
    return cards.value<std::string>(keyword);
}

// The unit matches when its EXTNAME is the name, or else its HDUNAME, and its EXTVER, 1 without it, is the version,
// any for 0.
bool has_name(const hdu_info& unit, std::string_view name, std::int64_t version)
{
    const header& cards = unit.header();
    const std::optional<std::string> extname = string_of(cards, "EXTNAME");
    bool named = extname && same_name(*extname, name);
    if (!named) {
        const std::optional<std::string> hduname = string_of(cards, "HDUNAME");
        named = hduname && same_name(*hduname, name);
    }
    return named && (version == 0 || unit.version() == version);
}

bool holds_image(const hdu_info& unit)
{
    const hdu_kind kind = unit.kind();
    if (kind != hdu_kind::primary && kind != hdu_kind::image && kind != hdu_kind::compressed_image) {
        return false;
    }
    return detail::image_axes(unit).has_value();
}

bool holds_table(const hdu_info& unit)
{
    const hdu_kind kind = unit.kind();
    return kind == hdu_kind::table || kind == hdu_kind::bintable || kind == hdu_kind::compressed_table;
}

[[noreturn]] void throw_not_found(const std::string& what)
{
    throw usage_error(errc::hdu_not_found, "the file has no " + what);
}

// The file of a reader: its source, or the content that the source inflates to when it starts with the bytes of a gzip
// member (RFC 1952, §2.3.1), a convention outside the standard, since a FITS file starts with SIMPLE; and the first
// bytes of a source that is not in memory, read to tell, which the walk takes as the first read of the primary header.
struct opened_file
{
    std::unique_ptr<input_source> source{};
    std::vector<std::byte> start{};
};

opened_file content_of(std::unique_ptr<input_source> source, const read_options& options)
{
    opened_file file;
    if (source == nullptr || source->size() < 2) {
        file.source = std::move(source);
        return file;
    }
    std::span<const std::byte> first = source->bytes();
    if (first.size() != source->size()) {
        const std::size_t probe = std::min(source->size(), detail::first_header_read);
        file.start.resize(probe);
        source->read(0, file.start);
        first = file.start;
    }
    if (first[0] != std::byte{0x1F} || first[1] != std::byte{0x8B}) {
        file.source = std::move(source);
        return file;
    }
    file.source = std::make_unique<gzip_source>(*source, options.limits, options.inflate);
    file.start.clear();
    return file;
}

// The table of a unit as the reads of its columns take it: the unit itself and the source, or for a compressed table
// (standard §10.3) the table that it holds, its data decompressed into memory.
class table_view
{
public:
    table_view(const hdu_info& unit, const detail::thread_safe_source& source, const limits& limits, bool with_data)
        : unit_(&unit), source_(&source)
    {
        if (unit.kind() != hdu_kind::compressed_table) {
            return;
        }
        const detail::compressed_table compressed = detail::describe_compressed_table(unit);
        restored_.emplace(detail::restored_table_unit(unit, compressed));
        unit_ = &*restored_;
        if (with_data) {
            data_.emplace(std::make_unique<memory_source>(detail::decompress_table(source, unit, compressed, limits)));
            source_ = &*data_;
        }
    }

    [[nodiscard]] const hdu_info& unit() const noexcept
    {
        return *unit_;
    }

    [[nodiscard]] const detail::thread_safe_source& source() const noexcept
    {
        return *source_;
    }

private:
    const hdu_info* unit_;
    const detail::thread_safe_source* source_;
    std::optional<hdu_info> restored_{};
    std::optional<detail::thread_safe_source> data_{};
};

} // namespace

struct reader::state
{
    state(opened_file file, const read_options& options)
        : walk(std::move(file.source), options.limits, options.strict, file.start), limits(options.limits)
    {}

    // The description of the compressed image of unit, made by its first read and kept for the others: a read of a row
    // or of a few pixels would otherwise spend most of its time describing the image again.
    const detail::compressed_image& compressed_image_of(const hdu_info& unit)
    {
        {
            const std::scoped_lock lock(descriptions_mutex);
            if (const auto found = descriptions.find(unit.index()); found != descriptions.end()) {
                return found->second;
            }
        }
        // Made outside the lock, by each thread that finds none; the first kept is the one every read takes.
        detail::compressed_image described = detail::describe_compressed_image(unit, false);
        const std::scoped_lock lock(descriptions_mutex);
        return descriptions.try_emplace(unit.index(), std::move(described)).first->second;
    }

    detail::unit_walk walk;
    openfits::limits limits;
    std::size_t selected = 0;
    std::optional<region> section{};
    // The identity of the reader, under which a tile_cache keeps the tiles it decodes: a control block that no other
    // reader takes while the cache holds a reference to it.
    std::shared_ptr<const int> identity = std::make_shared<const int>(0);
    // The descriptions of compressed_image_of(), by unit. A description stays where the map puts it, so that a read
    // keeps a reference to it after the lock.
    std::mutex descriptions_mutex{};
    std::map<std::size_t, detail::compressed_image> descriptions{};
};

const detail::thread_safe_source& detail::reader_access::source(const reader& file) noexcept
{
    return file.state_->walk.source();
}

reader::reader(std::string_view path, const read_options& options)
    : reader(std::make_unique<file_source>(path), options)
{}

reader::reader(std::unique_ptr<input_source> source, const read_options& options)
    : state_(std::make_unique<state>(content_of(std::move(source), options), options))
{
    if (options.strict) {
        // A strict open walks every unit, and verifies each against its checksums: a unit without them is not read.
        const std::size_t count = hdu_count();
        for (std::size_t i = 0; i < count; ++i) {
            const header& cards = hdu(i).header();
            if (!detail::datasum_of(cards).known && !detail::has_checksum(cards)) {
                continue;
            }
            const checksum_report report = verify(i);
            if (report.datasum == checksum_state::invalid || report.checksum == checksum_state::invalid) {
                detail::throw_checksum_mismatch(i, report);
            }
        }
    }
}

reader::reader(const file_specification& specification, const read_options& options)
    : reader(specification.path, options)
{
    std::size_t selected = 0;
    if (specification.hdu_index) {
        selected = hdu(*specification.hdu_index).index();
    } else if (!specification.hdu_name.empty()) {
        selected = hdu(specification.hdu_name, specification.hdu_version).index();
    } else if (!specification.section.empty()) {
        const std::optional<std::size_t> image = first_image();
        if (!image) {
            throw usage_error(errc::invalid_file_specification,
                              "the file specification gives an image section, and the file holds no image");
        }
        selected = *image;
    }
    if (!specification.section.empty()) {
        state_->section = detail::section_region(hdu(selected), specification.section);
    }
    state_->selected = selected;
}

reader::reader(reader&& other) noexcept = default;
reader& reader::operator=(reader&& other) noexcept = default;
reader::~reader() = default;

std::size_t reader::hdu_count() const
{
    return state_->walk.units().size();
}

std::vector<std::reference_wrapper<const hdu_info>> reader::hdus() const
{
    return state_->walk.units();
}

const hdu_info& reader::hdu(std::size_t index) const
{
    const hdu_info* unit = state_->walk.unit(index);
    if (unit == nullptr) {
        throw_not_found("unit " + std::to_string(index));
    }
    return *unit;
}

const hdu_info& reader::hdu(std::string_view name, std::int64_t version) const
{
    const std::optional<std::size_t> index = find_hdu(name, version);
    if (!index) {
        throw_not_found("unit named " + detail::quote(name) +
                        (version == 0 ? std::string() : " of version " + std::to_string(version)));
    }
    return hdu(*index);
}

std::optional<std::size_t> reader::find_hdu(std::string_view name, std::int64_t version) const
{
    return state_->walk.find([name, version](const hdu_info& unit) { return has_name(unit, name, version); });
}

std::optional<std::size_t> reader::first_image(std::size_t from) const
{
    return state_->walk.find(holds_image, from);
}

std::optional<std::size_t> reader::first_table(std::size_t from) const
{
    return state_->walk.find(holds_table, from);
}

// Through find(), which throws the error of a walk that stops before the end of the file, as hdu_count() does not.
std::vector<std::size_t> reader::images() const
{
    std::vector<std::size_t> found;
    std::optional<std::size_t> unit = first_image();
    while (unit) {
        found.push_back(*unit);
        unit = first_image(*unit + 1);
    }
    return found;
}

std::vector<std::size_t> reader::tables() const
{
    std::vector<std::size_t> found;
    std::optional<std::size_t> unit = first_table();
    while (unit) {
        found.push_back(*unit);
        unit = first_table(*unit + 1);
    }
    return found;
}

std::vector<diagnostic> reader::diagnostics() const
{
    return state_->walk.diagnostics();
}

std::size_t reader::selected_hdu() const noexcept
{
    return state_->selected;
}

const std::optional<region>& reader::section() const noexcept
{
    return state_->section;
}

std::uint64_t reader::size() const noexcept
{
    return state_->walk.source().size();
}

void reader::read_raw(std::size_t index, std::uint64_t offset, std::span<std::byte> destination) const
{
    const hdu_info& unit = hdu(index);
    const std::uint64_t size = unit.data_size().value_or(0);
    if (offset > size || destination.size() > size - offset) {
        throw usage_error(errc::invalid_argument,
                          "the bytes asked for lie beyond the " + std::to_string(size) + " bytes of the data of unit " +
                              std::to_string(index),
                          {.hdu = index});
    }
    state_->walk.source().read(unit.data_offset() + offset, destination);
}

checksum_report reader::verify(std::size_t index, const verify_options& options) const
{
    return detail::verify_unit(state_->walk.source(), hdu(index), options.progress);
}

image_info reader::image(std::size_t index) const
{
    return detail::describe_image(hdu(index));
}

std::size_t reader::destination_size(std::size_t index, const pixel_read_options& options, std::size_t value_size) const
{
    const image_info image = detail::describe_image(hdu(index), false);
    const std::uint64_t span =
        detail::destination_span(detail::select_region(options.region, image.geometry), options.pitch);
    const std::uint64_t most = state_->limits.max_allocation;
    if (span > std::numeric_limits<std::uint64_t>::max() / value_size || (most != 0 && span * value_size > most)) {
        throw limit_error(errc::allocation_too_large,
                          "the " + std::to_string(span) + " values of the read of unit " + std::to_string(index) +
                              " take more than limits::max_allocation, " + std::to_string(most) + " bytes",
                          {.hdu = index});
    }
    return span;
}

pixel_read_result reader::read_values(std::size_t index, numeric_type type, std::span<std::byte> destination,
                                      const pixel_read_options& options) const
{
    const hdu_info& unit = hdu(index);
    if (unit.kind() == hdu_kind::compressed_image) {
        return detail::read_compressed_pixels(state_->walk.source(), unit, state_->compressed_image_of(unit), type,
                                              destination, options, state_->limits, state_->identity);
    }
    return detail::read_pixels(state_->walk.source(), unit, detail::describe_image(unit, false), type, destination,
                               options);
}

pixel_read_result reader::read_pixels(std::size_t index, numeric_type type, std::span<std::byte> destination,
                                      const pixel_read_options& options) const
{
    const std::size_t size = size_of(type);
    if (destination.size() % size != 0 || std::bit_cast<std::uintptr_t>(destination.data()) % size != 0) {
        throw usage_error(errc::invalid_argument,
                          "the destination of the read of the image of unit " + std::to_string(index) +
                              " does not hold whole values of its type at an address of their alignment",
                          {.hdu = index});
    }
    return read_values(index, type, destination, options);
}

table_info reader::table(std::size_t index) const
{
    const table_view view(hdu(index), state_->walk.source(), state_->limits, false);
    return detail::describe_table(view.unit());
}

std::size_t reader::column_index(std::size_t hdu_index, std::string_view name) const
{
    const table_view view(hdu(hdu_index), state_->walk.source(), state_->limits, false);
    const table_info table = detail::describe_table(view.unit(), false);
    const std::optional<std::size_t> column = table.find_column(name);
    if (!column) {
        throw usage_error(errc::column_not_found,
                          "the table of unit " + std::to_string(hdu_index) + " has no column named " +
                              detail::quote(name),
                          {.hdu = hdu_index});
    }
    return *column;
}

std::size_t reader::column_size(std::size_t hdu_index, std::size_t column, const column_read_options& options,
                                std::size_t value_size) const
{
    const table_view view(hdu(hdu_index), state_->walk.source(), state_->limits, false);
    const table_info table = detail::describe_table(view.unit(), false);
    if (column >= table.columns.size()) {
        throw usage_error(errc::column_not_found,
                          "the table of unit " + std::to_string(hdu_index) + " has " +
                              std::to_string(table.columns.size()) + " columns, and none at index " +
                              std::to_string(column),
                          {.hdu = hdu_index});
    }
    const detail::row_selection rows = detail::select_rows(options.rows, table.rows, hdu_index);
    const std::uint64_t per_row = detail::field_elements(table.columns[column]);
    const std::uint64_t most = state_->limits.max_allocation;
    const std::uint64_t limit = std::numeric_limits<std::uint64_t>::max() / value_size;
    if ((per_row != 0 && rows.count > limit / per_row) || (most != 0 && rows.count * per_row * value_size > most)) {
        throw limit_error(errc::allocation_too_large,
                          "the values of the read of column " + std::to_string(column + 1) + " of unit " +
                              std::to_string(hdu_index) + " take more than limits::max_allocation, " +
                              std::to_string(most) + " bytes",
                          {.hdu = hdu_index, .column = column + 1});
    }
    return rows.count * per_row;
}

column_read_result reader::read_columns(std::size_t hdu_index, std::span<const column_destination> columns,
                                        const column_read_options& options) const
{
    const table_view view(hdu(hdu_index), state_->walk.source(), state_->limits, true);
    return detail::read_columns(view.source(), view.unit(), detail::describe_table(view.unit(), false), columns,
                                options);
}

std::vector<std::string> reader::read_strings(std::size_t hdu_index, std::size_t column,
                                              const column_read_options& options) const
{
    const table_view view(hdu(hdu_index), state_->walk.source(), state_->limits, true);
    return detail::read_strings(view.source(), view.unit(), detail::describe_table(view.unit(), false), column, options,
                                state_->limits);
}

void reader::read_cell_arrays(std::size_t hdu_index, std::size_t column, element_type type,
                              const column_read_options& options,
                              const std::function<std::span<std::byte>(std::uint64_t)>& allocate,
                              std::vector<std::uint64_t>& offsets, std::vector<std::uint8_t>& mask) const
{
    const table_view view(hdu(hdu_index), state_->walk.source(), state_->limits, true);
    detail::read_arrays(view.source(), view.unit(), detail::describe_table(view.unit(), false), column, type, options,
                        state_->limits, allocate, offsets, mask);
}

groups_info reader::groups(std::size_t index) const
{
    return detail::describe_groups(hdu(index), state_->limits);
}

std::size_t reader::group_range_size(std::size_t hdu_index, const parameter_read_options& options,
                                     std::size_t value_size) const
{
    const groups_info groups = detail::describe_groups(hdu(hdu_index), state_->limits, false);
    const std::uint64_t count = detail::selected_groups(groups, options, hdu_index);
    const std::uint64_t most = state_->limits.max_allocation;
    if (count > std::numeric_limits<std::uint64_t>::max() / value_size || (most != 0 && count * value_size > most)) {
        throw limit_error(errc::allocation_too_large,
                          "the values of the read of a parameter of unit " + std::to_string(hdu_index) +
                              " take more than limits::max_allocation, " + std::to_string(most) + " bytes",
                          {.hdu = hdu_index});
    }
    return count;
}

void reader::read_parameter_values(std::size_t hdu_index, std::size_t parameter, std::string_view name,
                                   numeric_type type, std::span<std::byte> destination,
                                   const parameter_read_options& options) const
{
    const hdu_info& unit = hdu(hdu_index);
    detail::read_parameters(state_->walk.source(), unit, detail::describe_groups(unit, state_->limits, false),
                            parameter, name, type, destination, options);
}

std::size_t reader::group_destination_size(std::size_t hdu_index, const pixel_read_options& options,
                                           std::size_t value_size) const
{
    const image_info image = detail::group_image(detail::describe_groups(hdu(hdu_index), state_->limits, false));
    const std::uint64_t span =
        detail::destination_span(detail::select_region(options.region, image.geometry), options.pitch);
    const std::uint64_t most = state_->limits.max_allocation;
    if (span > std::numeric_limits<std::uint64_t>::max() / value_size || (most != 0 && span * value_size > most)) {
        throw limit_error(errc::allocation_too_large,
                          "the " + std::to_string(span) + " values of the read of a group of unit " +
                              std::to_string(hdu_index) + " take more than limits::max_allocation, " +
                              std::to_string(most) + " bytes",
                          {.hdu = hdu_index});
    }
    return span;
}

pixel_read_result reader::read_group_values(std::size_t hdu_index, std::uint64_t group, numeric_type type,
                                            std::span<std::byte> destination, const pixel_read_options& options) const
{
    const hdu_info& unit = hdu(hdu_index);
    return detail::read_group(state_->walk.source(), unit, detail::describe_groups(unit, state_->limits, false), group,
                              type, destination, options);
}

} // namespace openfits
