// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "core/keyword_name.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using openfits::detail::canonical_keyword;
using openfits::detail::has_wildcards;
using openfits::detail::is_commentary_keyword;
using openfits::detail::is_standard_keyword;
using openfits::detail::match_keyword;

// Standard §4.1.2.1: one to eight characters of A to Z, 0 to 9, hyphen and underscore; names made of hyphens,
// underscores and digits alone are legal.
TEST(keyword_name, standard_names_are_one_to_eight_permitted_characters)
{
    for (const char* name : {"SIMPLE", "NAXIS1", "DATE-OBS", "TTYPE999", "A", "_", "-", "0", "12345678", "-_09AZ"}) {
        EXPECT_TRUE(is_standard_keyword(name)) << name;
    }
    for (const char* name : {"", "NAXIS123X", "naxis", "DATE OBS", "ESO.DET", "A=B", "\xC3\x91"}) {
        EXPECT_FALSE(is_standard_keyword(name)) << name;
    }
}

TEST(keyword_name, the_canonical_name_is_in_upper_case_without_surrounding_spaces)
{
    EXPECT_EQ(canonical_keyword("exptime"), "EXPTIME");
    EXPECT_EQ(canonical_keyword("  Date-Obs  "), "DATE-OBS");
    EXPECT_EQ(canonical_keyword(""), "");
    EXPECT_EQ(canonical_keyword("        "), "");
}

// The canonical name of a HIERARCH keyword is the text between HIERARCH and the equals sign with its spaces made one,
// and the prefix itself is not part of it.
TEST(keyword_name, the_canonical_name_of_the_hierarch_convention_has_single_spaces_and_no_prefix)
{
    EXPECT_EQ(canonical_keyword("HIERARCH ESO DET DIT"), "ESO DET DIT");
    EXPECT_EQ(canonical_keyword("HIERARCH   ESO   DET  DIT "), "ESO DET DIT");
    EXPECT_EQ(canonical_keyword("hierarch eso det dit"), "ESO DET DIT");
    EXPECT_EQ(canonical_keyword("ESO  DET   DIT"), "ESO DET DIT");
    // The name HIERARCH alone, and names that only start with it, stay.
    EXPECT_EQ(canonical_keyword("HIERARCH"), "HIERARCH");
    EXPECT_EQ(canonical_keyword("HIERARCHY"), "HIERARCHY");
}

// A canonical name is its own canonical name, so that a card is found by its keyword: a hierarchical name that starts
// with HIERARCH again, as the record "HIERARCH HIERARCH ESO DET NDIT= 11" of a fuzzed file holds, loses every prefix.
TEST(keyword_name, the_canonical_name_is_its_own_canonical_name)
{
    using openfits::detail::canonical_hierarch_name;
    for (const std::string_view name :
         {"HIERARCH HIERARCH ESO DET NDIT", "hierarch  Hierarch   eso", "HIERARCH HIERARCH", "HIERARCH ESO DET DIT",
          "ESO DET DIT", "EXPTIME", "HIERARCH"}) {
        const std::string canonical = canonical_keyword(name);
        EXPECT_EQ(canonical_keyword(canonical), canonical) << name;
        EXPECT_EQ(canonical_hierarch_name(canonical), canonical) << name;
    }
    EXPECT_EQ(canonical_keyword("HIERARCH HIERARCH ESO DET NDIT"), "ESO DET NDIT");
    EXPECT_EQ(canonical_hierarch_name("HIERARCH ESO DET NDIT"), "ESO DET NDIT");
    EXPECT_EQ(canonical_keyword("HIERARCH HIERARCH"), "HIERARCH");
}

TEST(keyword_name, commentary_keywords_are_comment_history_and_the_blank_keyword)
{
    EXPECT_TRUE(is_commentary_keyword("COMMENT"));
    EXPECT_TRUE(is_commentary_keyword("HISTORY"));
    EXPECT_TRUE(is_commentary_keyword(""));
    EXPECT_FALSE(is_commentary_keyword("CONTINUE"));
    EXPECT_FALSE(is_commentary_keyword("END"));
    EXPECT_FALSE(is_commentary_keyword("COMMENTS"));
}

