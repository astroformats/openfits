// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/editor.h>
#include <openfits/error.h>

#include "checksum/checksum.h"
#include "compress/image_tiles.h"
#include "compress/table_tiles.h"
#include "core/checked_math.h"
#include "core/diagnostic_log.h"
#include "core/quote.h"
#include "data/conversion.h"
#include "data/image_description.h"
#include "data/image_read.h"
#include "data/image_write.h"
#include "edit/header_edit.h"
#include "edit/layout.h"
#include "edit/overlay.h"
#include "edit/patch.h"
#include "edit/rows.h"
#include "edit/tiles.h"
#include "hdu/copy.h"
#include "header/emission.h"
#include "header/record.h"
#include "io/thread_safe_device.h"
#include "table/table_description.h"
#include "writer_state.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace openfits {

namespace {

constexpr std::uint64_t block_size = 2880;
constexpr std::uint32_t negative_zero = 0xFFFFFFFFU;

using shared_bytes = std::shared_ptr<const std::vector<std::byte>>;

shared_bytes bytes_of(std::string_view text)
{
    const std::span<const std::byte> bytes = std::as_bytes(std::span(text));
    return std::make_shared<const std::vector<std::byte>>(bytes.begin(), bytes.end());
}

// The forms of the records of the unit at index that options allow.
detail::record_options records_of(const write_options& options, std::size_t index)
{
    const detail::byte_policy policy = options.non_ascii == non_ascii::keep      ? detail::byte_policy::keep
                                       : options.non_ascii == non_ascii::replace ? detail::byte_policy::replace
                                                                                 : detail::byte_policy::refuse;
    return {.hierarch = options.hierarch,
            .truncate_comment = options.truncate_comments,
            .continue_reserved = options.continue_reserved_keywords,
            .non_ascii = policy,
            .hdu = index};
}

// The progress of a commit, reported to the progress function of its options, which cancels it by returning false.
class commit_progress
{
public:
    explicit commit_progress(const progress_function& function) : function_(function) {}

    void add_to_total(std::uint64_t bytes) noexcept
    {
        total_ += bytes;
    }

    void advance(std::uint64_t bytes)
    {
        done_ += bytes;
        total_ = std::max(total_, done_);
        report();
    }

    void report() const
    {
        if (function_ && !function_(done_, total_)) {
            throw cancelled_error(errc::cancelled, "the commit was cancelled");
        }
    }

private:
    const progress_function& function_;
    std::uint64_t total_ = 0;
    std::uint64_t done_ = 0;
};

// The device of an editor, a file compressed whole with gzip refused: its bytes are not those of the FITS file.
std::unique_ptr<detail::thread_safe_device> open_device(std::unique_ptr<random_access_device> device)
{
    auto file = std::make_unique<detail::thread_safe_device>(std::move(device));
    if (file->size() >= 2) {
        std::array<std::byte, 2> first{};
        file->read(0, first);
        if (first[0] == std::byte{0x1F} && first[1] == std::byte{0x8B}) {
            throw unsupported_error(errc::not_editable,
                                    "the file is compressed whole with gzip, whose bytes are not those of the FITS "
                                    "file: an editor cannot change it in place");
        }
    }
    return file;
}

// The reader of the file of a device, every unit walked: a walk that stops before the end of the file is refused, since
// the editor needs the whole layout.
reader open_reader(const detail::thread_safe_device& device, const editor_options& options)
{
    reader file(device.source(), read_options{.limits = options.limits});
    const std::size_t count = file.hdu_count();
    for (const diagnostic& found : file.diagnostics()) {
        if (found.severity == severity::error) {
            detail::throw_file_error(found.code, found.message, found.context);
        }
    }
    if (count > 0 && !file.hdu(count - 1).data_size()) {
        detail::throw_unavailable(file.hdu(count - 1));
    }
    return file;
}

bool holds_image(hdu_kind kind) noexcept
{
    return kind == hdu_kind::primary || kind == hdu_kind::image || kind == hdu_kind::compressed_image;
}

bool is_compressed(hdu_kind kind) noexcept
{
    return kind == hdu_kind::compressed_image || kind == hdu_kind::compressed_table;
}

// The header of a unit converted to its place in a file: a primary HDU of an image after the first unit an IMAGE
// extension, and an IMAGE extension first the primary HDU, as writer::add_hdu() converts a copy; the other cards in
// their order, their records kept.
openfits::header converted(openfits::header cards, detail::copy_form form)
{
    if (form == detail::copy_form::as_is || cards.empty()) {
        return cards;
    }
    const std::size_t axes = cards.find<std::size_t>("NAXIS").value_or(0);
    const bool extension = form == detail::copy_form::to_extension;
    for (const std::string_view keyword : extension ? std::vector<std::string_view>{"EXTEND", "GROUPS"}
                                                    : std::vector<std::string_view>{"PCOUNT", "GCOUNT"}) {
        (void)cards.erase(keyword);
    }
    cards.erase_at(0);
    cards.insert(0, extension ? card("XTENSION", "IMAGE", "image extension")
                              : card("SIMPLE", true, "conforms to the FITS standard"));
    // After the last NAXISn, or NAXIS.
    std::size_t place = cards.index_of("NAXIS").value_or(0) + 1;
    for (std::size_t n = 1; n <= axes; ++n) {
        if (const std::optional<std::size_t> at = cards.index_of("NAXIS" + std::to_string(n))) {
            place = std::max(place, *at + 1);
        }
    }
    if (extension) {
        cards.insert(place, card("PCOUNT", 0, "no parameters"));
        cards.insert(place + 1, card("GCOUNT", 1, "one array"));
    } else {
        cards.insert(place, card("EXTEND", true, "extensions may follow"));
    }
    return cards;
}

// The header of a primary HDU without data, before an extension that cannot be the first unit of a file.
std::string empty_primary(const write_options& options)
{
    image_info empty;
    empty.bitpix = bitpix::uint8;
    const std::vector<card> closing =
        options.checksums ? detail::checksum_cards(options.checksum_time ? format_date(*options.checksum_time) : "")
                          : std::vector<card>();
    std::string blocks =
        detail::header_blocks(detail::image_cards(empty, true), header{},
                              {.records = records_of(options, 0), .closing = std::span<const card>(closing)});
    if (options.checksums) {
        (void)detail::seal_checksums(blocks, 0, options.checksum_time ? format_date(*options.checksum_time) : "",
                                     records_of(options, 0));
    }
    return blocks;
}

} // namespace

