// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/openfits.h>

#include <array>
#include <cstddef>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Built against an installed package, or against the library as another project consumes it. It fails when the header,
// the library and the version that the package reports do not agree.
int main()
{
    try {
        const std::string_view linked = openfits::version();
        std::cout << "openfits " << linked << '\n';

        if (linked != OPENFITS_VERSION_STRING) {
            std::cerr << "the library reports " << linked << ", the header says " << OPENFITS_VERSION_STRING << '\n';
            return 1;
        }
        if (linked != EXPECTED_VERSION) {
            std::cerr << "the library reports " << linked << ", the package says " << EXPECTED_VERSION << '\n';
            return 1;
        }
        const openfits::semantic_version numbers = openfits::version_numbers();
        if (numbers.major != OPENFITS_VERSION_MAJOR || numbers.minor != OPENFITS_VERSION_MINOR ||
            numbers.patch != OPENFITS_VERSION_PATCH) {
            std::cerr << "the library reports other version numbers\n";
            return 1;
        }

        // The classes and functions of the API that a shared library exports.
        const openfits::card exposure = openfits::card::parse("EXPTIME =                300.0 / [s] exposure time");
        if (exposure.value().get<double>() != 300.0 || exposure.unit() != "s" ||
            openfits::format_display(openfits::display_format::parse("F6.1"), 300.0) != " 300.0") {
            std::cerr << "the library reads a keyword record differently\n";
            return 1;
        }

        // A source of the library, whose error is caught by its type, which is one type on both sides of a DLL.
        const std::array<std::byte, 4> content{std::byte{'F'}, std::byte{'I'}, std::byte{'T'}, std::byte{'S'}};
        const openfits::memory_source source{std::span<const std::byte>(content)};
        std::array<std::byte, 2> read{};
        source.read(2, read);
        bool past_the_end = false;
        try {
            source.read(3, read);
        } catch (const openfits::io_error& failure) {
            past_the_end = failure.code() == openfits::errc::end_of_data;
        }
        if (read[0] != std::byte{'T'} || read[1] != std::byte{'S'} || !past_the_end) {
            std::cerr << "the memory source reads differently\n";
            return 1;
        }

        // A file read by a reader, its header through the templates that the application instantiates.
        std::string records;
        for (const std::string_view record : {"SIMPLE  =                    T", "BITPIX  =                    8",
                                              "NAXIS   =                    0", "OBJECT  = 'M 31    '", "END"}) {
            records += record;
            records.resize(records.size() + 80 - record.size(), ' ');
        }
        records.resize(2880, ' ');
        std::vector<std::byte> file(records.size());
        for (std::size_t i = 0; i < records.size(); ++i) {
            file[i] = static_cast<std::byte>(records[i]);
        }
        const openfits::reader input(std::make_unique<openfits::memory_source>(std::move(file)));
        const openfits::header& cards = input.hdu(0).header();
        if (input.hdu_count() != 1 || cards.value<std::string>("OBJECT") != "M 31" || cards.value<int>("BITPIX") != 8 ||
            cards.find<double>("EXPTIME").has_value() ||
            openfits::file_specification::parse("a.fits[SCI,2]").hdu_version != 2) {
            std::cerr << "the reader reads a header differently\n";
            return 1;
        }
        return 0;
    } catch (...) {
        // A failing stream must not turn into a crash that hides which check was running.
        return 2;
    }
}
