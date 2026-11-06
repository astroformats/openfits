// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>
#include <openfits/header.h>

#include "support/throws.h"

#include <gtest/gtest.h>

#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

using openfits::card;
using openfits::card_value;
using openfits::errc;
using openfits::header;
using openfits::invalid_data_error;
using openfits::usage_error;
using openfits::test::throws;

// A header of cards parsed from records.
header parsed(std::initializer_list<std::string_view> records)
{
    std::vector<card> cards;
    for (const std::string_view text : records) {
        cards.push_back(card::parse(text));
    }
    return header(std::move(cards));
}

// The value of the only card of a header, converted to T.
template <typename T> T value_of(std::string_view record)
{
    return parsed({record}).value<T>("KEY");
}

template <typename T> testing::AssertionResult refused(errc code, std::string_view record)
{
    return throws<invalid_data_error>(code, [record] { (void)value_of<T>(record); });
}

std::string end_record()
{
    std::string text = "END";
    text.resize(80, ' ');
    return text;
}

TEST(header, keeps_its_cards_in_their_order)
{
    const header cards = parsed({"SIMPLE  =                    T", "BITPIX  =                    8", "COMMENT a"});
    ASSERT_EQ(cards.size(), 3U);
    EXPECT_FALSE(cards.empty());
    EXPECT_EQ(cards[1].keyword(), "BITPIX");
    EXPECT_EQ(cards.at(2).comment(), "a");
    EXPECT_EQ(cards.cards().size(), 3U);
    EXPECT_EQ(std::distance(cards.begin(), cards.end()), 3);
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { (void)cards.at(3); }));
    EXPECT_TRUE(header().empty());
}

TEST(header, finds_the_first_card_of_a_keyword_in_any_case_and_with_the_hierarch_prefix)
{
    const header cards =
        parsed({"EXPTIME =                  1.0", "HIERARCH ESO DET  DIT = 2", "EXPTIME =                  3.0"});
    EXPECT_EQ(cards.index_of("EXPTIME"), 0U);
    EXPECT_EQ(cards.index_of("exptime"), 0U);
    EXPECT_EQ(cards.index_of(" ExpTime "), 0U);
    EXPECT_EQ(cards.index_of("ESO DET DIT"), 1U);
    EXPECT_EQ(cards.index_of("hierarch eso  det dit"), 1U);
    EXPECT_EQ(cards.index_of("OBJECT"), std::nullopt);
    EXPECT_TRUE(cards.contains("exptime"));
    EXPECT_FALSE(cards.contains("OBJECT"));
    EXPECT_EQ(cards.count("EXPTIME"), 2U);
    EXPECT_EQ(cards.count("OBJECT"), 0U);
    EXPECT_EQ(cards.value<double>("EXPTIME"), 1.0);
}

// More cards than a lookup searches in order: the index sorts the keywords by their first eight bytes, and then whole
// for those that share them, as HIERARCH names do, and keeps the first card of each.
TEST(header, an_index_finds_the_first_card_of_keywords_that_share_their_first_eight_bytes)
{
    std::vector<std::string> records;
    records.reserve(43);
    for (int i = 0; i < 40; ++i) {
        records.push_back("HIERARCH ESO DET CHIP" + std::to_string(i % 20) + " GAIN = " + std::to_string(i));
    }
    records.emplace_back("SHORT   =                    1");
    records.emplace_back("SHORTER =                    2");
    records.emplace_back("SHORT   =                    3");
    std::vector<card> list;
    list.reserve(records.size());
    for (const std::string& record : records) {
        list.push_back(card::parse(record));
    }
    const header cards(std::move(list));
    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(cards.index_of("ESO DET CHIP" + std::to_string(i) + " GAIN"), static_cast<std::size_t>(i)) << i;
    }
    EXPECT_EQ(cards.index_of("SHORT"), 40U);
    EXPECT_EQ(cards.index_of("SHORTER"), 41U);
    EXPECT_EQ(cards.index_of("ESO DET CHIP"), std::nullopt);
    EXPECT_EQ(cards.index_of("ESO DET CHIP1 GAIN X"), std::nullopt);
    EXPECT_EQ(cards.value<std::int64_t>("ESO DET CHIP7 GAIN"), 7);
}

