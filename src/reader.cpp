// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/reader.h>

#include "core/ascii.h"
#include "core/quote.h"
#include "data/image_description.h"
#include "data/image_read.h"
#include "hdu/section.h"
#include "hdu/walk.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

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

[[noreturn]] void throw_not_found(const std::string& what)
{
    throw usage_error(errc::hdu_not_found, "the file has no " + what);
}

} // namespace

struct reader::state
{
    state(std::unique_ptr<input_source> source, const read_options& options)
        : walk(std::move(source), options.limits, options.strict), limits(options.limits)
    {}

    detail::unit_walk walk;
    openfits::limits limits;
    std::size_t selected = 0;
    std::optional<region> section{};
};

reader::reader(std::string_view path, read_options options) : reader(std::make_unique<file_source>(path), options) {}

reader::reader(std::unique_ptr<input_source> source, read_options options)
    : state_(std::make_unique<state>(std::move(source), options))
{}

reader::reader(const file_specification& specification, read_options options) : reader(specification.path, options)
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

std::optional<std::size_t> reader::first_image() const
{
    return state_->walk.find(holds_image);
}

std::optional<std::size_t> reader::first_table() const
{
    return state_->walk.find([](const hdu_info& unit) {
        const hdu_kind kind = unit.kind();
        return kind == hdu_kind::table || kind == hdu_kind::bintable || kind == hdu_kind::compressed_table;
    });
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
    return detail::read_pixels(state_->walk.source(), unit, detail::describe_image(unit, false), type, destination,
                               options);
}

} // namespace openfits
