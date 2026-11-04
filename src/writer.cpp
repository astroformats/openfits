// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>
#include <openfits/reader.h>
#include <openfits/writer.h>

#include "checksum/checksum.h"
#include "compress/image_compress.h"
#include "compress/table_tiles.h"
#include "data/conversion.h"
#include "data/image_write.h"
#include "hdu/copy.h"
#include "header/emission.h"
#include "header/record.h"
#include "table/table_write.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace openfits {

namespace {

// The progress of a save, reported to the progress function of its options, which cancels it by returning false.
class save_progress
{
public:
    save_progress(const progress_function& function, std::uint64_t total) : function_(function), total_(total) {}

    // The work of the save once the units are compressed and the size of the file is known.
    void add_to_total(std::uint64_t bytes) noexcept
    {
        total_ += bytes;
    }

    // Work done; beyond the total, that of a streamed table whose size is known as it is written, which the total
    // takes.
    void advance(std::uint64_t bytes)
    {
        done_ += bytes;
        total_ = std::max(total_, done_);
        report();
    }

    void report() const
    {
        if (function_ && !function_(done_, total_)) {
            throw cancelled_error(errc::cancelled, "the save was cancelled");
        }
    }

private:
    const progress_function& function_;
    std::uint64_t total_;
    std::uint64_t done_ = 0;
};

} // namespace

struct writer::state
{
    // A unit as it is added: an image, its model and its values, borrowed, given by a function or reserved, or none for
    // a header alone; a table instead when it has one, its values borrowed or its rows given by a function; or a copy
    // of a unit of a file.
    struct unit
    {
        image_info image{};
        numeric_type type = numeric_type::uint8;
        std::span<const std::byte> data{};
        image_fill fill{};
        image_write_options options{};
        std::optional<reserved_fill> reserved{};
        std::optional<detail::table_model> table{};
        table_rows rows{};
        std::optional<detail::unit_copy> copy{};
        // The primary HDU without data that a first image compressed by its own options comes after, whose image the
        // table names as the primary one (standard §10.1.2).
        bool hosts_compressed_primary = false;
    };

    // A unit of the file of a save: a unit of the writer, compressed or not, or the primary HDU without data that a
    // compressed first image needs (standard §10.1).
    struct file_unit
    {
        std::optional<std::size_t> source{};
        std::optional<detail::compressed_unit> compressed{};
        // The bytes of its data records; nothing for a streamed table whose rows or heap are known once written.
        std::optional<std::uint64_t> data{};
        // Its header holds the records of checksum_cards(), which the save completes once its data are written.
        bool sealed = false;
    };

    // The units of the file of a save, their headers formatted, the bytes of the file and of the data of its units,
    // those of streamed tables whose size is known once they are written left out.
    struct formatted_file
    {
        std::vector<file_unit> units{};
        std::vector<std::string> headers{};
        std::uint64_t size = 0;
        std::uint64_t data = 0;
        bool size_known = true;
    };

    std::vector<unit> units{};

    // The compression of the unit at index: its own, or else that of options; none for a unit that has no data to
    // compress, an image without values, a reserved image, a streamed table or a copy.
    [[nodiscard]] std::optional<compression_options> compression_of(std::size_t index,
                                                                    const write_options& options) const;

    // The bytes of the stored values of the units that a save with options compresses.
    [[nodiscard]] std::uint64_t compressed_bytes(const write_options& options) const;

    // The file of a save with options, its units compressed, advance receiving the bytes of the values compressed.
    [[nodiscard]] formatted_file format(const write_options& options,
                                        const std::function<void(std::uint64_t)>& advance = {}) const;

    // The header of a unit of the file, at position in it, as a save with options writes it, in whole blocks, closing
    // with the cards of closing.
    [[nodiscard]] std::string format_header(const file_unit& entry, std::size_t position, const write_options& options,
                                            std::span<const card> closing) const;

