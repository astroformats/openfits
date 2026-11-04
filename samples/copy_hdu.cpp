// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Appending the units of one file to another. This program writes a file of the units of a first file, as they are,
// then those of a second one, each with a HISTORY card and, when asked, a keyword set to the text of a value: a copy of
// the header and the data of a unit, which the writer reads from its file as it writes it, byte for byte when nothing
// changes. The primary HDU of the second file becomes an IMAGE extension, and is left out when it holds no data, only
// the keywords of its file. The output may be the first file, which is replaced only once the new one is complete.
//
// Usage: copy_hdu <output> <first> <second> [<KEYWORD>=<value text>]

#include <openfits/openfits.h>

#include "arguments.h"

#include <cstddef>
#include <cstdio>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

const char* kind_name(openfits::hdu_kind kind)
{
    switch (kind) {
    case openfits::hdu_kind::primary:
        return "primary";
    case openfits::hdu_kind::image:
        return "image";
    case openfits::hdu_kind::table:
        return "ASCII table";
    case openfits::hdu_kind::bintable:
        return "binary table";
    case openfits::hdu_kind::compressed_image:
        return "compressed image";
    case openfits::hdu_kind::compressed_table:
        return "compressed table";
    case openfits::hdu_kind::random_groups:
        return "random groups";
    case openfits::hdu_kind::unknown:
        break;
    }
    return "extension of unknown type";
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const std::vector<std::string> arguments = sample::arguments(argc, argv);
        if (arguments.size() < 3) {
            std::cerr << "usage: copy_hdu <output> <first> <second> [<KEYWORD>=<value text>]\n";
            return 2;
        }
        // The keyword to set, as KEYWORD='a string', KEYWORD=300.0 or KEYWORD=T: the text of its value is written as
        // it is.
        std::string keyword;
        std::string value_text;
        if (arguments.size() > 3) {
            const std::size_t equals = arguments[3].find('=');
            if (equals == std::string::npos) {
                std::cerr << "copy_hdu: " << arguments[3] << " is not KEYWORD=value\n";
                return 2;
            }
            keyword = arguments[3].substr(0, equals);
            value_text = arguments[3].substr(equals + 1);
        }

        // The readers stay open until the save, which reads the units from them.
        const openfits::reader first(arguments[1]);
        const openfits::reader second(arguments[2]);
        openfits::writer output;
        for (std::size_t i = 0; i < first.hdu_count(); ++i) {
            (void)output.add_hdu(first, i);
        }
        for (std::size_t i = 0; i < second.hdu_count(); ++i) {
            const openfits::hdu_info& unit = second.hdu(i);
            if (i == 0 && unit.data_size() == 0U) {
                continue;
            }
            // The header without the structural cards, which the copy takes from the unit, edited.
            openfits::header cards = openfits::without_structural(unit.header());
            cards.add_history("Copied from unit " + std::to_string(i) + " of " + arguments[2]);
            if (!keyword.empty()) {
                // The card takes the place of the first card of the keyword, or comes after the others.
                openfits::card edited = openfits::card::with_value_text(keyword, value_text);
                if (const std::optional<std::size_t> position = cards.index_of(keyword)) {
                    (void)cards.erase(keyword);
                    cards.insert(*position, std::move(edited));
                } else {
                    cards.append(std::move(edited));
                }
            }
            (void)output.add_hdu(second, i, {.header = cards});
        }
        // Checksums for every unit, which the save computes for each copy as it writes it.
        output.save(arguments[0], {.checksums = true});

        const openfits::reader check(arguments[0]);
        for (const openfits::hdu_info& unit : check.hdus()) {
            const openfits::checksum_report sums = check.verify(unit.index());
            std::cout << "HDU " << unit.index() << ": " << kind_name(unit.kind());
            if (!unit.name().empty()) {
                std::cout << " " << unit.name();
            }
            std::cout << ", " << unit.header().size() << " cards, " << unit.data_size().value_or(0)
                      << " bytes of data, CHECKSUM "
                      << (sums.checksum == openfits::checksum_state::valid ? "valid" : "absent or invalid") << '\n';
        }
        return 0;
    } catch (const std::exception& failure) {
        (void)std::fputs("copy_hdu: ", stderr);
        (void)std::fputs(failure.what(), stderr);
        (void)std::fputs("\n", stderr);
        return 1;
    }
}
