// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "header/emission.h"

#include <openfits/error.h>

#include "core/quote.h"
#include "header/keywords.h"
#include "header/long_string.h"

#include <cstdint>
#include <vector>

namespace openfits::detail {

namespace {

constexpr std::size_t block_size = 2880;

void append_records(std::string& blocks, const card& value, const record_options& options)
{
    for (const std::string& record : card_records(value, options)) {
        blocks += record;
    }
}

} // namespace

std::string header_blocks(std::span<const card> structural, const header& caller, const emission_options& options)
{
    std::string blocks;
    for (const card& value : structural) {
        append_records(blocks, value, options.records);
    }

    // The caller's cards, without the fill space that ends them. CONTINUE records read from a file are their own,
    // continuing a string or commentary.
    const std::size_t end = caller.size() - caller.free_cards();
    std::string records;
    bool long_strings = false;
    for (std::size_t i = 0; i < end; ++i) {
        const card& value = caller[i];
        if (value.keyword() != "CONTINUE" && is_structural_keyword(value.keyword())) {
            throw validation_error(errc::structural_keyword,
                                   "the keyword " + quote(value.keyword()) +
                                       " is written from the model of the unit, and cannot be given in its header",
                                   {.hdu = options.records.hdu, .keyword = std::string(value.keyword())});
        }
        const std::vector<std::string> card_text = card_records(value, options.records);
        long_strings = long_strings || card_text.size() > 1 || is_continuation(value);
        for (const std::string& record : card_text) {
            records += record;
        }
    }
    if (options.longstrn && long_strings && !caller.contains("LONGSTRN")) {
        append_records(blocks, card("LONGSTRN", "OGIP 1.0", "CONTINUE records may continue long strings"),
                       options.records);
    }
    blocks += records;

    std::string blank(record_length, ' ');
    for (std::size_t i = 0; i < options.reserved_cards; ++i) {
        blocks += blank;
    }
    std::string end_record = "END";
    end_record.resize(record_length, ' ');
    blocks += end_record;
    blocks.resize((blocks.size() + block_size - 1) / block_size * block_size, ' ');
    return blocks;
}

} // namespace openfits::detail
