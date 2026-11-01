// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Reading the structure of a file. This program opens a FITS file and lists its header and data units, the cards of
// each header and the problems that the reader found in them, without reading any data. The path is taken literally;
// with --spec it is parsed as an extended file name instead, such as "image.fits[SCI][1:100,1:100]", and the unit and
// the section it selects are shown.
//
// Usage: read_info [--spec] [--brief] <path>

#include <openfits/openfits.h>

#include "arguments.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string_view kind_name(openfits::hdu_kind kind)
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
    return "unknown extension";
}

std::string_view severity_name(openfits::severity level)
{
    switch (level) {
    case openfits::severity::info:
        return "info";
    case openfits::severity::warning:
        return "warning";
    case openfits::severity::error:
        break;
    }
    return "error";
}

void print_diagnostics(const std::vector<openfits::diagnostic>& diagnostics)
{
    for (const openfits::diagnostic& found : diagnostics) {
        std::cout << "  " << severity_name(found.severity) << ": " << found.message;
        if (!found.context.keyword.empty()) {
            std::cout << " [" << found.context.keyword << "]";
        }
        if (found.context.offset) {
            std::cout << " at byte " << *found.context.offset;
        }
        std::cout << '\n';
    }
}

void print_unit(const openfits::hdu_info& unit, bool brief)
{
    std::cout << "HDU " << unit.index() << ": " << kind_name(unit.kind());
    if (!unit.name().empty()) {
        std::cout << " '" << unit.name() << "' version " << unit.version();
    }
    std::cout << "\n  header: " << unit.header().size() << " cards, " << unit.header_size() << " bytes at "
              << unit.header_offset() << " (" << unit.header().free_cards() << " free)\n";
    if (const std::optional<std::uint64_t> size = unit.data_size()) {
        std::cout << "  data: " << *size << " bytes at " << unit.data_offset() << '\n';
    } else {
        std::cout << "  data: unknown size\n";
    }

    // Typed values: the axes, NAXIS1 to NAXISn, and keywords that a header may lack, as find() gives them. A value that
    // does not convert throws invalid_data_error.
    const openfits::header& cards = unit.header();
    try {
        const auto naxis = cards.find<std::uint64_t>("NAXIS");
        if (naxis && *naxis > 0 && *naxis <= 999) {
            std::cout << "  axes:";
            for (const std::uint64_t length : cards.values<std::uint64_t>("NAXIS", 1, *naxis)) {
                std::cout << ' ' << length;
            }
            std::cout << '\n';
        }
        if (const std::optional<std::string> object = cards.find<std::string>("OBJECT")) {
            std::cout << "  object: " << *object << '\n';
        }
        if (const std::optional<double> exposure = cards.find<double>("EXPTIME")) {
            std::cout << "  exposure: " << *exposure << " s\n";
        }
    } catch (const openfits::invalid_data_error& failure) {
        std::cout << "  " << failure.what() << '\n';
    }

    if (!brief) {
        // Each card as its record, without the spaces that end it.
        for (const openfits::card& value : unit.header()) {
            const std::string_view text = value.text();
            std::cout << "  | " << text.substr(0, text.find_last_not_of(' ') + 1) << '\n';
        }
    }
    print_diagnostics(unit.diagnostics());
}

} // namespace

int main(int argc, char** argv)
{
    try {
        bool specification = false;
        bool brief = false;
        std::string path;
        for (const std::string& argument : sample::arguments(argc, argv)) {
            if (argument == "--spec") {
                specification = true;
            } else if (argument == "--brief") {
                brief = true;
            } else {
                path = argument;
            }
        }
        if (path.empty()) {
            std::cerr << "usage: read_info [--spec] [--brief] <path>\n";
            return 2;
        }

        // A reader walks the units as they are asked for; hdus() walks them all.
        const openfits::reader file =
            specification ? openfits::reader(openfits::file_specification::parse(path)) : openfits::reader(path);
        std::cout << path << ": " << file.size() << " bytes, " << file.hdu_count() << " HDUs\n";
        if (specification) {
            std::cout << "selected HDU " << file.selected_hdu();
            if (const std::optional<openfits::region>& section = file.section()) {
                std::cout << ", section";
                for (std::size_t axis = 0; axis < section->start.size(); ++axis) {
                    std::cout << ' ' << section->size[axis] << " from " << section->start[axis] << " by "
                              << section->step[axis];
                }
            }
            std::cout << '\n';
        }
        for (const openfits::hdu_info& unit : file.hdus()) {
            print_unit(unit, brief);
        }
        const std::vector<openfits::diagnostic> walk = file.diagnostics();
        if (!walk.empty()) {
            std::cout << "file:\n";
            print_diagnostics(walk);
        }
        return 0;
    } catch (const std::exception& failure) {
        (void)std::fputs("read_info: ", stderr);
        (void)std::fputs(failure.what(), stderr);
        (void)std::fputs("\n", stderr);
        return 1;
    }
}