    // The header of a table unit at position, with the cards of the model table.
    [[nodiscard]] static std::string table_header(const detail::table_model& table, const unit& model,
                                                  std::size_t position, const write_options& options,
                                                  std::span<const card> closing);

    // Writes the data of a unit of the file into sink, the fill of the last block included; for a streamed table, what
    // the write found, which completes its header.
    std::optional<detail::streamed_table> write_data(output_sink& sink, const file_unit& entry, std::size_t position,
                                                     const std::function<void(std::uint64_t)>& advance) const;

    // The unit of the writer that a unit of the file writes, or nothing for the primary HDU that a save adds.
    [[nodiscard]] const unit* model_of(const file_unit& entry) const noexcept
    {
        return entry.source ? &units[*entry.source] : nullptr;
    }

    // Throws usage_error for a unit whose header a save with options completes once its streamed data are written,
    // which a sink that cannot rewrite refuses: the checksums of uncompressed streamed data, which a pass of their own
    // would ask the function for again, and the NAXIS2 or the heap of a streamed table.
    void check_streams(const write_options& options) const;

    // True for a streamed table whose header is completed once its rows are written: their number, or its heap.
    [[nodiscard]] bool completes_table(const file_unit& entry) const noexcept
    {
        const unit* model = model_of(entry);
        return model != nullptr && model->rows && !entry.data;
    }

    // Writes the file to sink, as writer::save() does, without gzip.
    void save(output_sink& sink, const write_options& options) const;
};