struct editor::state
{
    // The pending changes of a unit of the file.
    struct unit_edit
    {
        // The header to write, made by the first call of header(): for a compressed unit, the header that the reader
        // presents.
        std::optional<openfits::header> edited{};
        // The writes into the data not yet laid over the overlay, in their order.
        std::vector<detail::data_patch> patches{};
        // The writes laid over the data, and the sums of the bytes that they changed, before and after.
        detail::byte_overlay overlay{};
        detail::sum_change change{};
        // The writes into a compressed image, over the data of its image uncompressed.
        std::vector<detail::data_patch> tile_patches{};
        detail::byte_overlay tiles{};
        // The rows appended to a table, in their order.
        std::vector<detail::appended_rows> rows{};
        // The sum of the fill of the last block of a table whose rows a commit appends in place, as the file held it
        // before the first commit that tried: a commit that failed may have written rows there.
        std::optional<std::uint32_t> fill_sum{};
        // The rows appended in place and the fill after them, as a commit lays them over the end of the table.
        std::optional<detail::byte_overlay> growth{};
        bool erased = false;
    };

    // The units of a writer placed in the file: before the unit at index before, or after the last for the number of
    // units.
    struct insertion
    {
        std::size_t before = 0;
        std::shared_ptr<const writer> units{};
    };

    state(std::unique_ptr<random_access_device> given, std::string file_path, editor_options given_options)
        : options(std::move(given_options)), path(std::move(file_path)), device(open_device(std::move(given))),
          file(open_reader(*device, options))
    {}

    // The unit at index of the file, which a pending change has not erased.
    const hdu_info& unit_at(std::size_t index) const
    {
        const hdu_info& unit = file.hdu(index);
        const auto found = edits.find(index);
        if (found != edits.end() && found->second.erased) {
            throw usage_error(errc::invalid_argument,
                              "unit " + std::to_string(index) + " is erased or replaced by a pending change",
                              {.hdu = index});
        }
        return unit;
    }

    void check_usable() const
    {
        if (damaged) {
            throw usage_error(errc::not_editable,
                              "a commit that moved units in place failed, and left the file damaged: open it again");
        }
    }

    // The header of a unit as the editor presents it: that of the image or the table that a compressed unit holds.
    openfits::header presented(const hdu_info& unit) const
    {
        if (unit.kind() == hdu_kind::compressed_image) {
            return file.image(unit.index()).header;
        }
        if (unit.kind() == hdu_kind::compressed_table) {
            return file.table(unit.index()).header;
        }
        return unit.header();
    }

    // The bytes of the data of a unit from offset on, those beyond the end of the file as zeros.
    detail::data_reader data_reader_of(const hdu_info& unit) const
    {
        // The size of the file as the last commit left it: a commit that failed may have made the device longer.
        return [this, start = unit.data_offset()](std::uint64_t offset, std::span<std::byte> destination) {
            const std::uint64_t at = start + offset;
            const std::uint64_t size = file.size();
            const std::uint64_t present = at >= size ? 0 : std::min<std::uint64_t>(size - at, destination.size());
            if (present > 0) {
                device->read(at, destination.first(present));
            }
            std::ranges::fill(destination.subspan(present), std::byte{0});
        };
    }

    // The blocks of the header of a unit as the file holds them, read once.
    shared_bytes original_blocks(const hdu_info& unit)
    {
        const auto found = blocks.find(unit.index());
        if (found != blocks.end()) {
            return found->second;
        }
        const std::uint64_t size = std::min(unit.header_size(), file.size() - unit.header_offset());
        std::vector<std::byte> bytes(detail::checked_cast<std::size_t>(size));
        device->read(unit.header_offset(), bytes);
        shared_bytes kept = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
        blocks.emplace(unit.index(), kept);
        return kept;
    }

    // The number of units that a writer places after the first unit of a file: its units but the primary HDUs that it
    // adds itself.
    static std::size_t placed_units(const writer& units) noexcept
    {
        const auto& all = units.state_->units;
        return static_cast<std::size_t>(std::ranges::count_if(all, [](const auto& unit) { return !unit.implicit; }));
    }

    // Throws usage_error (errc::invalid_argument) for a writer without units, or whose random groups would come after
    // the first unit of the file, unless first.
    static void check_units(const writer& units, bool first);

    // Lays the pending writes of every unit over its overlay, each write whole or not at all.
    void fold_writes();

    // The unit at index as the commit writes it at position, its header and its data with their pending changes, which
    // last_slot when nothing follows it in the new file, and ends_file when no unit of the file follows it, so that
    // rows appended in place extend it; rebuilds set when its data are built again, which changes the layout.
    [[nodiscard]] detail::layout_unit plan_kept(std::size_t index, std::size_t position, bool last_slot, bool ends_file,
                                                bool& rebuilds);

    // Updates the ZHECKSUM and ZDATASUM of the table header cards of the compressed unit, the CHECKSUM and DATASUM of
    // the image or table that it holds once restored, from what they recorded, the restored header, and change, the
    // change of the sum of the values of a compressed image whose tiles are encoded again (standard Appendix J.4).
    void update_restored_sums(openfits::header& cards, const hdu_info& unit,
                              const std::optional<detail::sum_change>& change,
                              const detail::record_options& records) const;

    // The sum of standard §4.4.2.7 of the data that pieces make, read from the file with their writes laid over them.
    [[nodiscard]] std::uint32_t pieces_sum(const std::vector<detail::layout_piece>& pieces) const;

    // A reader of the file again, of fresh when it is given, which then becomes the device of the editor.
    void reopen(std::unique_ptr<detail::thread_safe_device> fresh);

    void commit(const commit_options& requested);

    editor_options options;
    std::string path;
    std::unique_ptr<detail::thread_safe_device> device;
    reader file;
    std::size_t selected = 0;
    bool damaged = false;
    std::map<std::size_t, unit_edit> edits{};
    std::vector<insertion> insertions{};
    // The header blocks of the units as the file held them before the first commit that tried.
    std::map<std::size_t, shared_bytes> blocks{};
};

editor::editor(std::string_view path, const editor_options& options)
    : state_(std::make_unique<state>(std::make_unique<file_device>(path), std::string(path), options))
{}

editor::editor(const file_specification& specification, const editor_options& options)
{
    if (!specification.section.empty()) {
        throw usage_error(errc::invalid_file_specification,
                          "the file specification gives an image section, which an editor does not take: it selects "
                          "a unit alone");
    }
    state_ = std::make_unique<state>(std::make_unique<file_device>(specification.path), specification.path, options);
    if (specification.hdu_index) {
        state_->selected = state_->file.hdu(*specification.hdu_index).index();
    } else if (!specification.hdu_name.empty()) {
        state_->selected = state_->file.hdu(specification.hdu_name, specification.hdu_version).index();
    }
}

editor::editor(std::unique_ptr<random_access_device> device, const editor_options& options)
    : state_(std::make_unique<state>(std::move(device), std::string(), options))
{}

editor::editor(editor&& other) noexcept = default;
editor& editor::operator=(editor&& other) noexcept = default;
editor::~editor() = default;

