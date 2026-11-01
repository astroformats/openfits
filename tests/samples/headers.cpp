// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The headers of the samples of other software: their units as the catalogue gives them, their cards compared one by
// one with what astropy reads of them (the .cards file next to each sample, which tests/data/scripts/dump_cards.py
// writes), and their records, which are the bytes of the file.

#include <openfits/error.h>
#include <openfits/reader.h>

#include "header/long_string.h"
#include "samples/sample_catalog.h"
#include "support/files.h"
#include "support/temp_directory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using openfits::card;
using openfits::diagnostic;
using openfits::hdu_info;
using openfits::reader;
using openfits::severity;
using openfits::value_kind;
using openfits::test::sample;

class samples_headers : public testing::TestWithParam<sample>
{};

// A card as astropy reads it.
struct astropy_card
{
    std::size_t hdu = 0;
    std::string keyword{};
    std::string type{};
    std::string value{};
    std::string comment{};
};

std::vector<astropy_card> astropy_cards(const sample& unit)
{
    std::string path = openfits::test::sample_path(unit);
    path.replace(path.rfind(".fits"), 5, ".cards");
    std::ifstream file(path, std::ios::binary);
    std::vector<astropy_card> cards;
    for (std::string line; std::getline(file, line);) {
        std::vector<std::string> fields;
        std::size_t start = 0;
        for (std::size_t tab = line.find('\t'); tab != std::string::npos; tab = line.find('\t', start)) {
            fields.push_back(line.substr(start, tab - start));
            start = tab + 1;
        }
        fields.push_back(line.substr(start));
        EXPECT_EQ(fields.size(), 5U) << line;
        if (fields.size() == 5) {
            const std::size_t hdu = std::stoul(fields[0]);
            cards.push_back(
                {.hdu = hdu, .keyword = fields[1], .type = fields[2], .value = fields[3], .comment = fields[4]});
        }
    }
    return cards;
}

// Text that astropy read: it makes every byte outside ASCII a question mark.
bool same_text(std::string_view ours, std::string_view theirs)
{
    return std::ranges::equal(ours, theirs,
                              [](char a, char b) { return a == b || (b == '?' && (a < ' ' || a > '~')); });
}

double real_of(std::string_view text)
{
    return std::strtod(std::string(text).c_str(), nullptr);
}

// The value of one of our cards as astropy writes it in a .cards file, or why it differs.
testing::AssertionResult same_value(const std::vector<card>& cards, std::size_t index, const astropy_card& theirs)
{
    const card& ours = cards[index];
    const openfits::card_value& value = ours.value();
    if (theirs.type == "commentary") {
        if (!ours.has_value_indicator() && same_text(ours.comment(), theirs.value)) {
            return testing::AssertionSuccess();
        }
    } else if (theirs.type == "undefined") {
        if (value.kind() == value_kind::undefined) {
            return testing::AssertionSuccess();
        }
    } else if (theirs.type == "bool") {
        if (value.kind() == value_kind::logical && (value.get<bool>() ? "T" : "F") == theirs.value) {
            return testing::AssertionSuccess();
        }
    } else if (theirs.type == "int") {
        if (const auto* integer = std::get_if<std::int64_t>(&value.data());
            integer != nullptr && std::to_string(*integer) == theirs.value) {
            return testing::AssertionSuccess();
        }
        if (const auto* integer = std::get_if<std::uint64_t>(&value.data());
            integer != nullptr && std::to_string(*integer) == theirs.value) {
            return testing::AssertionSuccess();
        }
    } else if (theirs.type == "float") {
        if (value.kind() == value_kind::real && value.get<double>() == real_of(theirs.value)) {
            return testing::AssertionSuccess();
        }
    } else if (theirs.type == "complex") {
        const std::size_t comma = theirs.value.find(',');
        const double real = real_of(theirs.value.substr(0, comma));
        const double imag = real_of(theirs.value.substr(comma + 1));
        if (const auto* number = std::get_if<openfits::complex_integer>(&value.data());
            number != nullptr && static_cast<double>(number->real) == real &&
            static_cast<double>(number->imag) == imag) {
            return testing::AssertionSuccess();
        }
        if (const auto* number = std::get_if<std::complex<double>>(&value.data());
            number != nullptr && number->real() == real && number->imag() == imag) {
            return testing::AssertionSuccess();
        }
    } else if (theirs.type == "str" && value.kind() == value_kind::string) {
        const std::string joined = openfits::detail::joined_string(cards, index);
        // astropy reads a string of spaces alone, the empty string, as the null string (a known difference).
        if (same_text(joined, theirs.value) ||
            (theirs.value.empty() && std::ranges::all_of(joined, [](char c) { return c == ' '; }))) {
            return testing::AssertionSuccess();
        }
    }
    return testing::AssertionFailure() << ours.keyword() << ": ours " << ours.text() << ", astropy's " << theirs.type
                                       << " " << theirs.value;
}