// The keywords that a file departing from the standard can hold, which differ only in the zero bytes at their end or
// begin with one, are found by an index as by a search: fuzz_header found the first card of eight bytes from 0 to 7
// missing from the index, which took it for the blank keyword.
TEST(header, an_index_finds_keywords_that_differ_in_the_zero_bytes_at_their_end)
{
    std::vector<std::string> keywords;
    keywords.reserve(38);
    for (int i = 0; i < 34; ++i) {
        keywords.push_back("KEY" + std::to_string(i));
    }
    keywords.emplace_back("A");
    keywords.push_back(std::string("A") + '\0');
    keywords.emplace_back(1, '\0');
    std::string bytes(8, '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<char>(i);
    }
    keywords.push_back(bytes);
    std::vector<card> list;
    list.reserve(keywords.size() + 1);
    list.push_back(card::commentary("", "a blank card"));
    for (std::size_t i = 0; i < keywords.size(); ++i) {
        const std::string number = std::to_string(i);
        std::string record = keywords[i];
        record.resize(8, ' ');
        record += "= " + std::string(20 - number.size(), ' ') + number;
        list.push_back(card::parse(record));
    }
    const header cards(std::move(list));
    for (std::size_t i = 0; i < cards.size(); ++i) {
        EXPECT_EQ(cards.index_of(cards[i].keyword()), i) << i;
    }
}

// The index made for a header of 33 cards grows as cards are appended, and keeps finding the first card of each
// keyword, short ones and HIERARCH names that share their first eight bytes alike.
TEST(header, an_index_keeps_finding_every_keyword_as_it_grows)
{
    header cards;
    std::vector<std::string> keywords;
    keywords.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        keywords.push_back(i % 2 == 0 ? "K" + std::to_string(i) : "ESO DET CHIP" + std::to_string(i) + " GAIN");
        cards.append(card(keywords.back(), i));
        if (i % 97 == 0 || i == 999) {
            for (std::size_t k = 0; k < keywords.size(); ++k) {
                ASSERT_EQ(cards.index_of(keywords[k]), k) << keywords[k] << " in " << cards.size() << " cards";
            }
        }
    }
    cards.append(card("K0", 1));
    EXPECT_EQ(cards.index_of("K0"), 0U);
    EXPECT_EQ(cards.index_of("ESO DET CHIP"), std::nullopt);
    EXPECT_EQ(cards.index_of("ESO DET CHIP1 GAIN X"), std::nullopt);
}

TEST(header, matches_keywords_with_patterns)
{
    const header cards = parsed({"NAXIS   =                    2", "NAXIS1  =                   10",
                                 "NAXIS2  =                   20", "CTYPE1  = 'RA---TAN'", "HISTORY x"});
    EXPECT_EQ(cards.matching("NAXIS#"), (std::vector<std::size_t>{1, 2}));
    EXPECT_EQ(cards.matching("naxis*"), (std::vector<std::size_t>{0, 1, 2}));
    EXPECT_EQ(cards.matching("C?YPE1"), (std::vector<std::size_t>{3}));
    EXPECT_EQ(cards.matching("NAXIS"), (std::vector<std::size_t>{0}));
    EXPECT_TRUE(cards.matching("DATE*").empty());
}

TEST(header, gives_the_comment_of_a_keyword)
{
    const header cards = parsed({"EXPTIME =                300.0 / [s] exposure", "HISTORY step one",
                                 "LONG    = 'abc&'", "CONTINUE  'def' / the end"});
    EXPECT_EQ(cards.comment("EXPTIME"), "[s] exposure");
    EXPECT_EQ(cards.comment("HISTORY"), "step one");
    EXPECT_EQ(cards.comment("LONG"), "the end");
    EXPECT_EQ(cards.comment("OBJECT"), std::nullopt);
}