const reader& editor::file() const noexcept
{
    return state_->file;
}

std::size_t editor::selected_hdu() const noexcept
{
    return state_->selected;
}

header& editor::header(std::size_t index)
{
    state_->check_usable();
    const hdu_info& unit = state_->unit_at(index);
    state::unit_edit& edit = state_->edits[index];
    if (!edit.edited) {
        edit.edited = state_->presented(unit);
    }
    return *edit.edited;
}

void editor::write_pixels(std::size_t index, numeric_type type, std::span<const std::byte> values,
                          const pixel_write_options& options)
{
    const std::size_t size = size_of(type);
    if (values.size() % size != 0 || std::bit_cast<std::uintptr_t>(values.data()) % size != 0) {
        throw usage_error(errc::invalid_argument,
                          "the values written into the image of unit " + std::to_string(index) +
                              " are not whole values of their type at an address of their alignment",
                          {.hdu = index});
    }
    write_values(index, type, values, options);
}

void editor::write_values(std::size_t index, numeric_type type, std::span<const std::byte> values,
                          const pixel_write_options& options)
{
    state_->check_usable();
    const hdu_info& unit = state_->unit_at(index);
    if (!holds_image(unit.kind())) {
        throw usage_error(errc::not_an_image, "unit " + std::to_string(index) + " holds no image", {.hdu = index});
    }
    const image_info image = detail::describe_image(unit, false);
    const detail::region_selection selection = detail::select_region(options.region, image.geometry);
    const std::uint64_t span = detail::destination_span(selection, options.pitch);
    const std::size_t value_size = size_of(type);
    const bool fits = options.pitch == 0 ? values.size() / value_size == span : values.size() / value_size >= span;
    if (!fits) {
        throw usage_error(errc::invalid_argument,
                          "the values have " + std::to_string(values.size() / value_size) +
                              " elements, and the region of unit " + std::to_string(index) + " needs " +
                              std::to_string(span),
                          {.hdu = index});
    }
    scaling applied{};
    switch (options.scaling) {
    case scaling_policy::apply:
        applied = image.scaling;
        if (applied.scale == 0.0) {
            throw invalid_data_error(errc::reserved_keyword,
                                     "BSCALE is 0, which leaves the physical values of unit " + std::to_string(index) +
                                         " unavailable",
                                     {.hdu = index, .keyword = "BSCALE"});
        }
        break;
    case scaling_policy::raw:
        break;
    case scaling_policy::override:
        applied = options.override_scaling;
        if (!std::isfinite(applied.scale) || applied.scale == 0.0 || !std::isfinite(applied.zero)) {
            throw usage_error(errc::invalid_argument,
                              "the scaling of the write has a scale of 0, or a value that is not finite",
                              {.hdu = index});
        }
        break;
    }
    const bitpix stored = image.bitpix.value_or(bitpix::uint8);
    const detail::write_conversion conversion =
        detail::make_write_conversion(type, stored, applied, image.blank, options.overflow, index);
    detail::data_patch patch = detail::region_patch(selection, image.geometry.axes, options.pitch, conversion, type,
                                                    values, state_->options.limits.max_allocation);
    state::unit_edit& edit = state_->edits[index];
    if (unit.kind() == hdu_kind::compressed_image) {
        if (!edit.patches.empty() || !edit.overlay.empty()) {
            throw usage_error(errc::invalid_argument,
                              "the compressed image of unit " + std::to_string(index) +
                                  " has bytes of its table written by write_raw(), which its values cannot follow "
                                  "before a commit",
                              {.hdu = index});
        }
        edit.tile_patches.push_back(std::move(patch));
    } else {
        edit.patches.push_back(std::move(patch));
    }
}

void editor::write_blank(std::size_t index, const std::optional<region>& area)
{
    state_->check_usable();
    const hdu_info& unit = state_->unit_at(index);
    if (!holds_image(unit.kind())) {
        throw usage_error(errc::not_an_image, "unit " + std::to_string(index) + " holds no image", {.hdu = index});
    }
    const image_info image = detail::describe_image(unit, false);
    const detail::region_selection selection = detail::select_region(area, image.geometry);
    const bitpix stored = image.bitpix.value_or(bitpix::uint8);
    if (static_cast<int>(stored) > 0 && !image.blank) {
        throw usage_error(errc::undefined_value,
                          "the integer data of unit " + std::to_string(index) +
                              " have no BLANK, which stores their undefined value",
                          {.hdu = index, .keyword = "BLANK"});
    }
    detail::data_patch patch = detail::blank_patch(selection, image.geometry.axes, stored, image.blank.value_or(0),
                                                   state_->options.limits.max_allocation);
    state::unit_edit& edit = state_->edits[index];
    if (unit.kind() == hdu_kind::compressed_image) {
        edit.tile_patches.push_back(std::move(patch));
    } else {
        edit.patches.push_back(std::move(patch));
    }
}

void editor::write_raw(std::size_t index, std::uint64_t offset, std::span<const std::byte> bytes)
{
    state_->check_usable();
    const hdu_info& unit = state_->unit_at(index);
    const std::uint64_t size = unit.data_size().value_or(0);
    if (offset > size || bytes.size() > size - offset) {
        throw usage_error(errc::invalid_argument,
                          "the bytes written lie beyond the " + std::to_string(size) + " bytes of the data of unit " +
                              std::to_string(index),
                          {.hdu = index});
    }
    state::unit_edit& edit = state_->edits[index];
    if (!edit.tile_patches.empty()) {
        throw usage_error(errc::invalid_argument,
                          "the compressed image of unit " + std::to_string(index) +
                              " has values written, which bytes of its table cannot follow before a commit",
                          {.hdu = index});
    }
    edit.patches.push_back(detail::raw_patch(offset, bytes));
}

namespace {

// The table of a unit that the editor writes into: an ASCII or a binary table, not compressed.
table_info writable_table(const reader& file, const hdu_info& unit)
{
    const std::size_t index = unit.index();
    if (unit.kind() == hdu_kind::compressed_table) {
        throw usage_error(errc::not_editable,
                          "the fields of the compressed table of unit " + std::to_string(index) +
                              " change the size of its tiles: a rewrite of the table (editor::replace_hdu()) changes "
                              "them",
                          {.hdu = index});
    }
    if (unit.kind() != hdu_kind::table && unit.kind() != hdu_kind::bintable) {
        throw usage_error(errc::not_a_table, "unit " + std::to_string(index) + " holds no table", {.hdu = index});
    }
    return file.table(index);
}

} // namespace

