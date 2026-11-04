// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The conformance report of files. This program verifies each file given against the FITS standard and prints one line
// for each departure that it finds: its severity, where it is, and the section of the standard that it departs from,
// then a summary of the file. A file compressed with gzip is verified as the file it holds.
//
// Usage: verify <path>...
//
// The exit status is 0 when no file has a finding of severity error, and 1 otherwise or when a file cannot be read.

#include <openfits/openfits.h>

#include "arguments.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

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

// Where a finding is: its unit, its keyword or column, and its byte.
std::string place(const openfits::error_context& context)
{
    std::string where = context.hdu ? "HDU " + std::to_string(*context.hdu) : std::string("file");
    if (!context.keyword.empty()) {
        where += ", " + context.keyword;
    }
    if (context.column) {
        where += ", column " + std::to_string(*context.column);
    }
    if (context.offset) {
        where += ", byte " + std::to_string(*context.offset);
    }
    return where;
}

// Prints the report of the file at path, and returns the number of its findings of each severity.
std::array<std::size_t, 3> report(const std::string& path)
{
    const std::vector<openfits::diagnostic> findings = openfits::verify(path);
    std::array<std::size_t, 3> counts{};
    for (const openfits::diagnostic& finding : findings) {
        ++counts[static_cast<std::size_t>(finding.severity)];
        std::cout << path << ": " << severity_name(finding.severity) << ": " << place(finding.context) << ": "
                  << finding.message;
        if (!finding.section.empty()) {
            std::cout << " (standard " << finding.section << ')';
        }
        std::cout << '\n';
    }
    std::cout << path << ": " << counts[2] << " errors, " << counts[1] << " warnings, " << counts[0] << " notes"
              << (findings.empty() ? ": the file conforms to the standard" : "") << '\n';
    return counts;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const std::vector<std::string> paths = sample::arguments(argc, argv);
        if (paths.empty()) {
            std::cerr << "usage: verify <path>...\n";
            return 2;
        }
        int status = 0;
        for (const std::string& path : paths) {
            // A file that cannot be read, such as one that does not exist, fails alone, and the others are verified.
            try {
                if (report(path)[2] != 0) {
                    status = 1;
                }
            } catch (const openfits::error& failure) {
                std::cerr << "verify: " << path << ": " << failure.what() << '\n';
                status = 1;
            }
        }
        return status;
    } catch (const std::exception& failure) {
        (void)std::fputs("verify: ", stderr);
        (void)std::fputs(failure.what(), stderr);
        (void)std::fputs("\n", stderr);
        return 1;
    }
}
