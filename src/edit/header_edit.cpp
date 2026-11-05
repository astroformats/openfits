// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "edit/header_edit.h"

#include <openfits/checksum.h>
#include <openfits/error.h>

#include "checksum/checksum.h"
#include "compress/image_tiles.h"
#include "core/quote.h"
#include "header/keywords.h"
#include "header/long_string.h"

#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace openfits::detail {

namespace {

constexpr std::size_t block_size = 2880;
// The value of CHECKSUM while its unit is summed (standard Appendix J.1), from byte 12 of its record.
constexpr std::string_view placeholder = "0000000000000000";
constexpr std::size_t checksum_value = 11;

bool is_checksum_keyword(std::string_view keyword) noexcept
{
    return keyword == "CHECKSUM" || keyword == "DATASUM";
}

// A card that the library writes from the structure of a unit; a CONTINUE record belongs to the card it continues,
// and an orphaned one is commentary.
bool is_structural_card(const card& value) noexcept
{
    return value.keyword() != "CONTINUE" && is_structural_keyword(value.keyword());
}

struct structural_card
{
    std::string keyword{};
    card_value value{};
};

// The structural cards of a header but CHECKSUM and DATASUM, their long strings joined, and how many of them start the
// header, before any other card.
struct structure
{
    std::vector<structural_card> cards{};
    std::size_t leading = 0;
};

structure structure_of(const header& cards)
{
    structure result;
    const std::span<const card> all = cards.cards();
    bool leading = true;
    for (std::size_t i = 0; i < all.size(); ++i) {
        const std::size_t continued = continuation_length(all, i);
        const card& value = all[i];
        if (!is_structural_card(value)) {
            leading = false;
        } else if (!is_checksum_keyword(value.keyword())) {
            card_value held = value.value();
            if (held.kind() == value_kind::string && continued > 0) {
                held = card_value(joined_string(all, i));
            }
            result.cards.push_back({.keyword = std::string(value.keyword()), .value = std::move(held)});
            if (leading) {
                ++result.leading;
            }
        }
        i += continued;
    }
    return result;
}

[[noreturn]] void refuse_change(std::string_view keyword, std::uint64_t hdu)
{
    throw validation_error(errc::structural_keyword,
                           "the keyword " + quote(keyword) + " of unit " + std::to_string(hdu) +
                               " is written from the structure of the unit, which a change of its header cannot "
                               "change: its value and its place stay as they are (standard §4.4.1)",
                           {.hdu = hdu, .keyword = std::string(keyword)});
}

// The groups of cards of a header that are not structural, a card with the CONTINUE records of its string, before its
// fill space: their first index and their number of cards.
std::vector<std::pair<std::size_t, std::size_t>> user_groups(const header& cards)
{
    std::vector<std::pair<std::size_t, std::size_t>> groups;
    const std::span<const card> all = cards.cards();
    const std::size_t end = all.size() - cards.free_cards();
    for (std::size_t i = 0; i < end; ++i) {
        const std::size_t continued = continuation_length(all, i);
        if (!is_structural_card(all[i])) {
            groups.emplace_back(i, std::min(continued + 1, end - i));
        }
        i += continued;
    }
    return groups;
}

// The first card of keyword among cards, or nothing.
const card* first_card(const header& cards, std::string_view keyword)
{
    const std::optional<std::size_t> position = cards.index_of(keyword);
    return position ? &cards[*position] : nullptr;
}

} // namespace

void check_structure(const header& original, const header& edited, std::uint64_t hdu)
{
    const structure before = structure_of(original);
    const structure after = structure_of(edited);
    const std::size_t common = std::min(before.cards.size(), after.cards.size());
    for (std::size_t i = 0; i < common; ++i) {
        if (before.cards[i].keyword != after.cards[i].keyword || before.cards[i].value != after.cards[i].value) {
            refuse_change(before.cards[i].keyword, hdu);
        }
    }
    if (before.cards.size() != after.cards.size()) {
        refuse_change(before.cards.size() > common ? before.cards[common].keyword : after.cards[common].keyword, hdu);
    }
    if (after.leading < before.leading) {
        // Another card among the mandatory keywords that start the header (standard §4.4.1).
        refuse_change(after.cards[after.leading].keyword, hdu);
    }
}

