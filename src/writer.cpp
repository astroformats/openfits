// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>
#include <openfits/writer.h>

#include "checksum/checksum.h"
#include "compress/image_compress.h"
#include "compress/table_tiles.h"
#include "data/conversion.h"
#include "data/image_write.h"
#include "header/emission.h"
#include "header/record.h"
#include "table/table_write.h"

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

    void advance(std::uint64_t bytes)
    {
        done_ += bytes;
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
    // A unit as it is added: its model, and the values borrowed until the last save; a table instead of an image when
    // it has one.
    struct unit
    {
        image_info image{};
        numeric_type type = numeric_type::uint8;
        std::span<const std::byte> data{};
        image_write_options options{};
        std::optional<detail::table_model> table{};
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
    };

    // The units of the file of a save, their headers formatted, the bytes of the file, and those of the data of its
    // units.
    struct formatted_file
    {
        std::vector<file_unit> units{};
        std::vector<std::string> headers{};
        std::uint64_t size = 0;
        std::uint64_t data = 0;
    };

    std::vector<unit> units{};

    // The compression of the unit at index: its own, or else that of options; none for a unit that has no data to
    // compress, an image without values or a table.
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

    // Writes the data of a unit of the file into sink, the fill of the last block included.
    void write_data(output_sink& sink, const file_unit& entry, std::size_t position,
                    const std::function<void(std::uint64_t)>& advance) const;
};

namespace {

// The forms of the records of the unit at index that options allow.
detail::record_options records_of(const write_options& options, std::size_t index)
{
    return {.hierarch = options.hierarch,
            .truncate_comment = options.truncate_comments,
            .continue_reserved = options.continue_reserved_keywords,
            .hdu = index};
}

// The comment of the checksum cards: the time of the checksums, or none.
std::string checksum_comment(const write_options& options)
{
    return options.checksum_time ? format_date(*options.checksum_time) : std::string();
}

// The checksum cards that options ask for, which hold placeholders that a save completes once it has the sum of the
// data of each unit.
std::vector<card> checksum_cards_of(const write_options& options)
{
    return options.checksums ? detail::checksum_cards(checksum_comment(options)) : std::vector<card>();
}

} // namespace

writer::writer() : state_(std::make_unique<state>()) {}

writer::writer(writer&& other) noexcept = default;
writer& writer::operator=(writer&& other) noexcept = default;
writer::~writer() = default;

std::size_t writer::add_values(image_info info, numeric_type type, std::span<const std::byte> data,
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
    image_info image = detail::resolve_image(std::move(info), type, data.size() / size_of(type), index);
    const bool compressed =
        options.compression && options.compression->type != compression::none && image.geometry.element_count() > 0;
    if (compressed) {
        detail::check_compression(*options.compression, detail::model_bitpix(image), image.geometry.axes,
                                  index == 0 ? 1 : index);
        if (index == 0) {
            // A compressed image is an extension, after a primary HDU without data (standard §10.1).
            (void)add_header({});
            state_->units.front().hosts_compressed_primary = true;
            state_->units.push_back(
                {.image = std::move(image), .type = type, .data = data, .options = std::move(options)});
            return 1;
        }
    }
    state_->units.push_back({.image = std::move(image), .type = type, .data = data, .options = std::move(options)});
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
    return add_values(std::move(info), type, data, std::move(options));
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
    if (state_->units.empty()) {
        // A table is an extension: the file starts with a primary HDU without data.
        (void)add_header({});
    }
    const std::size_t index = state_->units.size();
    detail::table_model table = detail::resolve_table(std::move(info), values, options, index);
    state_->units.push_back({.options = {.reserved_cards = options.reserved_cards,
                                         .overflow = options.overflow,
                                         .scaling = options.scaling,
                                         .compression = std::move(options.compression)},
                             .table = std::move(table)});
    return index;
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
    file_sink sink(path, {.flush_to_disk = options.flush_to_disk, .replace = options.if_exists == if_exists::replace});
    save(sink, options);
}

std::optional<compression_options> writer::state::compression_of(std::size_t index, const write_options& options) const
{
    const unit& entry = units[index];
    const std::optional<compression_options>& chosen =
        entry.options.compression ? entry.options.compression : options.compression;
    if (!chosen || chosen->type == compression::none) {
        return std::nullopt;
    }
    if (entry.table) {
        // A binary table; an ASCII table is never compressed (standard §10.3).
        return entry.table->info.kind == hdu_kind::bintable ? chosen : std::nullopt;
    }
    if (entry.data.empty() || entry.image.geometry.element_count() == 0) {
        return std::nullopt;
    }
    return chosen;
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
            scaling applied = model.image.scaling;
            if (model.options.scaling == scaling_policy::raw) {
                applied = {};
            } else if (model.options.scaling == scaling_policy::override) {
                applied = model.options.override_scaling;
            }
            const detail::write_conversion conversion =
                detail::make_write_conversion(model.type, detail::model_bitpix(model.image), applied, model.image.blank,
                                              model.options.overflow, position);
            // The first image of the writer is the primary one, after the primary HDU that a save or the writer added.
            const file_unit& first = file.units.front();
            const bool primary = position == 1 && (!first.source || units[*first.source].hosts_compressed_primary);
            entry.compressed = detail::compress_image(model.image, conversion, model.type, model.data, *compression,
                                                      primary, position, advance);
        }
        file.units.push_back(std::move(entry));
    }
    std::vector<std::string>& headers = file.headers;
    headers.reserve(file.units.size());
    const std::vector<card> checksums = checksum_cards_of(options);
    for (std::size_t p = 0; p < file.units.size(); ++p) {
        const file_unit& entry = file.units[p];
        headers.push_back(format_header(entry, p, options, checksums));
        std::uint64_t data = 0;
        if (entry.compressed) {
            data = detail::data_records(entry.compressed->data.size());
        } else if (entry.source) {
            const unit& model = units[*entry.source];
            data = model.table ? detail::padded_table_size(*model.table)
                               : detail::padded_data_size(detail::model_bitpix(model.image),
                                                          model.image.geometry.element_count());
        }
        file.size += headers.back().size() + data;
        file.data += data;
    }
    return file;
}

