// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Changing the keywords of a file in place. This program sets and erases keywords in the header of a unit of a FITS
// file, adds a HISTORY card, and commits the change into the file itself: the header is written over its own blocks
// when its cards still fit them, the blank cards before END taking the cards added, and otherwise the file is written
// again, to a copy that replaces it once it is complete. CHECKSUM and DATASUM, when the unit has them, are updated from
// the bytes that change, so that a unit that matched them still does. The unit is given in brackets after the path, as
// file.fits[1] or file.fits[SCI,2], and is the primary HDU without them. update_keywords() sets cards in one call.
//
// Usage: update_keyword <file>[<unit>] <KEYWORD>=<value text>|-<KEYWORD>...

#include <openfits/openfits.h>

#include "arguments.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

const char* state_name(openfits::checksum_state state)
{
    switch (state) {
    case openfits::checksum_state::valid:
        return "valid";
    case openfits::checksum_state::invalid:
        return "invalid";
    case openfits::checksum_state::absent:
        break;
    }
    return "absent";
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const std::vector<std::string> arguments = sample::arguments(argc, argv);
        if (arguments.size() < 2) {
            std::cerr << "usage: update_keyword <file>[<unit>] <KEYWORD>=<value text>|-<KEYWORD>...\n";
            return 2;
        }
        // The path and the unit, as file.fits[SCI,2] selects them. A path with brackets of its own, such as
        // "M31 [Ha].fits", goes to the constructor that takes a path, which takes it literally.
        openfits::editor file(openfits::file_specification::parse(arguments[0]));
        const std::size_t index = file.selected_hdu();
        const std::uint64_t header_size = file.file().hdu(index).header_size();
        const std::uint64_t file_size = file.file().size();

        // The header to change, which is valid until the commit.
        openfits::header& cards = file.header(index);
        for (std::size_t i = 1; i < arguments.size(); ++i) {
            const std::string& change = arguments[i];
            if (change.starts_with('-')) {
                if (!cards.erase(change.substr(1))) {
                    std::cerr << "update_keyword: unit " << index << " has no keyword " << change.substr(1) << '\n';
                    return 2;
                }
                continue;
            }
            const std::size_t equals = change.find('=');
            if (equals == std::string::npos) {
                std::cerr << "update_keyword: " << change << " is neither KEYWORD=value nor -KEYWORD\n";
                return 2;
            }
            // The text of the value is written as it is given: 'a string', 300.0 or T. The card takes the place and the
            // comment of the first card of its keyword, or comes after the last card.
            const std::string keyword = change.substr(0, equals);
            const std::string value_text = change.substr(equals + 1);
            openfits::card edited =
                openfits::card::with_value_text(keyword, value_text, cards.comment(keyword).value_or(""));
            if (const std::optional<std::size_t> position = cards.index_of(keyword)) {
                (void)cards.erase(keyword);
                cards.insert(*position, std::move(edited));
            } else {
                cards.append(std::move(edited));
            }
        }
        cards.add_history("Keywords changed by the sample update_keyword");
        // Structural keywords, such as NAXIS1, are refused here, before anything is written.
        file.commit();

        // The file as the commit left it.
        const openfits::hdu_info& unit = file.file().hdu(index);
        std::cout << "HDU " << index << ": " << unit.header().size() << " cards, ";
        if (unit.header_size() == header_size) {
            std::cout << "written in place over the " << header_size << " bytes of the header\n";
        } else {
            std::cout << "header grown from " << header_size << " to " << unit.header_size()
                      << " bytes: the file was written again, " << file_size << " bytes to " << file.file().size()
                      << '\n';
        }
        const openfits::checksum_report sums = file.file().verify(index);
        std::cout << "CHECKSUM " << state_name(sums.checksum) << ", DATASUM " << state_name(sums.datasum) << '\n';
        return 0;
    } catch (const std::exception& failure) {
        (void)std::fputs("update_keyword: ", stderr);
        (void)std::fputs(failure.what(), stderr);
        (void)std::fputs("\n", stderr);
        return 1;
    }
}