std::string edited_blocks(const header& cards, const record_options& options, bool longstrn,
                          std::uint64_t minimum_blocks)
{
    std::string blocks;
    blocks.reserve((cards.size() + 1) * record_length);
    bool announce = longstrn && !cards.contains("LONGSTRN");
    for (const card& value : cards) {
        if (!value.text().empty()) {
            // The record that the card was read from, as it is.
            blocks += value.text();
            continue;
        }
        const std::vector<std::string> records = card_records(value, options);
        if (announce && records.size() > 1) {
            for (const std::string& record :
                 card_records(card("LONGSTRN", "OGIP 1.0", "CONTINUE records may continue long strings"), options)) {
                blocks += record;
            }
            announce = false;
        }
        for (const std::string& record : records) {
            blocks += record;
        }
    }
    // A header of fewer records than its blocks held keeps END in its last block, after blank cards: the fill space of
    // standard §4.4.2.4, since a header ends with the block of END.
    const std::uint64_t records_per_block = block_size / record_length;
    const std::uint64_t records = blocks.size() / record_length;
    if (minimum_blocks > 0 && records < (minimum_blocks - 1) * records_per_block) {
        const std::size_t blank = ((minimum_blocks - 1) * records_per_block) - records;
        blocks.append(blank * record_length, ' ');
    }
    std::string end_record = "END";
    end_record.resize(record_length, ' ');
    blocks += end_record;
    const std::uint64_t used = (blocks.size() + block_size - 1) / block_size;
    blocks.resize(std::max(used, minimum_blocks) * block_size, ' ');
    return blocks;
}

header with_checksum_cards(header cards, std::optional<std::uint32_t> datasum, bool checksum)
{
    if (datasum && cards.contains("DATASUM")) {
        const std::string comment = cards.comment("DATASUM").value_or("");
        cards.set("DATASUM", std::to_string(*datasum), comment);
    }
    if (checksum && cards.contains("CHECKSUM")) {
        const std::string comment = cards.comment("CHECKSUM").value_or("");
        cards.set("CHECKSUM", placeholder, comment);
    }
    return cards;
}

void seal_checksum(std::string& blocks, std::uint32_t data_sum, std::uint32_t total)
{
    std::size_t at = blocks.size();
    for (std::size_t offset = 0; offset + record_length <= blocks.size(); offset += record_length) {
        if (std::string_view(blocks.data() + offset, 8) == "CHECKSUM") {
            at = offset;
            break;
        }
    }
    if (at == blocks.size() ||
        std::string_view(blocks).substr(at + checksum_value, placeholder.size()) != placeholder) {
        throw usage_error(errc::invalid_argument, "the header has no record of CHECKSUM to complete");
    }
    // The blocks with the placeholder and the data sum to before; the value of CHECKSUM adds total less before, which
    // encode_checksum() encodes as the complement of its argument (standard Appendix J.2).
    const std::uint32_t before = add_sums(sum_of(std::as_bytes(std::span(blocks))), data_sum);
    const std::string value = encode_checksum(~add_sums(total, ~before));
    blocks.replace(at + checksum_value, value.size(), value);
}

header compressed_header(const header& table, const header& restored, const header& edited)
{
    // The cards of the table that the restored header copies as they are, in their order: those of its groups of cards
    // that are not structural, found in the cards of the table in the same order.
    std::vector<const card*> copied;
    for (const auto& [first, count] : user_groups(restored)) {
        for (std::size_t i = 0; i < count; ++i) {
            copied.push_back(&restored[first + i]);
        }
    }
    const std::span<const card> all = table.cards();
    const std::size_t end = all.size() - table.free_cards();
    std::vector<card> kept;
    std::optional<std::size_t> place;
    std::size_t next = 0;
    for (std::size_t i = 0; i < end; ++i) {
        if (next < copied.size() && all[i] == *copied[next]) {
            if (!place) {
                place = kept.size();
            }
            ++next;
            continue;
        }
        kept.push_back(all[i]);
    }

    // The cards of edited that are not structural, without its fill space.
    std::vector<card> changed;
    for (const auto& [first, count] : user_groups(edited)) {
        for (std::size_t i = 0; i < count; ++i) {
            changed.push_back(edited[first + i]);
        }
    }
    const auto at = kept.begin() + static_cast<std::ptrdiff_t>(place.value_or(kept.size()));
    kept.insert(at, changed.begin(), changed.end());

    // The checksums of the image or table that the unit holds, which edited gives as CHECKSUM and DATASUM, kept by the
    // table as ZHECKSUM and ZDATASUM.
    header result(std::move(kept));
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 2> sums{
        {{"CHECKSUM", "ZHECKSUM"}, {"DATASUM", "ZDATASUM"}}};
    for (const auto& [plain, zed] : sums) {
        const card* before = first_card(restored, plain);
        const card* after = first_card(edited, plain);
        if ((before == nullptr && after == nullptr) || (before != nullptr && after != nullptr && *before == *after)) {
            continue;
        }
        if (after == nullptr) {
            (void)result.erase(zed);
            continue;
        }
        const card replacement = renamed_card(*after, zed);
        if (const std::optional<std::size_t> position = result.index_of(zed)) {
            result.erase_at(*position);
            result.insert(*position, replacement);
        } else {
            result.append(replacement);
        }
    }
    for (std::size_t i = 0; i < edited.free_cards(); ++i) {
        result.append(card());
    }
    return result;
}

} // namespace openfits::detail
