// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>
#include <openfits/writer.h>

#include "data/conversion.h"
#include "data/image_write.h"
#include "header/emission.h"

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
    // A unit as it is added: its model, and the values borrowed until the last save.
    struct unit
    {
        image_info image{};
        numeric_type type = numeric_type::uint8;
        std::span<const std::byte> data{};
        image_write_options options{};
    };

    std::vector<unit> units{};
};

writer::writer() : state_(std::make_unique<state>()) {}

writer::writer(writer&& other) noexcept = default;
writer& writer::operator=(writer&& other) noexcept = default;
writer::~writer() = default;

std::size_t writer::add_values(image_info info, numeric_type type, std::span<const std::byte> data,
                               const image_write_options& options)
{
    const std::size_t index = state_->units.size();
    image_info image = detail::resolve_image(std::move(info), type, data.size() / size_of(type), index);
    state_->units.push_back({.image = std::move(image), .type = type, .data = data, .options = options});
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

void writer::save(output_sink& sink, const write_options& options) const
{
    const std::vector<state::unit>& units = state_->units;
    if (units.empty()) {
        throw usage_error(errc::invalid_argument, "the writer has no unit to save");
    }
    // Every header first, so that a model whose header cannot be written writes nothing.
    std::vector<std::string> headers;
    headers.reserve(units.size());
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < units.size(); ++i) {
        const state::unit& unit = units[i];
        const detail::emission_options emission{
            .records = {.hierarch = options.hierarch, .truncate_comment = options.truncate_comments, .hdu = i},
            .longstrn = options.longstrn,
            .reserved_cards = unit.options.reserved_cards};
        headers.push_back(detail::header_blocks(detail::image_cards(unit.image, i == 0), unit.image.header, emission));
        total += headers.back().size() +
                 detail::padded_data_size(detail::model_bitpix(unit.image), unit.image.geometry.element_count());
    }

    save_progress progress(options.progress, total);
    progress.report();
    const auto advance = [&progress](std::uint64_t bytes) { progress.advance(bytes); };
    for (std::size_t i = 0; i < units.size(); ++i) {
        const state::unit& unit = units[i];
        sink.write(std::as_bytes(std::span(headers[i])));
        progress.advance(headers[i].size());
        if (!unit.data.empty()) {
            const detail::write_conversion conversion =
                detail::make_write_conversion(unit.type, detail::model_bitpix(unit.image), unit.image.scaling,
                                              unit.image.blank, unit.options.overflow, i);
            detail::write_image_data(sink, conversion, unit.type, unit.data, advance);
        }
    }
    sink.finish();
}

} // namespace openfits