// The table of header::value(): an integer card.
TEST(header, converts_an_integer)
{
    EXPECT_EQ(value_of<int>("KEY     =                   42"), 42);
    EXPECT_EQ(value_of<std::uint8_t>("KEY     =                  255"), 255);
    EXPECT_TRUE(refused<std::uint8_t>(errc::value_out_of_range, "KEY     =                  256"));
    EXPECT_TRUE(refused<std::int8_t>(errc::value_out_of_range, "KEY     =                 -129"));
    EXPECT_TRUE(refused<unsigned>(errc::value_out_of_range, "KEY     =                   -1"));
    EXPECT_EQ(value_of<std::uint64_t>("KEY     = 18446744073709551615"), std::numeric_limits<std::uint64_t>::max());
    EXPECT_TRUE(refused<std::int64_t>(errc::value_out_of_range, "KEY     = 9223372036854775808"));
    EXPECT_EQ(value_of<std::int64_t>("KEY     = -9223372036854775808"), std::numeric_limits<std::int64_t>::min());
    EXPECT_EQ(value_of<double>("KEY     =                   42"), 42.0);
    EXPECT_EQ(value_of<double>("KEY     =     9007199254740993"), 9007199254740992.0);
    EXPECT_EQ(value_of<float>("KEY     =                   -7"), -7.0F);
    EXPECT_TRUE(refused<bool>(errc::keyword_type, "KEY     =                    1"));
    EXPECT_EQ(value_of<std::string>("KEY     =                  +42"), "+42");
    EXPECT_EQ(value_of<std::complex<double>>("KEY     =                   42"), std::complex<double>(42.0, 0.0));
}

TEST(header, converts_a_real)
{
    EXPECT_EQ(value_of<int>("KEY     =                 42.0"), 42);
    EXPECT_EQ(value_of<std::uint64_t>("KEY     =                1E19"), 10'000'000'000'000'000'000U);
    EXPECT_TRUE(refused<int>(errc::keyword_type, "KEY     =                 42.5"));
    EXPECT_TRUE(refused<int>(errc::value_out_of_range, "KEY     =                 1E30"));
    EXPECT_TRUE(refused<unsigned>(errc::value_out_of_range, "KEY     =                 -1.0"));
    EXPECT_EQ(value_of<unsigned>("KEY     =                 -0.0"), 0U);
    EXPECT_EQ(value_of<double>("KEY     =               1.5D-3"), 1.5e-3);
    EXPECT_EQ(value_of<float>("KEY     =                  0.1"), 0.1F);
    EXPECT_TRUE(refused<float>(errc::value_out_of_range, "KEY     =               1.0E39"));
    EXPECT_TRUE(refused<bool>(errc::keyword_type, "KEY     =                  1.0"));
    EXPECT_EQ(value_of<std::string>("KEY     =               1.5E3"), "1.5E3");
    EXPECT_EQ(value_of<std::complex<double>>("KEY     =                  2.5"), std::complex<double>(2.5, 0.0));
}

TEST(header, converts_a_logical)
{
    EXPECT_TRUE(value_of<bool>("KEY     =                    T"));
    EXPECT_FALSE(value_of<bool>("KEY     =                    F"));
    EXPECT_TRUE(refused<int>(errc::keyword_type, "KEY     =                    T"));
    EXPECT_TRUE(refused<double>(errc::keyword_type, "KEY     =                    T"));
    EXPECT_EQ(value_of<std::string>("KEY     =                    T"), "T");
    EXPECT_EQ(value_of<std::string>("KEY     =                    f"), "F");
    EXPECT_TRUE(refused<std::complex<double>>(errc::keyword_type, "KEY     =                    T"));
}