namespace {

// The level of zlib of write_options::gzip_level, which gzip_sink takes: checked before a file is made.
void check_gzip_level(const write_options& options)
{
    if (options.gzip_level && (*options.gzip_level < -1 || *options.gzip_level > 9)) {
        throw usage_error(errc::invalid_argument,
                          "write_options::gzip_level is " + std::to_string(*options.gzip_level) + ", not from -1 to 9");
    }
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

// The comment of the checksum cards: the time of the checksums, or none.
std::string checksum_comment(const write_options& options)
{
    return options.checksum_time ? format_date(*options.checksum_time) : std::string();
}

// The checksum cards that a save puts in the headers it completes, with placeholders.
std::vector<card> checksum_cards_of(const write_options& options)
{
    return detail::checksum_cards(checksum_comment(options));
}

// The conversion of the values of an image model into its stored ones: stored values into the type of BITPIX alone,
// and the values of an override by its scaling, which the header does not give.
detail::write_conversion conversion_of(const image_info& image, numeric_type type, const image_write_options& options,
                                       std::size_t position)
{
    scaling applied = image.scaling;
    if (options.scaling == scaling_policy::raw) {
        applied = {};
    } else if (options.scaling == scaling_policy::override) {
        applied = options.override_scaling;
    }
    return detail::make_write_conversion(type, detail::model_bitpix(image), applied, image.blank, options.overflow,
                                         position);
}

// The elements of a row of an image: its first axis.
std::uint64_t row_of(const image_info& image) noexcept
{
    return image.geometry.axes.empty() ? 0 : image.geometry.axes.front();
}

// True for a table with variable-length arrays.
bool has_heap(const detail::table_model& table) noexcept
{
    return std::ranges::any_of(
        table.columns, [](const detail::column_model& model) { return model.format.storage != column_storage::fixed; });
}

} // namespace

writer::writer() : state_(std::make_unique<state>()) {}

writer::writer(writer&& other) noexcept = default;
writer& writer::operator=(writer&& other) noexcept = default;
writer::~writer() = default;

std::size_t writer::add_values(image_info info, numeric_type type, std::span<const std::byte> data, image_fill fill,
                               image_write_options options)
{
    const std::size_t index = state_->units.size();
    if (options.scaling != scaling_policy::apply && !info.bitpix) {
        throw usage_error(errc::invalid_argument,
                          "the values of the image of unit " + std::to_string(index) +
                              " convert into the BITPIX of the model, which has none",
                          {.hdu = index});
    }
    const scaling& converting = options.override_scaling;
    if (options.scaling == scaling_policy::override &&
        (!std::isfinite(converting.scale) || converting.scale == 0.0 || !std::isfinite(converting.zero))) {
        throw usage_error(errc::invalid_argument,
                          "the scaling that converts the values of the image of unit " + std::to_string(index) +
                              " has a scale of 0, or a value that is not finite",
                          {.hdu = index});
    }
    const bool streamed = static_cast<bool>(fill);
    image_info image = detail::resolve_image(
        std::move(info), type, streamed ? std::nullopt : std::optional(data.size() / size_of(type)), index);
    const bool compressed =
        options.compression && options.compression->type != compression::none && image.geometry.element_count() > 0;
    state::unit entry{
        .image = std::move(image), .type = type, .data = data, .fill = std::move(fill), .options = std::move(options)};
    if (compressed) {
        detail::check_compression(*entry.options.compression, detail::model_bitpix(entry.image),
                                  entry.image.geometry.axes, index == 0 ? 1 : index);
        if (index == 0) {
            // A compressed image is an extension, after a primary HDU without data (standard §10.1).
            (void)add_header({});
            state_->units.front().hosts_compressed_primary = true;
            state_->units.push_back(std::move(entry));
            return 1;
        }
    }
    state_->units.push_back(std::move(entry));
    return index;
}

std::size_t writer::add_image(image_info info, numeric_type type, std::span<const std::byte> data,
                              image_write_options options)
{
    const std::size_t size = size_of(type);
    if (data.size() % size != 0 || std::bit_cast<std::uintptr_t>(data.data()) % size != 0) {
        throw usage_error(errc::invalid_argument,
                          "the data of the image of unit " + std::to_string(state_->units.size()) +
                              " do not hold whole values of their type at an address of their alignment",
                          {.hdu = state_->units.size()});
    }
    return add_values(std::move(info), type, data, {}, std::move(options));
}

std::size_t writer::add_image(image_info info, numeric_type type, image_fill fill, image_write_options options)
{
    if (!fill) {
        throw usage_error(errc::invalid_argument,
                          "the image of unit " + std::to_string(state_->units.size()) +
                              " has no function for its values",
                          {.hdu = state_->units.size()});
    }
    return add_values(std::move(info), type, {}, std::move(fill), std::move(options));
}

std::size_t writer::add_reserved_image(image_info info, reserved_image_options options)
{
    const std::size_t index = state_->units.size();
    image_info image = detail::resolve_image(std::move(info), std::nullopt, std::nullopt, index);
    const bool floating = static_cast<int>(detail::model_bitpix(image)) < 0;
    if (options.fill == reserved_fill::undefined_values && !floating && !image.blank) {
        throw usage_error(errc::invalid_argument,
                          "the reserved image of unit " + std::to_string(index) +
                              " holds undefined values of integer data, and has no BLANK to store them",
                          {.hdu = index, .keyword = "BLANK"});
    }
    state_->units.push_back(
        {.image = std::move(image), .options = {.reserved_cards = options.reserved_cards}, .reserved = options.fill});
    return index;
}

std::size_t writer::add_header(openfits::header cards, std::size_t reserved_cards)
{
    const std::size_t index = state_->units.size();
    image_info image;
    image.bitpix = bitpix::uint8;
    image.header = std::move(cards);
    state_->units.push_back({.image = std::move(image), .options = {.reserved_cards = reserved_cards}});
    return index;
}

std::size_t writer::add_table(table_info info, std::span<const column_values> values, table_write_options options)
{
    // A table is an extension: the file starts with a primary HDU without data, added once the table is resolved.
    const bool primary_first = state_->units.empty();
    const std::size_t index = state_->units.size() + (primary_first ? 1U : 0U);
    detail::table_model table = detail::resolve_table(std::move(info), values, options, index);
    if (primary_first) {
        (void)add_header({});
    }
    state_->units.push_back({.options = {.reserved_cards = options.reserved_cards,
                                         .overflow = options.overflow,
                                         .scaling = options.scaling,
                                         .compression = std::move(options.compression)},
                             .table = std::move(table)});
    return index;
}

std::size_t writer::add_table(table_info info, table_rows rows, const table_write_options& options)
{
    const bool primary_first = state_->units.empty();
    const std::size_t index = state_->units.size() + (primary_first ? 1U : 0U);
    if (!rows) {
        throw usage_error(errc::invalid_argument,
                          "the table of unit " + std::to_string(index) + " has no function for its rows",
                          {.hdu = index});
    }
    detail::table_model table = detail::resolve_streamed_table(std::move(info), options, index);
    if (primary_first) {
        (void)add_header({});
    }
    state_->units.push_back({.options = {.reserved_cards = options.reserved_cards,
                                         .overflow = options.overflow,
                                         .scaling = options.scaling},
                             .table = std::move(table),
                             .rows = std::move(rows)});
    return index;
}

std::size_t writer::add_hdu(const reader& source, std::size_t index, hdu_copy_options options)
{
    // An extension that holds no image comes after a primary HDU, added once the copy is planned.
    const bool primary_first = state_->units.empty() && detail::needs_primary_before(source.hdu(index));
    const std::size_t position = state_->units.size() + (primary_first ? 1U : 0U);
    detail::unit_copy copy =
        detail::plan_copy(source, detail::reader_access::source(source), index, position, std::move(options));
    if (primary_first) {
        (void)add_header({});
    }
    state_->units.push_back({.copy = std::move(copy)});
    return position;
}

std::size_t writer::hdu_count() const noexcept
{
    return state_->units.size();
}

void writer::save(std::string_view path, const write_options& options) const
{
    if (state_->units.empty()) {
        throw usage_error(errc::invalid_argument, "the writer has no unit to save");
    }
    check_gzip_level(options);
    file_sink sink(path, {.flush_to_disk = options.flush_to_disk, .replace = options.if_exists == if_exists::replace});
    save(sink, options);
}

std::optional<compression_options> writer::state::compression_of(std::size_t index, const write_options& options) const
{
    const unit& entry = units[index];
    if (entry.copy || entry.reserved || entry.rows) {
        return std::nullopt;
    }
    const std::optional<compression_options>& chosen =
        entry.options.compression ? entry.options.compression : options.compression;
    if (!chosen || chosen->type == compression::none) {
        return std::nullopt;
    }
    if (entry.table) {
        // A binary table; an ASCII table is never compressed (standard §10.3).
        return entry.table->info.kind == hdu_kind::bintable ? chosen : std::nullopt;
    }
    if ((entry.data.empty() && !entry.fill) || entry.image.geometry.element_count() == 0) {
        return std::nullopt;
    }
    return chosen;
}

void writer::state::check_streams(const write_options& options) const
{
    // A first image that the options of the file compress comes after a primary HDU that the save adds.
    const std::size_t shift = !units.empty() && compression_of(0, options) ? 1U : 0U;
    for (std::size_t i = 0; i < units.size(); ++i) {
        const unit& model = units[i];
        const bool streamed =
            (static_cast<bool>(model.fill) || static_cast<bool>(model.rows)) && !compression_of(i, options).has_value();
        const bool completed = model.rows && model.table && (model.table->info.rows == 0 || has_heap(*model.table));
        if ((options.checksums && streamed) || completed) {
            throw usage_error(errc::invalid_argument,
                              "the header of unit " + std::to_string(i + shift) +
                                  " is completed once its streamed data are written, which needs a sink that can "
                                  "rewrite",
                              {.hdu = i + shift});
        }
    }
}

std::uint64_t writer::state::compressed_bytes(const write_options& options) const
{
    std::uint64_t bytes = 0;
    for (std::size_t i = 0; i < units.size(); ++i) {
        if (compression_of(i, options)) {
            const unit& entry = units[i];
            bytes += entry.table ? (entry.table->info.rows * entry.table->info.row_size) + entry.table->heap_size
                                 : entry.image.geometry.element_count() *
                                       detail::stored_size(detail::model_bitpix(entry.image));
        }
    }
    return bytes;
}

writer::state::formatted_file writer::state::format(const write_options& options,
                                                    const std::function<void(std::uint64_t)>& advance) const
{
    if (units.empty()) {
        throw usage_error(errc::invalid_argument, "the writer has no unit to save");
    }
    formatted_file file;
    file.units.reserve(units.size() + 1);
    for (std::size_t i = 0; i < units.size(); ++i) {
        const std::optional<compression_options> compression = compression_of(i, options);
        if (i == 0 && compression) {
            // A first image that the options of the file compress: a primary HDU without data before it.
            file.units.push_back({});
        }
        file_unit entry{.source = i};
        const unit& model = units[i];
        if (compression && model.table) {
            // The data of the table as an uncompressed table holds them, then compressed column by column.
            memory_sink rows;
            detail::write_table_data(rows, *model.table, [](std::uint64_t) {}, file.units.size());
            const std::uint64_t size = (model.table->info.rows * model.table->info.row_size) + model.table->heap_size;
            entry.compressed = detail::compress_table(*model.table, rows.data().first(static_cast<std::size_t>(size)),
                                                      *compression, file.units.size(), advance);
        } else if (compression) {
            const std::size_t position = file.units.size();
            const detail::write_conversion conversion = conversion_of(model.image, model.type, model.options, position);
            const detail::image_values values = model.fill ? detail::streamed_values(model.fill, model.type)
                                                           : detail::borrowed_values(model.data, model.type);
            // The first image of the writer is the primary one, after the primary HDU that a save or the writer added.
            const file_unit& first = file.units.front();
            const bool primary = position == 1 && (!first.source || units[*first.source].hosts_compressed_primary);
            entry.compressed = detail::compress_image(model.image, conversion, model.type, values, *compression,
                                                      primary, position, advance);
        }
        file.units.push_back(std::move(entry));
    }
    std::vector<std::string>& headers = file.headers;
    headers.reserve(file.units.size());
    const std::vector<card> checksums = checksum_cards_of(options);
    const auto closing_of = [&checksums](bool sealed) {
        return sealed ? std::span<const card>(checksums) : std::span<const card>();
    };
    for (std::size_t p = 0; p < file.units.size(); ++p) {
        file_unit& entry = file.units[p];
        const unit* model = model_of(entry);
        if (model != nullptr && model->copy) {
            // A copy keeps its header as it is when it can, and its checksums with it.
            const detail::unit_copy& copy = *model->copy;
            if (detail::header_as_is(copy, records_of(options, p), options.checksums)) {
                headers.push_back(detail::source_header(copy));
            } else {
                entry.sealed = options.checksums || detail::has_checksum_cards(copy);
                headers.push_back(format_header(entry, p, options, closing_of(entry.sealed)));
            }
            entry.data = detail::copy_data_size(copy);
        } else {
            entry.sealed = options.checksums;
            headers.push_back(format_header(entry, p, options, closing_of(entry.sealed)));
            if (entry.compressed) {
                entry.data = detail::data_records(entry.compressed->data.size());
            } else if (model == nullptr) {
                entry.data = 0;
            } else if (model->table) {
                const detail::table_model& table = *model->table;
                // A streamed table whose rows or heap are known once written has a size known then.
                if (!model->rows || (table.info.rows != 0 && !has_heap(table))) {
                    entry.data = detail::padded_table_size(table);
                }
            } else {
                entry.data =
                    detail::padded_data_size(detail::model_bitpix(model->image), model->image.geometry.element_count());
            }
        }
        file.size += headers.back().size() + entry.data.value_or(0);
        file.data += entry.data.value_or(0);
        file.size_known = file.size_known && entry.data.has_value();
    }
    return file;
}

std::string writer::state::table_header(const detail::table_model& table, const unit& model, std::size_t position,
                                        const write_options& options, std::span<const card> closing)
{
    const detail::emission_options emission{.records = records_of(options, position),
                                            .longstrn = options.longstrn,
                                            .reserved_cards = model.options.reserved_cards,
                                            .closing = closing};
    return detail::header_blocks(detail::table_cards(table), table.info.header, emission);
}

std::string writer::state::format_header(const file_unit& entry, std::size_t position, const write_options& options,
                                         std::span<const card> closing) const
{
    const unit* model = model_of(entry);
    if (model == nullptr) {
        image_info empty;
        empty.bitpix = bitpix::uint8;
        const detail::emission_options emission{.records = records_of(options, position), .closing = closing};
        return detail::header_blocks(detail::image_cards(empty, true), header{}, emission);
    }
    detail::emission_options emission{.records = records_of(options, position),
                                      .longstrn = options.longstrn,
                                      .reserved_cards = model->options.reserved_cards,
                                      .closing = closing};
    if (model->copy) {
        // The header written anew keeps the fill space of the cards it writes.
        emission.reserved_cards = detail::copy_fill(*model->copy);
        return detail::header_blocks(detail::copy_records(*model->copy), detail::copy_cards(*model->copy), emission);
    }
    if (entry.compressed) {
        return detail::header_blocks(entry.compressed->cards,
                                     model->table ? model->table->info.header : model->image.header, emission);
    }
    if (model->table) {
        return table_header(*model->table, *model, position, options, closing);
    }
    return detail::header_blocks(detail::image_cards(model->image, position == 0), model->image.header, emission);
}

std::optional<detail::streamed_table> writer::state::write_data(output_sink& sink, const file_unit& entry,
                                                                std::size_t position,
                                                                const std::function<void(std::uint64_t)>& advance) const
{
    const unit* model = model_of(entry);
    if (model == nullptr) {
        return std::nullopt;
    }
    if (entry.compressed) {
        const std::vector<std::byte>& data = entry.compressed->data;
        sink.write(data);
        // The zeros of the last block, fewer than a block, written as data as the fill of every unit is.
        const std::vector<std::byte> fill(detail::data_records(data.size()) - data.size(), std::byte{0});
        sink.write(fill);
        advance(data.size() + fill.size());
        return std::nullopt;
    }
    if (model->copy) {
        detail::write_copy_data(sink, *model->copy, advance);
        return std::nullopt;
    }
    if (model->table) {
        if (model->rows) {
            return detail::write_streamed_table(sink, *model->table, model->rows, advance, position);
        }
        detail::write_table_data(sink, *model->table, advance, position);
        return std::nullopt;
    }
    const image_info& image = model->image;
    if (model->reserved) {
        detail::write_reserved_data(sink, detail::model_bitpix(image), image.geometry.element_count(),
                                    *model->reserved == reserved_fill::undefined_values, image.blank, advance);
    } else if (!model->data.empty() || model->fill) {
        const detail::image_values values = model->fill ? detail::streamed_values(model->fill, model->type)
                                                        : detail::borrowed_values(model->data, model->type);
        detail::write_image_data(sink, conversion_of(image, model->type, model->options, position), model->type,
                                 image.geometry.element_count(), row_of(image), values, advance,
                                 model->fill ? detail::stream_transfer : detail::write_transfer);
    }
    return std::nullopt;
}

std::uint64_t writer::size(const write_options& options) const
{
    const state::formatted_file file = state_->format(options);
    if (!file.size_known) {
        throw usage_error(errc::invalid_argument,
                          "the file holds a streamed table whose size is known once its rows are written");
    }
    return file.size;
}

std::size_t writer::header_records(std::size_t index, const write_options& options) const
{
    if (index >= state_->units.size()) {
        throw usage_error(errc::hdu_not_found, "the writer has no unit " + std::to_string(index), {.hdu = index});
    }
    // The header of a compressed unit follows from its tiles, and units lie one further after a compressed first
    // image.
    const state::formatted_file file = state_->format(options);
    std::string formatted;
    for (std::size_t p = 0; p < file.units.size(); ++p) {
        if (file.units[p].source == index) {
            formatted = file.headers[p];
        }
    }
    // The fill after END is spaces, so that the record of END holds the last character that is not one.
    return (formatted.find_last_not_of(' ') / detail::record_length) + 1;
}

void writer::state::save(output_sink& sink, const write_options& options) const
{
    // A sink that cannot rewrite is refused first for the headers that need it, those of streamed data completed once
    // the data are written: their checksums, which a pass of their own before the file cannot take from a stream, and
    // the NAXIS2 or the heap of a streamed table.
    if (!sink.can_rewrite()) {
        check_streams(options);
    }
    // The units compressed and every header formatted then, so that a model whose header cannot be written writes
    // nothing.
    save_progress progress(options.progress, compressed_bytes(options));
    progress.report();
    const auto advance = [&progress](std::uint64_t bytes) { progress.advance(bytes); };
    formatted_file file = format(options, advance);
    std::vector<std::string>& headers = file.headers;
    const std::size_t count = file.units.size();

    // The checksums of a unit need the sum of its data, which DATASUM records and CHECKSUM completes (standard
    // §4.4.2.7): taken as the data are written, the two cards then rewritten in place, or in a pass of their own before
    // anything is written when the sink cannot rewrite. A streamed table whose rows or heap are known once written has
    // its header completed in place too.
    const bool rewrite = sink.can_rewrite();
    const std::string comment = checksum_comment(options);
    std::uint64_t summed = 0;
    if (!rewrite) {
        for (const file_unit& entry : file.units) {
            summed += entry.sealed ? entry.data.value_or(0) : 0;
        }
    }
    progress.add_to_total(file.size + summed);
    if (!rewrite) {
        for (std::size_t i = 0; i < count; ++i) {
            if (file.units[i].sealed) {
                detail::summing_sink data;
                (void)write_data(data, file.units[i], i, advance);
                (void)detail::seal_checksums(headers[i], data.sum(), comment, records_of(options, i));
            }
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        const file_unit& entry = file.units[i];
        const std::uint64_t start = sink.position();
        sink.write(std::as_bytes(std::span(headers[i])));
        progress.advance(headers[i].size());
        if (!rewrite || (!entry.sealed && !completes_table(entry))) {
            (void)write_data(sink, entry, i, advance);
            continue;
        }
        detail::summing_sink data(&sink);
        const std::optional<detail::streamed_table> written = write_data(data, entry, i, advance);
        std::string& blocks = headers[i];
        detail::byte_range changed{.offset = 0, .size = blocks.size()};
        const unit* model = model_of(entry);
        if (written && model != nullptr && model->table) {
            // The header of the table as its rows made it: their number, the heap and the emax of each column of
            // variable-length arrays, in records of the same places.
            const std::vector<card> closing = entry.sealed ? checksum_cards_of(options) : std::vector<card>();
            std::string completed =
                table_header(detail::completed_table(*model->table, *written, i), *model, i, options, closing);
            if (completed.size() != blocks.size()) {
                throw validation_error(errc::value_too_long,
                                       "the header of unit " + std::to_string(i) +
                                           " takes more blocks once the rows of its table are written",
                                       {.hdu = i});
            }
            blocks = std::move(completed);
        }
        if (entry.sealed) {
            const std::uint32_t sum = detail::add_sums(data.sum(), written ? written->rows_sum : 0);
            const detail::byte_range sealed = detail::seal_checksums(blocks, sum, comment, records_of(options, i));
            if (!written) {
                changed = sealed;
            }
        }
        sink.rewrite(start + changed.offset, std::as_bytes(std::span(blocks)).subspan(changed.offset, changed.size));
    }
    sink.finish();
}

void writer::save(output_sink& sink, const write_options& options) const
{
    check_gzip_level(options);
    if (options.gzip_level) {
        // The whole file is one gzip member, which the sink compresses as it is written; it cannot rewrite.
        gzip_sink compressed(sink, *options.gzip_level);
        state_->save(compressed, options);
        return;
    }
    state_->save(sink, options);
}

} // namespace openfits