void editor::write_cells(std::size_t index, std::size_t column, const column_values& values,
                         const cell_write_options& options)
{
    state_->check_usable();
    const hdu_info& unit = state_->unit_at(index);
    if (options.scaling == scaling_policy::override) {
        throw usage_error(errc::invalid_argument,
                          "the fields of a table take their values with the scaling of their column, or raw",
                          {.hdu = index});
    }
    const table_info table = writable_table(state_->file, unit);
    state_->edits[index].patches.push_back(
        detail::cell_patch(table, column, options.first_row, values, options.scaling, options.overflow, index));
}

void editor::write_cells(std::size_t index, std::string_view name, const column_values& values,
                         const cell_write_options& options)
{
    state_->check_usable();
    const hdu_info& unit = state_->unit_at(index);
    const table_info table = writable_table(state_->file, unit);
    const std::optional<std::size_t> column = table.find_column(name);
    if (!column) {
        throw usage_error(errc::column_not_found,
                          "the table of unit " + std::to_string(index) + " has no column named " + detail::quote(name),
                          {.hdu = index});
    }
    write_cells(index, *column, values, options);
}

void editor::append_rows(std::size_t index, std::span<const column_values> columns, const row_append_options& options)
{
    state_->check_usable();
    const hdu_info& unit = state_->unit_at(index);
    if (options.scaling == scaling_policy::override) {
        throw usage_error(errc::invalid_argument,
                          "the fields of a table take their values with the scaling of their column, or raw",
                          {.hdu = index});
    }
    const table_info table = writable_table(state_->file, unit);
    state::unit_edit& edit = state_->edits[index];
    std::uint64_t heap = table.heap_size;
    for (const detail::appended_rows& rows : edit.rows) {
        heap = detail::checked_add<std::uint64_t>(heap, rows.heap.size());
    }
    detail::appended_rows rows = detail::append_rows(table, columns, heap, options.scaling, options.overflow, index);
    if (rows.count > 0) {
        edit.rows.push_back(std::move(rows));
    }
}

namespace {} // namespace

void editor::state::check_units(const writer& units, bool first)
{
    if (units.hdu_count() == 0) {
        throw usage_error(errc::invalid_argument, "the writer has no unit to place in the file");
    }
    // Random groups can only be the first unit of a writer, which only the primary HDU holds (standard §6).
    const auto& entry = units.state_->units.front();
    if (!first && entry.copy && entry.copy->unit->kind() == hdu_kind::random_groups) {
        (void)detail::copy_form_at(*entry.copy->unit, 1);
    }
}

std::size_t editor::append(writer units)
{
    state_->check_usable();
    state::check_units(units, false);
    std::size_t position = 0;
    for (std::size_t i = 0; i < state_->file.hdu_count(); ++i) {
        const auto found = state_->edits.find(i);
        position += found != state_->edits.end() && found->second.erased ? 0U : 1U;
    }
    for (const state::insertion& inserted : state_->insertions) {
        position += state::placed_units(*inserted.units);
    }
    state_->insertions.push_back(
        {.before = state_->file.hdu_count(), .units = std::make_shared<const writer>(std::move(units))});
    return position;
}

std::size_t editor::append(const reader& source, std::size_t index)
{
    writer units;
    (void)units.add_hdu(source, index);
    return append(std::move(units));
}

void editor::insert_hdu(std::size_t index, writer units)
{
    state_->check_usable();
    state::check_units(units, index == 0);
    if (index > state_->file.hdu_count()) {
        throw usage_error(errc::hdu_not_found,
                          "the file has " + std::to_string(state_->file.hdu_count()) + " units, and none at index " +
                              std::to_string(index),
                          {.hdu = index});
    }
    if (index == 0 && state_->file.hdu(0).kind() == hdu_kind::random_groups) {
        throw usage_error(errc::invalid_argument,
                          "the random groups of unit 0 are the primary HDU of the file (standard §6), which no unit "
                          "can be inserted before",
                          {.hdu = 0});
    }
    state_->insertions.push_back({.before = index, .units = std::make_shared<const writer>(std::move(units))});
}

void editor::replace_hdu(std::size_t index, writer units)
{
    state_->check_usable();
    state::check_units(units, index == 0);
    (void)state_->unit_at(index);
    state_->edits[index].erased = true;
    state_->insertions.push_back({.before = index, .units = std::make_shared<const writer>(std::move(units))});
}

void editor::erase_hdu(std::size_t index)
{
    state_->check_usable();
    (void)state_->unit_at(index);
    std::size_t left = state_->insertions.empty() ? 0U : 1U;
    for (std::size_t i = 0; i < state_->file.hdu_count() && left == 0; ++i) {
        const auto found = state_->edits.find(i);
        left += i != index && (found == state_->edits.end() || !found->second.erased) ? 1U : 0U;
    }
    if (left == 0) {
        throw usage_error(errc::invalid_argument,
                          "unit " + std::to_string(index) + " is the last unit of the file, which a file needs",
                          {.hdu = index});
    }
    state_->edits[index].erased = true;
}

bool editor::has_pending_changes() const noexcept
{
    if (!state_->insertions.empty()) {
        return true;
    }
    return std::ranges::any_of(state_->edits, [this](const auto& entry) {
        const state::unit_edit& edit = entry.second;
        if (edit.erased || !edit.patches.empty() || !edit.overlay.empty() || !edit.tile_patches.empty() ||
            !edit.tiles.empty() || !edit.rows.empty()) {
            return true;
        }
        if (!edit.edited) {
            return false;
        }
        // A header that differs from the one the unit presents; one that no longer reads is a change too.
        try {
            return *edit.edited != state_->presented(state_->file.hdu(entry.first));
        } catch (...) {
            return true;
        }
    });
}

void editor::discard() noexcept
{
    state_->edits.clear();
    state_->insertions.clear();
}

void editor::commit(const commit_options& options)
{
    state_->check_usable();
    state_->commit(options);
}

void editor::state::fold_writes()
{
    for (auto& [index, edit] : edits) {
        if (edit.patches.empty() && edit.tile_patches.empty()) {
            continue;
        }
        const hdu_info& unit = file.hdu(index);
        // The bytes changed are summed when the unit has checksums to update from them.
        const bool sums = options.update_checksums &&
                          (detail::datasum_of(unit.header()).value || detail::has_checksum(unit.header()));
        const detail::data_reader read = data_reader_of(unit);
        // Each write laid over whole, or not at all when its reads fail, so that the next commit lays it again.
        std::size_t folded = 0;
        try {
            for (; folded < edit.patches.size(); ++folded) {
                detail::apply_patch(edit.patches[folded], edit.overlay, read, sums ? &edit.change : nullptr);
            }
        } catch (...) {
            edit.patches.erase(edit.patches.begin(), edit.patches.begin() + static_cast<std::ptrdiff_t>(folded));
            throw;
        }
        edit.patches.clear();
        // The writes into a compressed image lie over values that only a decoding gives: each element written alone.
        for (const detail::data_patch& patch : edit.tile_patches) {
            std::size_t consumed = 0;
            for (const detail::patch_run& run : patch.runs) {
                for (std::uint64_t e = 0; e < run.count; ++e) {
                    if (run.stride == run.width) {
                        const auto length = detail::checked_cast<std::size_t>(run.count * run.width);
                        edit.tiles.write(run.offset, std::span(patch.bytes).subspan(consumed, length));
                        consumed += length;
                        break;
                    }
                    const auto width = detail::checked_cast<std::size_t>(run.width);
                    edit.tiles.write(run.offset + (e * run.stride), std::span(patch.bytes).subspan(consumed, width));
                    consumed += width;
                }
            }
        }
        edit.tile_patches.clear();
    }
}