TEST(header, converts_a_string)
{
    EXPECT_EQ(value_of<int>("KEY     = '42'"), 42);
    EXPECT_EQ(value_of<int>("KEY     = ' 42  '"), 42);
    EXPECT_EQ(value_of<int>("KEY     = '4.2E1'"), 42);
    EXPECT_TRUE(refused<int>(errc::keyword_type, "KEY     = '4.5'"));
    EXPECT_TRUE(refused<int>(errc::keyword_type, "KEY     = 'abc'"));
    EXPECT_TRUE(refused<int>(errc::value_out_of_range, "KEY     = '99999999999999999999'"));
    EXPECT_EQ(value_of<double>("KEY     = '300.0'"), 300.0);
    EXPECT_TRUE(refused<double>(errc::value_out_of_range, "KEY     = '1E999'"));
    EXPECT_TRUE(value_of<bool>("KEY     = 'T'"));
    EXPECT_FALSE(value_of<bool>("KEY     = 'F'"));
    EXPECT_TRUE(refused<bool>(errc::keyword_type, "KEY     = 't'"));
    EXPECT_TRUE(refused<bool>(errc::keyword_type, "KEY     = 'TRUE'"));
    EXPECT_EQ(value_of<std::string>("KEY     = 'O''HARA  '"), "O'HARA");
    EXPECT_TRUE(refused<std::complex<double>>(errc::keyword_type, "KEY     = '(1, 2)'"));
}

TEST(header, converts_a_complex_value)
{
    EXPECT_TRUE(refused<int>(errc::keyword_type, "KEY     =               (1, 2)"));
    EXPECT_TRUE(refused<double>(errc::keyword_type, "KEY     =           (1.5, 2.5)"));
    EXPECT_TRUE(refused<bool>(errc::keyword_type, "KEY     =               (1, 2)"));
    EXPECT_EQ(value_of<std::string>("KEY     =               (1, 2)"), "(1, 2)");
    EXPECT_EQ(value_of<std::complex<double>>("KEY     =               (1, 2)"), std::complex<double>(1.0, 2.0));
    EXPECT_EQ(value_of<std::complex<double>>("KEY     =        (1.5, -2.5E1)"), std::complex<double>(1.5, -25.0));
}

TEST(header, refuses_an_undefined_value_and_a_card_without_one)
{
    EXPECT_TRUE(refused<int>(errc::keyword_undefined, "KEY     ="));
    EXPECT_TRUE(refused<std::string>(errc::keyword_undefined, "KEY     =      / no value"));
    EXPECT_TRUE(refused<double>(errc::keyword_type, "KEY     = 1,5"));
    EXPECT_EQ(value_of<std::string>("KEY     = 1,5"), "1,5");
    EXPECT_TRUE(refused<std::string>(errc::keyword_type, "KEY       commentary text"));
    EXPECT_TRUE(refused<int>(errc::keyword_not_found, "OTHER   =                    1"));
}

TEST(header, gives_the_text_of_values_that_the_application_made)
{
    header cards;
    cards.set("REAL", 1500.0);
    cards.set("INTEGER", -3);
    cards.set("COMPLEX", std::complex<double>(1.0, -0.5));
    EXPECT_EQ(cards.value<std::string>("REAL"), "1500.");
    EXPECT_EQ(cards.value<std::string>("INTEGER"), "-3");
    EXPECT_EQ(cards.value<std::string>("COMPLEX"), "(1., -0.5)");
    cards.set("NAN", std::numeric_limits<double>::quiet_NaN());
    EXPECT_TRUE(throws<invalid_data_error>(errc::keyword_type, [&] { (void)cards.value<std::string>("NAN"); }));
}

TEST(header, finds_nothing_for_an_absent_keyword_or_an_undefined_value)
{
    const header cards = parsed({"KEY     =", "NUMBER  =                    7", "NAME    = 'x'"});
    EXPECT_EQ(cards.find<int>("KEY"), std::nullopt);
    EXPECT_EQ(cards.find<int>("OTHER"), std::nullopt);
    EXPECT_EQ(cards.find<int>("NUMBER"), 7);
    EXPECT_TRUE(throws<invalid_data_error>(errc::keyword_type, [&] { (void)cards.find<int>("NAME"); }));
    EXPECT_TRUE(throws<invalid_data_error>(errc::keyword_not_found, [&] { (void)cards.value<int>("OTHER"); }));
}