TEST(keyword_pattern, wildcards_are_the_question_mark_the_asterisk_and_the_number_sign)
{
    EXPECT_TRUE(has_wildcards("NAXIS?"));
    EXPECT_TRUE(has_wildcards("*"));
    EXPECT_TRUE(has_wildcards("TTYPE#"));
    EXPECT_FALSE(has_wildcards("DATE-OBS"));
}

TEST(keyword_pattern, a_question_mark_matches_one_character)
{
    EXPECT_TRUE(match_keyword("NAXIS?", "NAXIS1"));
    EXPECT_TRUE(match_keyword("?AXIS1", "NAXIS1"));
    EXPECT_FALSE(match_keyword("NAXIS?", "NAXIS"));
    EXPECT_FALSE(match_keyword("NAXIS?", "NAXIS12"));
}

TEST(keyword_pattern, an_asterisk_matches_any_run_of_characters)
{
    EXPECT_TRUE(match_keyword("*", "EXPTIME"));
    EXPECT_TRUE(match_keyword("*", ""));
    EXPECT_TRUE(match_keyword("DATE*", "DATE"));
    EXPECT_TRUE(match_keyword("DATE*", "DATE-OBS"));
    EXPECT_TRUE(match_keyword("*OBS", "DATE-OBS"));
    EXPECT_TRUE(match_keyword("C*1_*", "CD1_2"));
    EXPECT_TRUE(match_keyword("*A*A*", "BANANA"));
    EXPECT_FALSE(match_keyword("*OBS", "OBSERVER"));
    EXPECT_FALSE(match_keyword("D*X", "DATE"));
}

// A number sign matches a run of digits, all of them, as a keyword search of indexed keywords wants.
TEST(keyword_pattern, a_number_sign_matches_all_the_digits_that_follow)
{
    EXPECT_TRUE(match_keyword("NAXIS#", "NAXIS1"));
    EXPECT_TRUE(match_keyword("NAXIS#", "NAXIS999"));
    EXPECT_TRUE(match_keyword("CD#_#", "CD1_2"));
    EXPECT_TRUE(match_keyword("TTYPE#*", "TTYPE12"));
    EXPECT_FALSE(match_keyword("NAXIS#", "NAXIS"));
    EXPECT_FALSE(match_keyword("NAXIS#", "NAXISA"));
    EXPECT_FALSE(match_keyword("A#1", "A21"));
}

TEST(keyword_pattern, letters_match_in_either_case)
{
    EXPECT_TRUE(match_keyword("naxis#", "NAXIS2"));
    EXPECT_TRUE(match_keyword("Date-*", "DATE-OBS"));
    EXPECT_TRUE(match_keyword("exptime", "EXPTIME"));
}

TEST(keyword_pattern, a_pattern_equal_to_the_name_matches_it_wildcards_included)
{
    EXPECT_TRUE(match_keyword("A#B", "A#B"));
    EXPECT_TRUE(match_keyword("ESO DET DIT", "ESO DET DIT"));
    EXPECT_FALSE(match_keyword("EXPTIME", "EXPTIM"));
    EXPECT_FALSE(match_keyword("", "A"));
}

// The work is bounded by the product of the lengths, also for patterns that fail late.
TEST(keyword_pattern, patterns_of_many_asterisks_fail_in_bounded_work)
{
    const std::string name(70, 'A');
    EXPECT_FALSE(match_keyword("*A*A*A*A*A*A*A*A*A*A*A*A*A*A*A*A*B", name));
    EXPECT_TRUE(match_keyword("*A*A*A*A*A*A*A*A*A*A*A*A*A*A*A*A*A", name));
}

} // namespace