std::string writer::state::format_header(const file_unit& entry, std::size_t position, const write_options& options,
                                         std::span<const card> closing) const
{
    if (!entry.source) {
        image_info empty;
        empty.bitpix = bitpix::uint8;
        const detail::emission_options emission{.records = records_of(options, position), .closing = closing};
        return detail::header_blocks(detail::image_cards(empty, true), header{}, emission);
    }
    const unit& model = units[*entry.source];
    const detail::emission_options emission{.records = records_of(options, position),
                                            .longstrn = options.longstrn,
                                            .reserved_cards = model.options.reserved_cards,
                                            .closing = closing};
    if (entry.compressed) {
        return detail::header_blocks(entry.compressed->cards,
                                     model.table ? model.table->info.header : model.image.header, emission);
    }
    if (model.table) {
        return detail::header_blocks(detail::table_cards(*model.table), model.table->info.header, emission);
    }
    return detail::header_blocks(detail::image_cards(model.image, position == 0), model.image.header, emission);
}

void writer::state::write_data(output_sink& sink, const file_unit& entry, std::size_t position,
                               const std::function<void(std::uint64_t)>& advance) const
{
    if (!entry.source) {
        return;
    }
    if (entry.compressed) {
        const std::vector<std::byte>& data = entry.compressed->data;
        sink.write(data);
        const std::vector<std::byte> fill(detail::data_records(data.size()) - data.size(), std::byte{0});
        sink.write(fill);
        advance(data.size() + fill.size());
        return;
    }
    const unit& model = units[*entry.source];
    if (model.table) {
        detail::write_table_data(sink, *model.table, advance, position);
    } else if (!model.data.empty()) {
        // Stored values convert into the type of BITPIX alone, and the values of an override by its scaling, which
        // the header does not give.
        scaling applied = model.image.scaling;
        if (model.options.scaling == scaling_policy::raw) {
            applied = {};
        } else if (model.options.scaling == scaling_policy::override) {
            applied = model.options.override_scaling;
        }
        const detail::write_conversion conversion =
            detail::make_write_conversion(model.type, detail::model_bitpix(model.image), applied, model.image.blank,
                                          model.options.overflow, position);
        detail::write_image_data(sink, conversion, model.type, model.data, advance);
    }
}

std::uint64_t writer::size(const write_options& options) const
{
    return state_->format(options).size;
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

void writer::save(output_sink& sink, const write_options& options) const
{
    // The units compressed and every header formatted first, so that a model whose header cannot be written writes
    // nothing.
    save_progress progress(options.progress, state_->compressed_bytes(options));
    progress.report();
    const auto advance = [&progress](std::uint64_t bytes) { progress.advance(bytes); };
    state::formatted_file file = state_->format(options, advance);
    std::vector<std::string>& headers = file.headers;
    const std::size_t count = file.units.size();

    // The checksums of a unit need the sum of its data, which DATASUM records and CHECKSUM completes (standard
    // §4.4.2.7): taken as the data are written, the two cards then rewritten in place, or in a pass of its own before
    // anything is written when the sink cannot rewrite.
    const bool rewrite = options.checksums && sink.can_rewrite();
    const bool summed_first = options.checksums && !rewrite;
    const std::string comment = checksum_comment(options);
    progress.add_to_total(file.size + (summed_first ? file.data : 0));
    if (summed_first) {
        for (std::size_t i = 0; i < count; ++i) {
            detail::summing_sink data;
            state_->write_data(data, file.units[i], i, advance);
            (void)detail::seal_checksums(headers[i], data.sum(), comment, records_of(options, i));
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t start = sink.position();
        sink.write(std::as_bytes(std::span(headers[i])));
        progress.advance(headers[i].size());
        if (!rewrite) {
            state_->write_data(sink, file.units[i], i, advance);
            continue;
        }
        detail::summing_sink data(&sink);
        state_->write_data(data, file.units[i], i, advance);
        const detail::byte_range changed =
            detail::seal_checksums(headers[i], data.sum(), comment, records_of(options, i));
        sink.rewrite(start + changed.offset,
                     std::as_bytes(std::span(headers[i])).subspan(changed.offset, changed.size));
    }
    sink.finish();
}

} // namespace openfits