TEST(header, reads_indexed_keywords)
{
    const header cards =
        parsed({"NAXIS1  =                   10", "NAXIS2  =                   20", "NAXIS3  =                   30"});
    EXPECT_EQ(cards.values<int>("NAXIS", 1, 3), (std::vector<int>{10, 20, 30}));
    EXPECT_EQ(cards.values<int>("NAXIS", 2, 2), (std::vector<int>{20}));
    EXPECT_TRUE(cards.values<int>("NAXIS", 3, 2).empty());
    EXPECT_TRUE(throws<invalid_data_error>(errc::keyword_not_found, [&] { (void)cards.values<int>("NAXIS", 1, 4); }));
}

TEST(header, joins_a_long_string)
{
    const header cards = parsed({"LONG    = 'abc&'", "CONTINUE  'def&'", "CONTINUE  'ghi'", "AMP     = 'x&'"});
    EXPECT_EQ(cards.value<std::string>("LONG"), "abcdefghi");
    EXPECT_EQ(cards.value<std::string>("AMP"), "x&");
}

TEST(header, joins_the_long_string_of_the_card_at_an_index)
{
    // A keyword that a header holds twice: each card joined with its own CONTINUE records.
    const header cards = parsed({"NOTE    = 'first&' / one", "CONTINUE  'part' / more", "NOTE    = 'second&'",
                                 "CONTINUE  'a&'", "CONTINUE  'b'", "VALUE   = 3"});
    EXPECT_EQ(cards.string_at(0), "firstpart");
    EXPECT_EQ(cards.comment_at(0), "one more");
    EXPECT_EQ(cards.continuation_count(0), 1U);
    EXPECT_EQ(cards.string_at(2), "secondab");
    EXPECT_EQ(cards.continuation_count(2), 2U);
    EXPECT_EQ(cards.string_at(5), std::nullopt);
    EXPECT_EQ(cards.continuation_count(5), 0U);
    EXPECT_TRUE(throws<openfits::usage_error>(errc::invalid_argument, [&] { (void)cards.string_at(6); }));
    EXPECT_TRUE(throws<openfits::usage_error>(errc::invalid_argument, [&] { (void)cards.comment_at(6); }));
    EXPECT_TRUE(throws<openfits::usage_error>(errc::invalid_argument, [&] { (void)cards.continuation_count(6); }));
}

TEST(header, sets_a_new_keyword_after_the_last_card_that_is_not_fill)
{
    header cards = parsed({"SIMPLE  =                    T", "", ""});
    EXPECT_EQ(cards.free_cards(), 2U);
    cards.set("EXPTIME", 300.0, "[s]");
    ASSERT_EQ(cards.size(), 3U);
    EXPECT_EQ(cards[1].keyword(), "EXPTIME");
    EXPECT_EQ(cards.free_cards(), 1U);
    EXPECT_EQ(cards.index_of(""), 2U);
    cards.set("OBJECT", "M 31");
    EXPECT_EQ(cards.size(), 3U);
    EXPECT_EQ(cards.free_cards(), 0U);
    EXPECT_EQ(cards.index_of(""), std::nullopt);
    cards.set("OBSERVER", "me");
    EXPECT_EQ(cards.size(), 4U);
    EXPECT_EQ(cards[3].keyword(), "OBSERVER");
    EXPECT_EQ(cards.value<std::string>("OBJECT"), "M 31");
}