namespace {

// A unit of the file that a commit makes, in order: a unit of the file with its changes, the units of a writer, or a
// primary HDU without data before an extension that cannot be the first unit of a file.
struct slot
{
    enum class kind : std::uint8_t
    {
        kept,
        added,
        primary,
    };

    kind type = kind::kept;
    std::size_t original = 0;
    std::size_t group = 0;
};

// The bytes of a piece whose size is known.
std::uint64_t piece_size(const detail::layout_piece& piece) noexcept
{
    return piece.type == detail::layout_piece::kind::unit && !piece.size_known ? 0 : piece.size;
}

// The ones' complement difference of two sums of the bytes changed, laid over a sum, whatever zero it gives.
std::uint32_t raw_change(std::uint32_t before, const detail::sum_change& change) noexcept
{
    return detail::add_sums(detail::add_sums(before, ~change.before), change.after);
}

// The value of the CHECKSUM record of blocks, its 16 characters from byte 12.
std::string checksum_value(std::string_view blocks)
{
    for (std::size_t at = 0; at + detail::record_length <= blocks.size(); at += detail::record_length) {
        if (blocks.substr(at, 8) == "CHECKSUM") {
            return std::string(blocks.substr(at + 11, 16));
        }
    }
    return {};
}

// Sets the value of the first card of keyword, its comment kept, or adds the card.
void set_value(header& cards, std::string_view keyword, card_value value)
{
    cards.set(keyword, std::move(value));
}

} // namespace