TEST_P(samples_headers, have_the_units_of_the_catalogue)
{
    const sample& unit = GetParam();
    const reader file(openfits::test::sample_path(unit));
    ASSERT_EQ(file.hdu_count(), unit.units.size());
    for (std::size_t i = 0; i < unit.units.size(); ++i) {
        const hdu_info& hdu = file.hdu(i);
        EXPECT_EQ(hdu.kind(), unit.units[i].kind) << i;
        EXPECT_EQ(hdu.name(), unit.units[i].name) << i;
        EXPECT_EQ(hdu.data_size(), unit.units[i].data_size) << i;
    }
    EXPECT_TRUE(file.diagnostics().empty());
}

TEST_P(samples_headers, depart_from_the_standard_as_the_catalogue_says)
{
    const sample& unit = GetParam();
    const reader file(openfits::test::sample_path(unit));
    std::vector<openfits::errc> found;
    for (const hdu_info& hdu : file.hdus()) {
        for (const diagnostic& entry : hdu.diagnostics()) {
            EXPECT_NE(entry.severity, severity::error) << entry.message;
            EXPECT_NE(std::ranges::find(unit.departures, entry.code), unit.departures.end()) << entry.message;
            found.push_back(entry.code);
        }
    }
    for (const openfits::errc code : unit.departures) {
        EXPECT_NE(std::ranges::find(found, code), found.end()) << static_cast<int>(code);
    }
}

TEST_P(samples_headers, read_their_cards_as_astropy_does)
{
    const sample& unit = GetParam();
    const reader file(openfits::test::sample_path(unit));
    const std::vector<astropy_card> theirs = astropy_cards(unit);
    ASSERT_FALSE(theirs.empty());
    // The n-th card of a keyword in astropy's list is the n-th one of the header; astropy joins CONTINUE records into
    // the card they continue, and leaves out the blank cards that end a header.
    std::map<std::pair<std::size_t, std::string>, std::size_t> seen;
    for (const astropy_card& expected : theirs) {
        const std::vector<card> cards(file.hdu(expected.hdu).header().begin(), file.hdu(expected.hdu).header().end());
        const std::size_t occurrence = seen[{expected.hdu, expected.keyword}]++;
        std::size_t index = cards.size();
        for (std::size_t i = 0, n = 0; i < cards.size(); ++i) {
            if (same_text(cards[i].keyword(), expected.keyword) && n++ == occurrence) {
                index = i;
                break;
            }
        }
        ASSERT_LT(index, cards.size()) << expected.keyword;
        EXPECT_TRUE(same_value(cards, index, expected));
        const std::string comment = openfits::detail::joined_comment(cards, index);
        EXPECT_TRUE(expected.type == "commentary" || same_text(comment, expected.comment))
            << expected.keyword << ": " << comment << " | " << expected.comment;
    }
}

TEST_P(samples_headers, keep_the_records_they_were_read_from)
{
    const sample& unit = GetParam();
    const std::string bytes = openfits::test::read_file(openfits::test::path_of(openfits::test::sample_path(unit)));
    const reader file(openfits::test::sample_path(unit));
    for (const hdu_info& hdu : file.hdus()) {
        std::string text;
        for (const card& value : hdu.header()) {
            text += value.text();
        }
        text += "END";
        text.resize(hdu.header_size(), ' ');
        EXPECT_EQ(text, bytes.substr(hdu.header_offset(), hdu.header_size())) << hdu.index();
    }
}

INSTANTIATE_TEST_SUITE_P(samples, samples_headers, testing::ValuesIn(openfits::test::all_samples()),
                         [](const testing::TestParamInfo<sample>& instance) { return std::string(instance.param.id); });

} // namespace