TEST(header, sets_an_existing_keyword_in_place_and_keeps_its_comment)
{
    header cards = parsed({"EXPTIME =                  1.0 / [s] exposure", "LONG    = 'abc&' / first",
                           "CONTINUE  'def' / second", "OBJECT  = 'x'"});
    cards.set("exptime", 2.0);
    EXPECT_EQ(cards.index_of("EXPTIME"), 0U);
    EXPECT_EQ(cards.value<double>("EXPTIME"), 2.0);
    EXPECT_EQ(cards.comment("EXPTIME"), "[s] exposure");
    EXPECT_TRUE(cards[0].text().empty());
    cards.set("LONG", "short");
    ASSERT_EQ(cards.size(), 3U);
    EXPECT_EQ(cards.value<std::string>("LONG"), "short");
    EXPECT_EQ(cards.comment("LONG"), "first second");
    EXPECT_EQ(cards.index_of("OBJECT"), 2U);
    cards.set("OBJECT", "y", "new comment");
    EXPECT_EQ(cards.comment("OBJECT"), "new comment");
}

TEST(header, refuses_to_set_a_keyword_that_cannot_have_a_value)
{
    header cards;
    for (const char* keyword : {"COMMENT", "HISTORY", "", "CONTINUE", "END"}) {
        EXPECT_TRUE(throws<usage_error>(errc::invalid_keyword, [&] { cards.set(keyword, 1); })) << keyword;
    }
    EXPECT_TRUE(cards.empty());
}

TEST(header, appends_blank_cards_as_fill)
{
    header cards = parsed({"SIMPLE  =                    T"});
    cards.append(card());
    cards.append(card());
    EXPECT_EQ(cards.free_cards(), 2U);
    cards.append(card::commentary("", "not fill"));
    EXPECT_EQ(cards[1].comment(), "not fill");
    EXPECT_EQ(cards.index_of(""), 1U);
    EXPECT_EQ(cards.free_cards(), 1U);
}

// A lookup gives the first card of a keyword whether the header is short and searched in order or long and indexed: at
// every size across the bound between the two, as cards are appended, blank ones taken as fill, and cards erased.
TEST(header, finds_the_first_card_of_a_keyword_at_every_size)
{
    header cards;
    const auto check = [&cards] {
        for (const std::string_view keyword : {"KEY0", "KEY3", "KEY7", "", "ABSENT"}) {
            std::optional<std::size_t> first;
            for (std::size_t i = 0; i < cards.size() && !first; ++i) {
                if (cards[i].keyword() == keyword) {
                    first = i;
                }
            }
            EXPECT_EQ(cards.index_of(keyword), first) << '"' << keyword << "\" in " << cards.size() << " cards";
        }
    };
    for (int i = 0; i < 60; ++i) {
        cards.append(card("KEY" + std::to_string(i % 8), i));
        if (i % 7 == 0) {
            cards.append(card());
            cards.append(card());
        }
        check();
    }
    ASSERT_GT(cards.size(), 40U);
    while (cards.erase("KEY3")) {
        check();
    }
    while (!cards.empty()) {
        cards.erase_at(cards.size() / 2);
        check();
    }
}

TEST(header, inserts_and_erases_cards)
{
    header cards = parsed(
        {"A       =                    1", "LONG    = 'abc&'", "CONTINUE  'def'", "B       =                    2"});
    cards.insert(0, card("FIRST", 0));
    EXPECT_EQ(cards.index_of("FIRST"), 0U);
    EXPECT_EQ(cards.index_of("B"), 4U);
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { cards.insert(6, card("X", 1)); }));
    cards.insert(5, card("LAST", 9));
    EXPECT_EQ(cards.index_of("LAST"), 5U);

    EXPECT_TRUE(cards.erase("long"));
    EXPECT_EQ(cards.size(), 4U);
    EXPECT_EQ(cards.index_of("CONTINUE"), std::nullopt);
    EXPECT_EQ(cards.index_of("B"), 2U);
    EXPECT_FALSE(cards.erase("LONG"));

    cards.erase_at(0);
    EXPECT_EQ(cards.index_of("A"), 0U);
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { cards.erase_at(3); }));
}