detail::layout_unit editor::state::plan_kept(std::size_t index, std::size_t position, bool last_slot, bool ends_file,
                                             bool& rebuilds)
{
    const hdu_info& unit = file.hdu(index);
    const auto found = edits.find(index);
    unit_edit* edit = found == edits.end() ? nullptr : &found->second;
    const write_options& write = options.write;
    const detail::record_options records = records_of(write, position);
    const detail::copy_form form = detail::copy_form_at(unit, position);
    const openfits::header& original = unit.header();
    const std::uint64_t data_offset = unit.data_offset();
    const std::uint64_t records_size = detail::data_records(unit.data_size().value_or(0));
    const std::uint64_t file_size = file.size();
    const std::uint64_t present = data_offset >= file_size ? 0 : std::min(records_size, file_size - data_offset);
    const std::byte fill = unit.kind() == hdu_kind::table ? std::byte{' '} : std::byte{0};
    const std::uint64_t minimum_blocks = (unit.header_size() + block_size - 1) / block_size;
    const detail::byte_overlay* overlay = edit != nullptr && !edit->overlay.empty() ? &edit->overlay : nullptr;

    // The header that the commit writes: the cards of the unit, edited, converted to the place of the unit.
    openfits::header cards = original;
    bool header_changed = form != detail::copy_form::as_is;
    bool presented_changed = false;
    if (edit != nullptr && edit->edited) {
        const openfits::header shown = presented(unit);
        if (*edit->edited != shown) {
            detail::check_structure(shown, *edit->edited, index);
            cards =
                is_compressed(unit.kind()) ? detail::compressed_header(original, shown, *edit->edited) : *edit->edited;
            header_changed = true;
            presented_changed = true;
        }
    }

    // The data, in pieces: as the file holds them with the writes laid over them, or built again.
    std::vector<detail::layout_piece> data;
    bool rebuilt = false;
    bool data_changed = overlay != nullptr;
    detail::sum_change change = edit != nullptr ? edit->change : detail::sum_change{};
    std::optional<detail::sum_change> image_change;
    if (edit != nullptr && !edit->rows.empty()) {
        const table_info table = file.table(index);
        std::uint64_t rows = 0;
        std::uint64_t heap = 0;
        std::vector<std::uint64_t> emax(table.columns.size(), 0);
        std::vector<std::byte> added;
        std::vector<std::byte> arrays;
        for (const detail::appended_rows& part : edit->rows) {
            rows += part.count;
            heap += part.heap.size();
            added.insert(added.end(), part.rows.begin(), part.rows.end());
            arrays.insert(arrays.end(), part.heap.begin(), part.heap.end());
            for (std::size_t c = 0; c < emax.size() && c < part.emax.size(); ++c) {
                emax[c] = std::max(emax[c], part.emax[c]);
            }
        }
        const std::uint64_t rows_bytes = detail::checked_multiply(table.rows, table.row_size);
        set_value(cards, "NAXIS2", detail::checked_add(table.rows, rows));
        header_changed = true;
        const bool heapless = table.heap_size == 0 && table.heap_offset == rows_bytes && heap == 0;
        if (heapless && ends_file) {
            // In place: the rows after those of the table, then the fill of their last block, over the fill as it was.
            const auto end = detail::checked_add<std::uint64_t>(rows_bytes, added.size());
            added.resize(detail::checked_cast<std::size_t>(detail::data_records(end) - rows_bytes), fill);
            if (!edit->fill_sum) {
                std::vector<std::byte> old(detail::checked_cast<std::size_t>(records_size - rows_bytes));
                data_reader_of(unit)(rows_bytes, old);
                edit->fill_sum = detail::sum_at(old, rows_bytes);
            }
            change.before = detail::add_sums(change.before, *edit->fill_sum);
            change.after = detail::add_sums(change.after, detail::sum_at(added, rows_bytes));
            edit->growth.emplace();
            edit->growth->write(rows_bytes, added);
            const std::uint64_t kept = std::min(present, rows_bytes);
            if (kept > 0) {
                data.push_back({.type = detail::layout_piece::kind::file,
                                .size = kept,
                                .source = data_offset,
                                .overlay = overlay,
                                .data_offset = 0});
            }
            if (kept < rows_bytes) {
                data.push_back({.type = detail::layout_piece::kind::fill,
                                .size = rows_bytes - kept,
                                .fill = fill,
                                .overlay = overlay,
                                .data_offset = kept});
            }
            data.push_back({.type = detail::layout_piece::kind::fill,
                            .size = added.size(),
                            .fill = fill,
                            .overlay = &*edit->growth,
                            .data_offset = rows_bytes});
            data_changed = true;
        } else {
            // The table built again: its rows, those appended, its heap after them, the arrays appended, the fill.
            rebuilt = true;
            data.push_back({.type = detail::layout_piece::kind::file,
                            .size = rows_bytes,
                            .source = data_offset,
                            .overlay = overlay,
                            .data_offset = 0});
            data.push_back({.type = detail::layout_piece::kind::bytes,
                            .size = added.size(),
                            .bytes = std::make_shared<const std::vector<std::byte>>(std::move(added))});
            if (table.heap_size > 0) {
                data.push_back({.type = detail::layout_piece::kind::file,
                                .size = table.heap_size,
                                .source = data_offset + table.heap_offset,
                                .overlay = overlay,
                                .data_offset = table.heap_offset});
            }
            const std::uint64_t data_end =
                detail::checked_add(detail::checked_add(rows_bytes, detail::checked_multiply(rows, table.row_size)),
                                    detail::checked_add<std::uint64_t>(table.heap_size, arrays.size()));
            if (!arrays.empty()) {
                data.push_back({.type = detail::layout_piece::kind::bytes,
                                .size = arrays.size(),
                                .bytes = std::make_shared<const std::vector<std::byte>>(std::move(arrays))});
            }
            data.push_back({.type = detail::layout_piece::kind::fill,
                            .size = detail::data_records(data_end) - data_end,
                            .fill = fill});
            if (table.kind == hdu_kind::bintable) {
                set_value(cards, "PCOUNT", detail::checked_add(table.heap_size, heap));
                (void)cards.erase("THEAP");
                for (std::size_t c = 0; c < table.columns.size(); ++c) {
                    column_format format = table.columns[c].format.value_or(column_format{});
                    if (format.storage != column_storage::fixed && emax[c] > format.max_elements.value_or(0)) {
                        format.max_elements = emax[c];
                        set_value(cards, "TFORM" + std::to_string(c + 1), format.to_string());
                    }
                }
            }
        }
    } else if (edit != nullptr && !edit->tiles.empty()) {
        // The compressed image with its tiles that the writes meet encoded again, in a table built again.
        const detail::compressed_image image = detail::describe_compressed_image(unit, false);
        detail::recompressed_image again =
            detail::recompress_image(detail::reader_access::source(file), unit, image, edit->tiles, options.limits);
        for (const card& entry : again.cards) {
            if (cards.contains(entry.keyword())) {
                set_value(cards, entry.keyword(), entry.value());
            } else {
                cards.append(entry);
            }
        }
        for (const std::string& keyword : again.removed) {
            (void)cards.erase(keyword);
        }
        const std::uint64_t size = again.data.size();
        data.push_back({.type = detail::layout_piece::kind::bytes,
                        .size = size,
                        .bytes = std::make_shared<const std::vector<std::byte>>(std::move(again.data))});
        data.push_back({.type = detail::layout_piece::kind::fill, .size = detail::data_records(size) - size});
        image_change = again.change;
        rebuilt = true;
        header_changed = true;
    } else {
        if (present > 0) {
            data.push_back({.type = detail::layout_piece::kind::file,
                            .size = present,
                            .source = data_offset,
                            .overlay = overlay,
                            .data_offset = 0});
        }
        // What a truncated file lacks: the fill of the last block, completed once something follows the unit or it
        // changes. Data that the file lacks are refused once the commit would have to write them, something after the
        // unit or a write among them, as copies refuse them: a header may declare far more data than any file holds.
        const std::uint64_t data_size = unit.data_size().value_or(0);
        const bool writes_beyond = overlay != nullptr && overlay->end() > present;
        if (present < data_size && (!last_slot || writes_beyond)) {
            throw io_error(errc::end_of_data,
                           "the file ends " + std::to_string(data_size - present) +
                               " bytes before the end of the data of unit " + std::to_string(index) +
                               ", which the commit would write",
                           {}, {.hdu = index, .offset = data_offset + present});
        }
        if (present >= data_size && present < records_size && (!last_slot || data_changed || header_changed)) {
            data.push_back({.type = detail::layout_piece::kind::fill,
                            .size = records_size - present,
                            .fill = fill,
                            .overlay = overlay,
                            .data_offset = present});
        }
    }
    rebuilds = rebuilds || rebuilt;

    if (form != detail::copy_form::as_is) {
        cards = converted(std::move(cards), form);
    }

    // The checksums of the image or the table that a compressed unit holds, as its decompression restores them.
    if (options.update_checksums && is_compressed(unit.kind()) && (presented_changed || image_change)) {
        update_restored_sums(cards, unit, image_change, records);
    }

    // The checksums of the unit: from those it had and the bytes changed (standard Appendix J.4), or summed as its
    // data are written when they are built again.
    detail::layout_unit result;
    std::string formatted;
    const bool changes = header_changed || data_changed || rebuilt;
    const detail::recorded_datasum written_datasum = detail::datasum_of(cards);
    const bool datasum_out = options.update_checksums && changes && written_datasum.value.has_value();
    const bool checksum_out = options.update_checksums && changes && detail::has_checksum(cards);
    if (rebuilt && (datasum_out || checksum_out)) {
        openfits::header placeholder = detail::with_checksum_cards(
            cards, datasum_out ? std::optional<std::uint32_t>(0) : std::nullopt, checksum_out);
        formatted = detail::edited_blocks(placeholder, records, write.longstrn, minimum_blocks);
        result.seal = [placeholder = std::move(placeholder), records, longstrn = write.longstrn, minimum_blocks,
                       datasum_out, checksum_out](std::uint32_t sum) {
            const openfits::header sealed = detail::with_checksum_cards(
                placeholder, datasum_out ? std::optional<std::uint32_t>(sum) : std::nullopt, checksum_out);
            std::string text = detail::edited_blocks(sealed, records, longstrn, minimum_blocks);
            if (checksum_out) {
                detail::seal_checksum(text, sum, negative_zero);
            }
            const std::span<const std::byte> bytes = std::as_bytes(std::span(text));
            return std::vector<std::byte>(bytes.begin(), bytes.end());
        };
    } else if (datasum_out || checksum_out) {
        const shared_bytes old_blocks = original_blocks(unit);
        const std::uint32_t old_sum = detail::sum_of(*old_blocks);
        const detail::recorded_datasum recorded = detail::datasum_of(original);
        const bool had_checksum = detail::has_checksum(original);
        std::optional<std::uint32_t> before = recorded.value;
        if (!before && had_checksum) {
            // The sum of the data that the header and CHECKSUM made negative zero.
            before = ~old_sum;
        }
        std::optional<std::uint32_t> data_sum;
        if (before) {
            data_sum = data_changed ? detail::changed_sum(*before, change) : before;
        }
        if (!data_sum) {
            data_sum = pieces_sum(data);
        }
        const std::uint32_t total = had_checksum && before ? detail::add_sums(old_sum, *before) : negative_zero;
        cards = detail::with_checksum_cards(std::move(cards), datasum_out ? data_sum : std::nullopt, checksum_out);
        formatted = detail::edited_blocks(cards, records, write.longstrn, minimum_blocks);
        if (checksum_out) {
            detail::seal_checksum(formatted, *data_sum, total);
        }
    } else if (header_changed) {
        formatted = detail::edited_blocks(cards, records, write.longstrn, minimum_blocks);
    }

    if (formatted.empty()) {
        result.pieces.push_back(
            {.type = detail::layout_piece::kind::file, .size = unit.header_size(), .source = unit.header_offset()});
    } else {
        // Built in its place: clang-tidy's analyzer of the clang-cl database takes a shared_ptr moved into a container
        // of the standard library for a leak.
        detail::layout_piece& header_piece = result.pieces.emplace_back();
        header_piece.type = detail::layout_piece::kind::bytes;
        header_piece.size = formatted.size();
        header_piece.bytes = bytes_of(formatted);
        // The blocks as the file holds them, so that a commit in place writes only those that differ.
        shared_bytes previous = original_blocks(unit);
        if (previous->size() == formatted.size()) {
            header_piece.previous = std::move(previous);
        }
    }
    result.pieces.insert(result.pieces.end(), data.begin(), data.end());
    return result;
}

