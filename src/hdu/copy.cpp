// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "hdu/copy.h"

#include <openfits/error.h>

#include "checksum/checksum.h"
#include "core/ascii.h"
#include "core/checked_math.h"
#include "core/scratch_buffer.h"
#include "header/keywords.h"
#include "header/long_string.h"

#include <algorithm>
#include <span>
#include <string_view>
#include <utility>

namespace openfits::detail {

namespace {

constexpr std::uint64_t block_size = 2880;
// The most bytes of a unit that a copy reads at once.
constexpr std::uint64_t copy_transfer = staging_size;

bool is_checksum_keyword(std::string_view keyword) noexcept
{
    return keyword == "CHECKSUM" || keyword == "DATASUM";
}

// The cards of the structural keywords of a header but CHECKSUM and DATASUM, each with the CONTINUE records that
// continue its string.
std::vector<card> structural_cards_of(const header& cards)
{
    std::vector<card> kept;
    const std::span<const card> all = cards.cards();
    for (std::size_t i = 0; i < all.size(); ++i) {
        const std::size_t continued = continuation_length(all, i);
        const std::string_view keyword = all[i].keyword();
        if (keyword != "CONTINUE" && is_structural_keyword(keyword) && !is_checksum_keyword(keyword)) {
            kept.insert(kept.end(), all.begin() + static_cast<std::ptrdiff_t>(i),
                        all.begin() + static_cast<std::ptrdiff_t>(i + continued + 1));
        }
        i += continued;
    }
    return kept;
}

// The first card of the keyword among cards, or nothing.
const card* find_card(std::span<const card> cards, std::string_view keyword)
{
    const auto found = std::ranges::find_if(cards, [keyword](const card& value) { return value.keyword() == keyword; });
    return found == cards.end() ? nullptr : &*found;
}

} // namespace

bool needs_primary_before(const hdu_info& unit) noexcept
{
    return unit.kind() != hdu_kind::primary && unit.kind() != hdu_kind::random_groups && unit.kind() != hdu_kind::image;
}

copy_form copy_form_at(const hdu_info& unit, std::size_t position)
{
    switch (unit.kind()) {
    case hdu_kind::random_groups:
        if (position > 0) {
            throw usage_error(errc::invalid_argument,
                              "the random groups of unit " + std::to_string(unit.index()) +
                                  " are the primary HDU of a file (standard §6), and cannot follow unit " +
                                  std::to_string(position - 1),
                              {.hdu = unit.index()});
        }
        return copy_form::as_is;
    case hdu_kind::primary:
        return position > 0 ? copy_form::to_extension : copy_form::as_is;
    case hdu_kind::image:
        return position == 0 ? copy_form::to_primary : copy_form::as_is;
    default:
        return copy_form::as_is;
    }
}

unit_copy plan_copy(const reader& source, const thread_safe_source& bytes, std::size_t index, std::size_t position,
                    hdu_copy_options options)
{
    const hdu_info& unit = source.hdu(index);
    if (!unit.data_size()) {
        throw invalid_data_error(errc::invalid_structure,
                                 "the header of unit " + std::to_string(index) + " gives no size for its data",
                                 {.hdu = index});
    }
    const copy_form form = copy_form_at(unit, position);
    return {.source = &bytes, .unit = &unit, .form = form, .cards = std::move(options.header)};
}

bool header_as_is(const unit_copy& copy, const record_options& records, bool checksums)
{
    if (copy.form != copy_form::as_is || copy.cards || checksums) {
        return false;
    }
    const hdu_info& unit = *copy.unit;
    if (unit.header_size() % block_size != 0 || std::ranges::any_of(unit.diagnostics(), [](const diagnostic& found) {
            return found.code == errc::missing_end;
        })) {
        return false;
    }
    return records.non_ascii == byte_policy::keep ||
           std::ranges::all_of(unit.header(), [](const card& value) { return is_printable(value.text()); });
}

std::string source_header(const unit_copy& copy)
{
    std::string blocks(checked_cast<std::size_t>(copy.unit->header_size()), ' ');
    copy.source->read(copy.unit->header_offset(), std::as_writable_bytes(std::span(blocks)));
    return blocks;
}

std::vector<card> copy_records(const unit_copy& copy)
{
    // The structural cards when the copy gives the others, and every card otherwise, in their order, without the fill
    // space that ends them, which the writer writes again.
    std::vector<card> kept;
    if (copy.cards) {
        kept = structural_cards_of(copy.unit->header());
    } else {
        const header& cards = copy.unit->header();
        const std::span<const card> all = cards.cards().first(cards.size() - cards.free_cards());
        for (const card& value : all) {
            if (!is_checksum_keyword(value.keyword())) {
                kept.push_back(value);
            }
        }
    }
    if (copy.form == copy_form::as_is) {
        return kept;
    }
    // The mandatory keywords of the place the copy takes (standard §4.4.1), the records of BITPIX, NAXIS and NAXISn
    // kept, then the other cards in their order, as CFITSIO converts a unit that it copies.
    const std::span<const card> all(kept);
    std::vector<card> converted;
    if (copy.form == copy_form::to_extension) {
        converted.emplace_back("XTENSION", "IMAGE", "image extension");
    } else {
        converted.emplace_back("SIMPLE", true, "conforms to the FITS standard");
    }
    std::vector<std::string> mandatory{"BITPIX", "NAXIS"};
    const std::uint64_t axes = copy.unit->header().find<std::uint64_t>("NAXIS").value_or(0);
    for (std::uint64_t n = 1; n <= axes; ++n) {
        mandatory.push_back("NAXIS" + std::to_string(n));
    }
    for (const std::string& keyword : mandatory) {
        if (const card* found = find_card(all, keyword)) {
            converted.push_back(*found);
        }
    }
    if (copy.form == copy_form::to_extension) {
        converted.emplace_back("PCOUNT", 0, "no parameters");
        converted.emplace_back("GCOUNT", 1, "one array");
    } else {
        converted.emplace_back("EXTEND", true, "extensions may follow");
    }
    for (const card& value : all) {
        const std::string_view keyword = value.keyword();
        const bool replaced = keyword == "SIMPLE" || keyword == "XTENSION" || keyword == "EXTEND" ||
                              keyword == "PCOUNT" || keyword == "GCOUNT" || keyword == "GROUPS" ||
                              std::ranges::find(mandatory, keyword) != mandatory.end();
        if (!replaced) {
            converted.push_back(value);
        }
    }
    return converted;
}

header copy_cards(const unit_copy& copy)
{
    return copy.cards.value_or(header());
}

std::size_t copy_fill(const unit_copy& copy) noexcept
{
    return copy.cards ? copy.cards->free_cards() : copy.unit->header().free_cards();
}

bool has_checksum_cards(const unit_copy& copy)
{
    const header& cards = copy.unit->header();
    return cards.contains("CHECKSUM") || cards.contains("DATASUM");
}

std::uint64_t copy_data_size(const unit_copy& copy) noexcept
{
    return data_records(copy.unit->data_size().value_or(0));
}

void write_copy_data(output_sink& sink, const unit_copy& copy, const std::function<void(std::uint64_t)>& advance)
{
    const hdu_info& unit = *copy.unit;
    const thread_safe_source& source = *copy.source;
    const std::uint64_t data = unit.data_size().value_or(0);
    const std::uint64_t records = data_records(data);
    // The data must be there, which a read beyond the file refuses; the fill of their last block is taken as far as
    // the file holds it.
    const std::uint64_t start = unit.data_offset();
    const std::uint64_t available = start >= source.size() ? 0 : source.size() - start;
    const std::uint64_t present = std::max(data, std::min(records, available));
    const std::span<const std::byte> held = source.bytes();
    std::optional<scratch_buffer> staging;
    for (std::uint64_t done = 0; done < present;) {
        const std::uint64_t piece = std::min(copy_transfer, present - done);
        if (!held.empty() && start + done + piece <= held.size()) {
            sink.write(held.subspan(checked_cast<std::size_t>(start + done), checked_cast<std::size_t>(piece)));
        } else {
            if (!staging) {
                staging.emplace(checked_cast<std::size_t>(std::min(copy_transfer, present)));
            }
            const std::span<std::byte> bytes = staging->bytes().first(checked_cast<std::size_t>(piece));
            source.read(start + done, bytes);
            sink.write(bytes);
        }
        advance(piece);
        done += piece;
    }
    if (present < records) {
        const std::uint64_t missing = records - present;
        if (unit.kind() == hdu_kind::table) {
            const std::vector<std::byte> spaces(checked_cast<std::size_t>(missing), static_cast<std::byte>(' '));
            sink.write(spaces);
        } else {
            sink.write_zeros(missing);
        }
        advance(missing);
    }
}

} // namespace openfits::detail