TEST(header, renames_a_card)
{
    header cards = parsed({"OLD     =                  1.5 / kept", "LONG    = 'abc&'", "CONTINUE  'def'",
                           "HIERARCH ESO DET DIT = 2 / dit"});
    EXPECT_TRUE(cards.rename("OLD", "NEW"));
    EXPECT_EQ(cards[0].text().substr(0, 10), "NEW     = ");
    EXPECT_EQ(cards[0].text().substr(10), card::parse("OLD     =                  1.5 / kept").text().substr(10));
    EXPECT_EQ(cards.value<double>("NEW"), 1.5);
    EXPECT_FALSE(cards.contains("OLD"));

    EXPECT_TRUE(cards.rename("LONG", "LONGER"));
    EXPECT_EQ(cards.value<std::string>("LONGER"), "abcdef");
    EXPECT_EQ(cards.size(), 4U);

    EXPECT_TRUE(cards.rename("ESO DET DIT", "DIT"));
    EXPECT_EQ(cards.value<int>("DIT"), 2);
    EXPECT_EQ(cards.comment("DIT"), "dit");
    EXPECT_TRUE(cards[3].text().empty());

    EXPECT_TRUE(cards.rename("LONGER", "HIERARCH A LONG NAME"));
    EXPECT_EQ(cards.value<std::string>("A LONG NAME"), "abcdef");
    EXPECT_EQ(cards.size(), 3U);

    EXPECT_FALSE(cards.rename("ABSENT", "X"));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_keyword, [&] { (void)cards.rename("NEW", "CONTINUE"); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_keyword, [&] { (void)cards.rename("NEW", "end"); }));
}

TEST(header, adds_commentary_in_cards_of_72_characters)
{
    header cards;
    cards.add_comment(std::string(72, 'a') + std::string(10, 'b'));
    cards.add_history({});
    ASSERT_EQ(cards.size(), 3U);
    EXPECT_EQ(cards[0].keyword(), "COMMENT");
    EXPECT_EQ(cards[0].comment(), std::string(72, 'a'));
    EXPECT_EQ(cards[1].comment(), std::string(10, 'b'));
    EXPECT_EQ(cards[2].keyword(), "HISTORY");
    EXPECT_EQ(cards[2].comment(), "");
    EXPECT_EQ(cards.count("COMMENT"), 2U);
}

TEST(header, writes_its_records_with_or_without_commentary)
{
    header cards = parsed({"SIMPLE  =                    T", "COMMENT a comment", "HISTORY a step", "        blank",
                           "LONG    = 'abc&'", "CONTINUE  'def'", "", ""});
    cards.set("NEW", 7, "added");
    const std::string all = cards.to_string();
    ASSERT_EQ(all.size() % 80, 0U);
    EXPECT_EQ(all.size(), std::size_t{8} * 80);
    EXPECT_EQ(all.substr(0, 80), cards[0].text());
    EXPECT_EQ(all.substr(std::size_t{5} * 80, 80), cards[5].text());
    EXPECT_EQ(all.substr(std::size_t{6} * 80, 80),
              std::string("NEW     =                    7 / added") + std::string(42, ' '));
    EXPECT_EQ(all.substr(std::size_t{7} * 80), end_record());

    const std::string values = cards.to_string(false);
    EXPECT_EQ(values.size(), 5U * 80U);
    EXPECT_EQ(values.substr(80, 80), cards[4].text());
    EXPECT_EQ(values.find("COMMENT"), std::string::npos);
    EXPECT_EQ(values.find("HISTORY"), std::string::npos);
}

TEST(header, writes_the_cards_that_a_change_made_as_the_writer_does)
{
    header cards;
    cards.set("HIERARCH ESO OBS NAME", "x");
    cards.set("LONG", std::string(100, 'z'), std::string(100, 'c'));
    const std::string text = cards.to_string();
    EXPECT_TRUE(text.starts_with("HIERARCH ESO OBS NAME = 'x       '"));
    EXPECT_EQ(text.substr(80, 11), "LONG    = '");
    EXPECT_EQ(text.substr(160, 11), "CONTINUE  '");
    const header reread = [&text] {
        std::vector<card> read;
        for (std::size_t i = 0; i + 80 < text.size(); i += 80) {
            read.push_back(card::parse(text.substr(i, 80)));
        }
        return header(std::move(read));
    }();
    EXPECT_EQ(reread.value<std::string>("LONG"), std::string(100, 'z'));
    EXPECT_EQ(reread.comment("LONG"), std::string(100, 'c'));
}