void editor::state::update_restored_sums(openfits::header& cards, const hdu_info& unit,
                                         const std::optional<detail::sum_change>& change,
                                         const detail::record_options& records) const
{
    const openfits::header restored_old = presented(unit);
    const openfits::header restored_new = unit.kind() == hdu_kind::compressed_image
                                              ? detail::restored_image_header(cards)
                                              : detail::restored_table_header(cards);
    const detail::recorded_datasum written = detail::datasum_of(restored_new);
    const bool datasum_out = written.value.has_value();
    const bool checksum_out = detail::has_checksum(restored_new);
    if (!datasum_out && !checksum_out) {
        return;
    }
    // The blocks of the image or the table as a decompression writes them, and what their sums recorded.
    const detail::record_options kept{
        .hierarch = true, .truncate_comment = true, .non_ascii = detail::byte_policy::keep};
    const std::string old_blocks = detail::edited_blocks(restored_old, kept, false, 0);
    const std::uint32_t old_sum = detail::sum_of(std::as_bytes(std::span(old_blocks)));
    const detail::recorded_datasum recorded = detail::datasum_of(restored_old);
    const bool had_checksum = detail::has_checksum(restored_old);
    std::optional<std::uint32_t> before = recorded.value;
    if (!before && had_checksum) {
        before = ~old_sum;
    }
    if (!before) {
        // Nothing to compute them from: the image held none, and they stay as the application gave them.
        return;
    }
    const std::uint32_t data_sum =
        change ? detail::changed_sum(*before, *change).value_or(raw_change(*before, *change)) : *before;
    const std::uint32_t total = had_checksum ? detail::add_sums(old_sum, *before) : negative_zero;
    if (datasum_out && recorded.value) {
        set_value(cards, "ZDATASUM", std::to_string(data_sum));
    }
    if (checksum_out) {
        openfits::header sealed = detail::with_checksum_cards(
            restored_new, datasum_out && recorded.value ? std::optional<std::uint32_t>(data_sum) : std::nullopt, true);
        std::string text = detail::edited_blocks(sealed, records, false, 0);
        detail::seal_checksum(text, data_sum, total);
        set_value(cards, "ZHECKSUM", checksum_value(text));
    }
}

std::uint32_t editor::state::pieces_sum(const std::vector<detail::layout_piece>& pieces) const
{
    detail::checksum_accumulator total;
    std::vector<std::byte> chunk;
    constexpr std::uint64_t transfer = std::uint64_t{4} << 20;
    for (const detail::layout_piece& piece : pieces) {
        if (piece.type == detail::layout_piece::kind::bytes) {
            total.add(*piece.bytes);
            continue;
        }
        for (std::uint64_t done = 0; done < piece.size;) {
            const std::uint64_t part = std::min(piece.size - done, transfer);
            chunk.resize(detail::checked_cast<std::size_t>(part));
            if (piece.type == detail::layout_piece::kind::file) {
                device->read(piece.source + done, chunk);
            } else {
                std::ranges::fill(chunk, piece.fill);
            }
            if (piece.overlay != nullptr) {
                piece.overlay->apply(piece.data_offset + done, chunk);
            }
            total.add(chunk);
            done += part;
        }
    }
    return total.sum();
}

void editor::state::reopen(std::unique_ptr<detail::thread_safe_device> fresh)
{
    if (fresh) {
        reader opened = open_reader(*fresh, options);
        // The reader before the device, whose source it reads.
        file = std::move(opened);
        device = std::move(fresh);
    } else {
        file = open_reader(*device, options);
    }
}