TEST(header, compares_its_cards)
{
    EXPECT_EQ(parsed({"A       =                    1"}), parsed({"A       =                    1"}));
    EXPECT_NE(parsed({"A       =                    1"}), parsed({"A       =                    2"}));
}

TEST(header, leaves_out_its_structural_cards)
{
    const header cards = parsed(
        {"SIMPLE  =                    T", "BITPIX  =                   16", "NAXIS   =                    1",
         "NAXIS1  =                   10", "BZERO   =                32768", "OBJECT  = 'M 31&'", "CONTINUE  ' galaxy'",
         "TFORM1  = 'longer than a card&'", "CONTINUE  'holds'", "CONTINUE  'orphan'", "HISTORY   step", ""});
    const header kept = openfits::without_structural(cards);
    ASSERT_EQ(kept.size(), 5U);
    EXPECT_EQ(kept.value<std::string>("OBJECT"), "M 31 galaxy");
    EXPECT_EQ(kept[2].text(), cards[9].text());
    EXPECT_EQ(kept[3].keyword(), "HISTORY");
    EXPECT_EQ(kept.free_cards(), 1U);
    EXPECT_EQ(kept[0].text(), cards[5].text());
    EXPECT_FALSE(kept.contains("BZERO"));
}

TEST(header, names_the_structural_keywords)
{
    for (const char* keyword :
         {"SIMPLE",   "bitpix",   "NAXIS",    "NAXIS3", "NAXIS999", "EXTEND",  "XTENSION", "PCOUNT",
          "GCOUNT",   "GROUPS",   "TFIELDS",  "TFORM1", "TBCOL12",  "THEAP",   "BSCALE",   "BZERO",
          "BLANK",    "TSCAL3",   "TZERO3",   "TNULL3", "TDIM3",    "ZIMAGE",  "ZCMPTYPE", "ZBITPIX",
          "ZNAXIS2",  "ZTILE1",   "ZNAME1",   "ZVAL1",  "ZSCALE",   "ZZERO",   "ZBLANK",   "ZQUANTIZ",
          "ZDITHER0", "ZHECKSUM", "ZDATASUM", "ZTABLE", "ZFORM2",   "ZCTYP2",  "ZTILELEN", "CHECKSUM",
          "DATASUM",  "CONTINUE", "END",      "TTYPE1", "TUNIT9",   "TDISP999"}) {
        EXPECT_TRUE(openfits::is_structural(keyword)) << keyword;
    }
    for (const char* keyword : {"OBJECT", "NAXIS0", "NAXIS01", "NAXIS1000", "TTYPE0", "TDMIN1", "EXTNAME", "DATE",
                                "COMMENT", "HISTORY", "", "BUNIT", "PTYPE1", "ESO DET DIT"}) {
        EXPECT_FALSE(openfits::is_structural(keyword)) << keyword;
    }
}

TEST(header, names_the_keywords_of_standard_form)
{
    // Standard §4.1.2.1: one to eight upper-case letters, digits, hyphens and underscores, as given.
    for (const char* keyword : {"NAXIS1", "DATE-OBS", "A_B", "X", "12345678"}) {
        EXPECT_TRUE(openfits::is_standard_keyword(keyword)) << keyword;
    }
    for (const char* keyword : {"naxis1", "ESO DET", "", "TOOLONGNAME", "A.B", "HIERARCH"}) {
        EXPECT_EQ(openfits::is_standard_keyword(keyword), std::string_view(keyword) == "HIERARCH") << keyword;
    }
}

} // namespace