void editor::state::commit(const commit_options& requested)
{
    commit_progress progress(requested.progress);
    progress.report();
    const write_options& write = options.write;
    if (write.gzip_level) {
        throw usage_error(errc::invalid_argument,
                          "an editor writes no file compressed with gzip: its write_options::gzip_level is not used");
    }
    fold_writes();

    // The units of the new file in their order: the units of each writer before the unit they are inserted before, then
    // the unit if no change erased it, and the units appended last.
    const std::size_t count = file.hdu_count();
    const auto erased = [this](std::size_t index) {
        const auto found = edits.find(index);
        return found != edits.end() && found->second.erased;
    };
    std::vector<slot> slots;
    for (std::size_t i = 0; i <= count; ++i) {
        for (std::size_t g = 0; g < insertions.size(); ++g) {
            if (insertions[g].before == i) {
                slots.push_back({.type = slot::kind::added, .group = g});
            }
        }
        if (i < count && !erased(i)) {
            slots.push_back({.type = slot::kind::kept, .original = i});
        }
    }
    if (slots.empty()) {
        throw usage_error(errc::invalid_argument, "the commit would leave the file without units");
    }
    if (slots.front().type == slot::kind::kept && detail::needs_primary_before(file.hdu(slots.front().original))) {
        slots.insert(slots.begin(), slot{.type = slot::kind::primary});
    }
    std::optional<std::size_t> last_kept;
    for (std::size_t s = 0; s < slots.size(); ++s) {
        if (slots[s].type == slot::kind::kept) {
            last_kept = s;
        }
    }

    // The bytes after the last unit that are not a header, special records (standard §3.5), which stay at the end.
    const hdu_info& last = file.hdu(count - 1);
    const std::uint64_t end_of_units =
        detail::checked_add(last.data_offset(), detail::data_records(last.data_size().value_or(0)));
    const std::uint64_t trailing = end_of_units < file.size() ? file.size() - end_of_units : 0;

    // Every unit of the new file planned before anything is written: headers checked and formatted, values compressed,
    // sums computed. Units inserted before others and units erased change the layout; units appended do not.
    bool rebuilds = std::ranges::any_of(insertions, [count](const insertion& entry) { return entry.before < count; }) ||
                    std::ranges::any_of(edits, [](const auto& entry) { return entry.second.erased; });
    std::vector<detail::layout_unit> units;
    std::size_t position = 0;
    for (std::size_t s = 0; s < slots.size(); ++s) {
        const slot& entry = slots[s];
        switch (entry.type) {
        case slot::kind::primary: {
            const shared_bytes header_bytes = bytes_of(empty_primary(write));
            units.push_back({.pieces = {{.type = detail::layout_piece::kind::bytes,
                                         .size = header_bytes->size(),
                                         .bytes = header_bytes}}});
            ++position;
            break;
        }
        case slot::kind::added: {
            const writer& group = *insertions[entry.group].units;
            auto formatted = std::make_shared<writer::state::formatted_file>(
                group.state_->format(write, {}, {.first = position, .extension = position > 0}));
            for (std::size_t j = 0; j < formatted->units.size(); ++j) {
                const std::optional<std::uint64_t> data = formatted->units[j].data;
                detail::layout_piece piece{.type = detail::layout_piece::kind::unit,
                                           .size = formatted->headers[j].size() + data.value_or(0),
                                           .size_known = data.has_value()};
                piece.write = [&group, formatted, j, &write](output_sink& sink,
                                                             const std::function<void(std::uint64_t)>& advance) {
                    group.state_->write_unit(sink, *formatted, j, write, advance);
                };
                units.push_back({.pieces = {std::move(piece)}});
            }
            position += formatted->units.size();
            break;
        }
        case slot::kind::kept:
            units.push_back(plan_kept(entry.original, position, s + 1 == slots.size(),
                                      last_kept && s == *last_kept && trailing == 0, rebuilds));
            ++position;
            break;
        }
    }
    if (trailing > 0) {
        units.push_back(
            {.pieces = {{.type = detail::layout_piece::kind::file, .size = trailing, .source = end_of_units}}});
    }

    // Whether every byte of the file stays where it lies, and the bytes that the commit writes.
    bool moves = false;
    std::uint64_t offset = 0;
    std::uint64_t work = 0;
    std::optional<std::uint64_t> first_change;
    for (const detail::layout_unit& unit : units) {
        for (const detail::layout_piece& piece : unit.pieces) {
            const bool stays = piece.type == detail::layout_piece::kind::file && piece.source == offset;
            moves = moves || (piece.type == detail::layout_piece::kind::file && !stays);
            if (!stays || piece.overlay != nullptr) {
                first_change = std::min(first_change.value_or(offset), offset);
            }
            work += stays ? (piece.overlay != nullptr ? piece.overlay->size() : 0) : piece_size(piece);
            offset += piece_size(piece);
        }
    }
    const std::uint64_t new_size = offset;
    const bool copy = options.atomic || ((rebuilds || moves) && options.relayout == relayout::safe_copy);
    const bool sized = std::ranges::all_of(units, [](const detail::layout_unit& unit) {
        return std::ranges::all_of(unit.pieces, [](const detail::layout_piece& piece) { return piece.size_known; });
    });
    if ((!copy || path.empty()) && sized && new_size != device->size() && !device->can_resize()) {
        throw usage_error(errc::invalid_argument, "the file of the editor cannot change its size, " +
                                                      std::to_string(device->size()) + " bytes, to the " +
                                                      std::to_string(new_size) + " bytes that the commit makes");
    }
    const auto advance = [&progress](std::uint64_t bytes) { progress.advance(bytes); };

    if (copy && !path.empty()) {
        progress.add_to_total(new_size);
        file_sink sink(path, {.flush_to_disk = write.flush_to_disk});
        detail::write_layout(sink, units, *device, advance);
        sink.finish();
        edits.clear();
        insertions.clear();
        blocks.clear();
        reopen(std::make_unique<detail::thread_safe_device>(std::make_unique<file_device>(path)));
        return;
    }
    if (copy) {
        // A device has no directory: the new file in memory, then written over the device from its first change.
        const std::uint64_t most = options.limits.max_allocation;
        if (sized && most != 0 && new_size > most) {
            throw limit_error(errc::allocation_too_large, "the copy of the file, " + std::to_string(new_size) +
                                                              " bytes, takes more than limits::max_allocation, " +
                                                              std::to_string(most) +
                                                              " bytes: relayout::in_place moves its units instead");
        }
        const std::uint64_t from = std::min(first_change.value_or(new_size), new_size);
        progress.add_to_total(new_size + (new_size - from));
        memory_sink sink;
        if (sized) {
            sink.reserve(new_size);
        }
        detail::write_layout(sink, units, *device, advance);
        const std::span<const std::byte> bytes = sink.data();
        if (bytes.size() > device->size()) {
            device->resize(bytes.size());
        }
        // From the first write on, a failure leaves the device damaged.
        constexpr std::size_t transfer = std::size_t{4} << 20;
        for (std::uint64_t at = std::min<std::uint64_t>(from, bytes.size()); at < bytes.size();) {
            const auto part = detail::checked_cast<std::size_t>(std::min<std::uint64_t>(bytes.size() - at, transfer));
            damaged = true;
            device->write(at, bytes.subspan(detail::checked_cast<std::size_t>(at), part));
            progress.advance(part);
            at += part;
        }
        if (bytes.size() < device->size()) {
            damaged = true;
            device->resize(bytes.size());
        }
        if (write.flush_to_disk) {
            device->flush();
        }
        damaged = false;
    } else {
        progress.add_to_total(work);
        bool moved = false;
        try {
            detail::shift_layout(*device, units, advance, moved);
        } catch (...) {
            damaged = moved;
            throw;
        }
        if (write.flush_to_disk) {
            device->flush();
        }
    }
    edits.clear();
    insertions.clear();
    blocks.clear();
    reopen(nullptr);
}

void update_keywords(std::string_view path, std::size_t hdu, std::span<const card> cards, const editor_options& options)
{
    editor file(path, options);
    header& edited = file.header(hdu);
    for (const card& value : cards) {
        const std::string_view keyword = value.keyword();
        const bool commentary =
            keyword.empty() || keyword == "COMMENT" || keyword == "HISTORY" || !value.has_value_indicator();
        const std::optional<std::size_t> position = commentary ? std::nullopt : edited.index_of(keyword);
        if (!position) {
            edited.append(value);
            continue;
        }
        (void)edited.erase(keyword);
        edited.insert(*position, value);
    }
    file.commit();
}

} // namespace openfits
