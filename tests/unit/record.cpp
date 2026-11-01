// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "header/record.h"

#include <openfits/card.h>
#include <openfits/error.h>

#include "support/throws.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using openfits::card;
using openfits::card_value;
using openfits::diagnostic;
using openfits::errc;
using openfits::severity;
using openfits::validation_error;
using openfits::value_kind;
using openfits::detail::parse_record;
using openfits::detail::record_options;
using openfits::detail::write_commentary_record;
using openfits::detail::write_record;
using openfits::detail::write_value_record;
using openfits::test::throws;

struct reading
{
    card value;
    std::vector<diagnostic> departures;
};

// The record of text padded to 80 characters, read.
reading read(std::string_view text)
{
    std::string record(text);
    record.resize(80, ' ');
    reading result;
    result.value = parse_record(record, result.departures);
    return result;
}

std::string describe(const std::vector<diagnostic>& departures)
{
    std::string text;
    for (const diagnostic& departure : departures) {
        text += "  " + std::to_string(static_cast<int>(departure.code)) + " [" + departure.context.keyword +
                "]: " + departure.message + '\n';
    }
    return text;
}

testing::AssertionResult no_departures(const reading& result)
{
    if (result.departures.empty()) {
        return testing::AssertionSuccess();
    }
    return testing::AssertionFailure() << "unexpected departures:\n" << describe(result.departures);
}

// Success when the record had exactly the given departures, in order, each a warning about the keyword of the card.
testing::AssertionResult departures(const reading& result, std::vector<errc> codes)
{
    bool same = result.departures.size() == codes.size();
    for (std::size_t i = 0; same && i < codes.size(); ++i) {
        const diagnostic& departure = result.departures[i];
        same = departure.code == codes[i] && departure.severity == severity::warning &&
               departure.context.keyword == result.value.keyword();
    }
    if (same) {
        return testing::AssertionSuccess();
    }
    return testing::AssertionFailure() << "departures:\n" << describe(result.departures);
}

TEST(record, conforming_records_have_no_departures)
{
    for (const char* text :
         {"SIMPLE  =                    T / conforms to FITS", "OBJECT  = 'M 31    '           / the target",
          "NAXIS1  =                 6000", "EXPTIME =                300.0", "CVAL    = (1.5, -2)", "UNDEF   =",
          "COMMENT   any text = here", "HISTORY", "", "END", "CONTINUE  'more of the string&' / and a comment"}) {
        EXPECT_TRUE(no_departures(read(text))) << text;
    }
}

TEST(record, bytes_outside_printable_ascii_are_kept_with_a_warning)
{
    const reading result = read("OBJECT  = 'Nebulosa del \xC3\x91"
                                "and\xC3\xBA'");
    EXPECT_TRUE(departures(result, {errc::invalid_character}));
    EXPECT_EQ(result.value.value().get<std::string>(), "Nebulosa del \xC3\x91"
                                                       "and\xC3\xBA");

    EXPECT_TRUE(departures(read("COMMENT tab\there"), {errc::invalid_character}));
}

// The value indicator elsewhere than bytes 9 and 10, or without its space, is read as one when only spaces separate it
// from the name.
TEST(record, a_misplaced_value_indicator_is_read_with_a_warning)
{
    const reading no_space = read("KEYWORD='abc'");
    EXPECT_TRUE(departures(no_space, {errc::invalid_value_indicator}));
    EXPECT_EQ(no_space.value.keyword(), "KEYWORD");
    EXPECT_EQ(no_space.value.value().get<std::string>(), "abc");

    const reading early = read("KEY= 5");
    EXPECT_TRUE(departures(early, {errc::invalid_value_indicator}));
    EXPECT_EQ(early.value.keyword(), "KEY");
    EXPECT_EQ(early.value.value().get<std::int64_t>(), 5);

    const reading late = read("KEYWORD   = 5");
    EXPECT_TRUE(departures(late, {errc::invalid_value_indicator}));
    EXPECT_EQ(late.value.value().get<std::int64_t>(), 5);

    // An equals sign after other text is commentary.
    const reading text = read("NOTE     This = that");
    EXPECT_TRUE(no_departures(text));
    EXPECT_FALSE(text.value.has_value_indicator());
    EXPECT_EQ(text.value.comment(), " This = that");
}

TEST(record, a_keyword_name_outside_the_standard_is_read_with_a_warning)
{
    const reading lowercase = read("exptime =                300.0");
    EXPECT_TRUE(departures(lowercase, {errc::invalid_keyword}));
    EXPECT_EQ(lowercase.value.keyword(), "EXPTIME");

    const reading spaces = read("MY KEY  =                    5");
    EXPECT_TRUE(departures(spaces, {errc::invalid_keyword}));
    EXPECT_EQ(spaces.value.keyword(), "MY KEY");

    const reading indented = read(" KEY    =                    5");
    EXPECT_TRUE(departures(indented, {errc::invalid_keyword}));
    EXPECT_EQ(indented.value.keyword(), "KEY");

    const reading other = read("DATE.OBS= '2026-10-26'");
    EXPECT_TRUE(departures(other, {errc::invalid_keyword}));
    EXPECT_EQ(other.value.keyword(), "DATE.OBS");

    // The name runs to the equals sign, beyond byte 8.
    const reading longer = read("LONGKEYWORD= 5");
    EXPECT_TRUE(departures(longer, {errc::invalid_keyword, errc::invalid_value_indicator}));
    EXPECT_EQ(longer.value.keyword(), "LONGKEYWORD");
    EXPECT_EQ(longer.value.value().get<std::int64_t>(), 5);

    // A commentary keyword in lowercase is still commentary.
    const reading comment = read("comment some text");
    EXPECT_TRUE(departures(comment, {errc::invalid_keyword}));
    EXPECT_EQ(comment.value.keyword(), "COMMENT");
    EXPECT_FALSE(comment.value.has_value_indicator());
}

TEST(record, a_string_without_its_closing_quote_runs_to_the_end_of_the_record)
{
    const reading result = read("OBJECT  = 'M 31 / not a comment");
    EXPECT_TRUE(departures(result, {errc::invalid_string}));
    EXPECT_EQ(result.value.value().get<std::string>(), "M 31 / not a comment");
    EXPECT_EQ(result.value.comment(), "");
}

TEST(record, lowercase_logicals_and_exponent_letters_are_read_with_a_warning)
{
    const reading logical = read("SIMPLE  =                    t");
    EXPECT_TRUE(departures(logical, {errc::invalid_logical}));
    EXPECT_TRUE(logical.value.value().get<bool>());
    EXPECT_FALSE(read("FLAG    =                    f").value.value().get<bool>());

    const reading real = read("EXPTIME =                3.0e2");
    EXPECT_TRUE(departures(real, {errc::invalid_real}));
    EXPECT_EQ(real.value.value().get<double>(), 300.0);
    EXPECT_EQ(read("EXPTIME =                  3d2").value.value().get<double>(), 300.0);
}

// A value that does not parse keeps its text, and has no value.
TEST(record, a_value_of_no_form_of_the_standard_keeps_its_text)
{
    const reading comma = read("EXPTIME =                300,5 / with a comma");
    EXPECT_TRUE(departures(comma, {errc::invalid_value}));
    EXPECT_TRUE(comma.value.has_value_indicator());
    EXPECT_EQ(comma.value.value().kind(), value_kind::none);
    EXPECT_EQ(comma.value.value_text(), "300,5");
    EXPECT_EQ(comma.value.comment(), "with a comma");

    for (const char* text : {"KEY     = TRUE", "KEY     = 1.2.3", "KEY     = E5", "KEY     = .", "KEY     = +",
                             "KEY     = 1E", "KEY     = 0x1F", "KEY     = inf", "KEY     = NaN"}) {
        const reading result = read(text);
        EXPECT_TRUE(departures(result, {errc::invalid_value})) << text;
        EXPECT_EQ(result.value.value().kind(), value_kind::none) << text;
    }
}

// A value of no form runs to the slash of its comment: a string written without its quotes keeps its spaces.
TEST(record, a_value_of_no_form_runs_to_the_slash_of_its_comment)
{
    const reading unquoted = read("INSTRUME=     Invented Cam 2600 /");
    EXPECT_TRUE(departures(unquoted, {errc::invalid_value}));
    EXPECT_EQ(unquoted.value.value().kind(), value_kind::none);
    EXPECT_EQ(unquoted.value.value_text(), "Invented Cam 2600");
    EXPECT_EQ(unquoted.value.comment(), "");

    const reading commented = read("SWCREATE= Invented Imaging 3.1 (x64) / the software");
    EXPECT_TRUE(departures(commented, {errc::invalid_value}));
    EXPECT_EQ(commented.value.value_text(), "Invented Imaging 3.1 (x64)");
    EXPECT_EQ(commented.value.comment(), "the software");

    const reading alone = read("FILTER  = Ha alpha");
    EXPECT_TRUE(departures(alone, {errc::invalid_value}));
    EXPECT_EQ(alone.value.value_text(), "Ha alpha");

    // A value of a form of the standard ends at its first space; the text after it is the comment.
    const reading logical = read("FLAG    =                    T and more");
    EXPECT_TRUE(departures(logical, {errc::invalid_comment}));
    EXPECT_TRUE(logical.value.value().get<bool>());
    EXPECT_EQ(logical.value.comment(), "and more");
}

TEST(record, numbers_beyond_their_range_keep_their_text)
{
    const reading integer = read("BIG     =    18446744073709551616");
    EXPECT_TRUE(departures(integer, {errc::value_out_of_range}));
    EXPECT_EQ(integer.value.value().kind(), value_kind::none);
    EXPECT_EQ(integer.value.value_text(), "18446744073709551616");

    const reading real = read("BIG     =               1.0E400");
    EXPECT_TRUE(departures(real, {errc::value_out_of_range}));
    EXPECT_EQ(real.value.value().kind(), value_kind::none);

    const reading part = read("BIG     = (9223372036854775808, 1)");
    EXPECT_TRUE(departures(part, {errc::value_out_of_range}));
    EXPECT_EQ(part.value.value().kind(), value_kind::none);
}

TEST(record, text_after_the_value_without_a_slash_is_the_comment)
{
    const reading result = read("EXPTIME =                300.0 seconds");
    EXPECT_TRUE(departures(result, {errc::invalid_comment}));
    EXPECT_EQ(result.value.value().get<double>(), 300.0);
    EXPECT_EQ(result.value.comment(), "seconds");

    const reading after_string = read("OBJECT  = 'M 31' and more");
    EXPECT_TRUE(departures(after_string, {errc::invalid_comment}));
    EXPECT_EQ(after_string.value.comment(), "and more");
}

TEST(record, a_malformed_complex_value_has_no_value)
{
    for (const char* text : {"CVAL    = (1, 2", "CVAL    = (1 2)", "CVAL    = (1, x)", "CVAL    = ()"}) {
        const reading result = read(text);
        EXPECT_TRUE(departures(result, {errc::invalid_complex})) << text;
        EXPECT_EQ(result.value.value().kind(), value_kind::none) << text;
    }
}

// A conforming CONTINUE record has spaces in bytes 9 and 10 and a string, which a header joins to the string before
// it; any other CONTINUE record is commentary.
TEST(record, a_continue_record_holds_a_string_without_a_value_indicator)
{
    const reading conforming = read("CONTINUE  'ollowed by cloudy skies overnight.&' / more");
    EXPECT_TRUE(no_departures(conforming));
    EXPECT_EQ(conforming.value.keyword(), "CONTINUE");
    EXPECT_FALSE(conforming.value.has_value_indicator());
    EXPECT_EQ(conforming.value.value().get<std::string>(), "ollowed by cloudy skies overnight.&");
    EXPECT_EQ(conforming.value.comment(), "more");

    for (const char* text : {"CONTINUE  12", "CONTINUE= 'abc'", "CONTINUE 'abc'", "CONTINUE"}) {
        const reading other = read(text);
        EXPECT_FALSE(other.value.has_value_indicator()) << text;
        EXPECT_EQ(other.value.value().kind(), value_kind::none) << text;
        EXPECT_EQ(other.value.comment(),
                  std::string_view(text).substr(std::min<std::size_t>(8, std::string_view(text).size())))
            << text;
    }
}

// The HIERARCH convention: the name runs from byte 10 to the equals sign.
TEST(record, a_hierarch_record_has_the_canonical_name)
{
    const reading eso = read("HIERARCH ESO DET DIT = 1.5 / [s] integration time");
    EXPECT_TRUE(no_departures(eso));
    EXPECT_EQ(eso.value.keyword(), "ESO DET DIT");
    EXPECT_EQ(eso.value.value().get<double>(), 1.5);
    EXPECT_EQ(eso.value.unit(), "s");
    EXPECT_EQ(eso.value.value_text(), "1.5");

    const reading spaced = read("HIERARCH  ESO   DET  DIT= 2");
    EXPECT_TRUE(no_departures(spaced));
    EXPECT_EQ(spaced.value.keyword(), "ESO DET DIT");

    const reading lowercase = read("HIERARCH eso det dit = 2");
    EXPECT_TRUE(departures(lowercase, {errc::invalid_keyword}));
    EXPECT_EQ(lowercase.value.keyword(), "ESO DET DIT");

    // The prefix of the record is not part of the name, even when the name starts with HIERARCH too.
    const reading twice = read("HIERARCH HIERARCH X = 5");
    EXPECT_EQ(twice.value.keyword(), "HIERARCH X");
    EXPECT_EQ(card("HIERARCH HIERARCH X", 5).keyword(), "HIERARCH X");
    EXPECT_EQ(card::parse(write_record(card("HIERARCH HIERARCH X", 5), {.hierarch = true})).keyword(), "HIERARCH X");

    // Without an equals sign, the record is commentary: bytes 9 to 80 are its text.
    const reading without_value = read("HIERARCH ESO DET DIT");
    EXPECT_EQ(without_value.value.keyword(), "HIERARCH");
    EXPECT_FALSE(without_value.value.has_value_indicator());
    EXPECT_EQ(without_value.value.comment(), " ESO DET DIT");
}

// ---------------------------------------------------------------------------------------------------------------------
// Writing

template <typename Function> testing::AssertionResult refused(errc code, Function&& function)
{
    return throws<validation_error>(code, std::forward<Function>(function));
}

TEST(record_writer, a_name_outside_the_standard_needs_the_hierarch_convention)
{
    EXPECT_TRUE(refused(errc::invalid_keyword, [] { (void)write_value_record("ESO DET DIT", 1.5, {}, {}); }));
    EXPECT_TRUE(refused(errc::invalid_keyword, [] { (void)write_value_record("LONGKEYWORD", 1, {}, {}); }));
    EXPECT_TRUE(refused(errc::invalid_keyword, [] { (void)write_value_record("DATE.OBS", "x", {}, {}); }));

    const std::string record = write_value_record("ESO DET DIT", 1.5, "[s] integration time", {.hierarch = true});
    EXPECT_EQ(record.substr(0, 49), "HIERARCH ESO DET DIT = 1.5 / [s] integration time");
    EXPECT_EQ(record.size(), 80U);

    // A name that the record cannot hold, even with the convention.
    EXPECT_TRUE(refused(errc::invalid_keyword, [] { (void)write_value_record("A=B", 1, {}, {.hierarch = true}); }));
}

// The convention saves the space before the equals sign when the value needs it.
TEST(record_writer, a_long_hierarch_record_drops_the_space_before_the_equals_sign)
{
    // HIERARCH, the name, "= " and the string take exactly 80 characters.
    const std::string name = "ESO " + std::string(55, 'X');
    const std::string record = write_value_record(name, "abcdefgh", {}, {.hierarch = true});
    EXPECT_EQ(record, "HIERARCH " + name + "= 'abcdefgh'");
    EXPECT_TRUE(refused(errc::value_too_long,
                        [&name] { (void)write_value_record(name, "abcdefghi", {}, {.hierarch = true}); }));
}

TEST(record_writer, commentary_continue_and_end_cannot_have_a_value)
{
    for (const char* keyword : {"COMMENT", "HISTORY", "", "CONTINUE", "END"}) {
        EXPECT_TRUE(refused(errc::invalid_keyword, [keyword] { (void)write_value_record(keyword, 1, {}, {}); }))
            << keyword;
    }
}

TEST(record_writer, a_comment_that_does_not_fit_is_refused_or_cut)
{
    const std::string comment(48, 'c');
    EXPECT_TRUE(refused(errc::comment_too_long, [&comment] { (void)write_value_record("NAXIS", 2, comment, {}); }));

    const std::string record = write_value_record("NAXIS", 2, comment, {.truncate_comment = true});
    EXPECT_EQ(record.substr(30), " / " + comment.substr(0, 47));
    EXPECT_EQ(write_value_record("NAXIS", 2, comment.substr(0, 47), {}), record);
}

// The comments of fixed-format values line up after byte 30, unless the comment needs the room.
TEST(record_writer, the_comment_of_a_short_string_moves_closer_when_it_needs_the_room)
{
    const std::string aligned = write_value_record("OBJECT", "M 31", "the target", {});
    EXPECT_EQ(aligned.substr(0, 43), "OBJECT  = 'M 31    '           / the target");

    const std::string comment(50, 'c');
    const std::string closer = write_value_record("OBJECT", "M 31", comment, {});
    EXPECT_EQ(closer, "OBJECT  = 'M 31    ' / " + comment + std::string(7, ' '));
}

TEST(record_writer, values_and_comments_outside_printable_ascii_are_refused)
{
    EXPECT_TRUE(refused(errc::invalid_character, [] {
        (void)write_value_record("OBJECT",
                                 "\xC3\x91"
                                 "and\xC3\xBA",
                                 {}, {});
    }));
    EXPECT_TRUE(refused(errc::invalid_character, [] { (void)write_value_record("OBJECT", "M\t31", {}, {}); }));
    EXPECT_TRUE(refused(errc::invalid_character, [] {
        (void)write_value_record("EXPTIME", 1.0,
                                 "100 \xC2\xB0"
                                 "C",
                                 {});
    }));
}

TEST(record_writer, values_that_no_keyword_can_hold_are_refused)
{
    EXPECT_TRUE(refused(errc::invalid_real,
                        [] { (void)write_value_record("X", std::numeric_limits<double>::quiet_NaN(), {}, {}); }));
    EXPECT_TRUE(refused(errc::invalid_real,
                        [] { (void)write_value_record("X", -std::numeric_limits<double>::infinity(), {}, {}); }));
    EXPECT_TRUE(refused(errc::invalid_real, [] {
        (void)write_value_record("X", std::complex<double>(1, std::numeric_limits<double>::infinity()), {}, {});
    }));
    EXPECT_TRUE(refused(errc::invalid_value, [] { (void)write_value_record("X", card_value(), {}, {}); }));
}

TEST(record_writer, a_refusal_names_the_keyword)
{
    try {
        (void)write_value_record("exptime", std::numeric_limits<double>::infinity(), {}, {});
        ADD_FAILURE() << "no exception";
    } catch (const validation_error& failure) {
        EXPECT_EQ(failure.context().keyword, "EXPTIME");
    }
}

TEST(record_writer, commentary_records_hold_up_to_72_characters_of_text)
{
    const std::string text(72, 't');
    EXPECT_EQ(write_commentary_record("HISTORY", text, {}), "HISTORY " + text);
    EXPECT_EQ(write_commentary_record("comment", "a note", {}), "COMMENT a note" + std::string(66, ' '));
    EXPECT_EQ(write_commentary_record("", "", {}), std::string(80, ' '));

    EXPECT_TRUE(refused(errc::comment_too_long, [&text] { (void)write_commentary_record("HISTORY", text + 't', {}); }));
    EXPECT_EQ(write_commentary_record("HISTORY", text + "more", {.truncate_comment = true}), "HISTORY " + text);
    EXPECT_TRUE(refused(errc::invalid_character, [] { (void)write_commentary_record("COMMENT", "\x7F", {}); }));
}

TEST(record_writer, commentary_records_of_other_keywords_cannot_read_as_values)
{
    EXPECT_EQ(write_commentary_record("TITLE", "a title", {}).substr(0, 15), "TITLE   a title");
    EXPECT_TRUE(refused(errc::invalid_value_indicator, [] { (void)write_commentary_record("TITLE", "= 5", {}); }));
    EXPECT_TRUE(refused(errc::invalid_value_indicator, [] { (void)write_commentary_record("TITLE", "  =5", {}); }));
    // COMMENT, HISTORY and the blank keyword have no value whatever their text.
    EXPECT_EQ(write_commentary_record("COMMENT", "= 5", {}).substr(0, 11), "COMMENT = 5");

    for (const char* keyword : {"CONTINUE", "END", "LONGKEYWORD", "ESO DET"}) {
        EXPECT_TRUE(refused(errc::invalid_keyword, [keyword] { (void)write_commentary_record(keyword, "x", {}); }))
            << keyword;
    }
}

// A text right after an eight-character name continues the name; after HIERARCH and a space, it can make a record of
// the convention.
TEST(record_writer, commentary_records_of_eight_character_names_cannot_continue_the_name)
{
    EXPECT_TRUE(refused(errc::invalid_value_indicator, [] { (void)write_commentary_record("ABCDEFGH", "IJ= 5", {}); }));
    EXPECT_TRUE(
        refused(errc::invalid_value_indicator, [] { (void)write_commentary_record("HIERARCH", " ESO X = 5", {}); }));
    EXPECT_EQ(write_commentary_record("ABCDEFGH", "IJ and more", {}).substr(0, 19), "ABCDEFGHIJ and more");
    EXPECT_EQ(card::parse(write_commentary_record("HIERARCH", " ESO X", {})).comment(), " ESO X");
    EXPECT_EQ(card::parse(write_commentary_record("HIERARCH", "ESO X = 5", {})).comment(), "ESO X = 5");
}

// A card read from a file is written back as it was read, departures included.
TEST(record_writer, a_card_read_from_a_record_is_written_as_that_record)
{
    for (const char* text : {"exptime = 300.0 seconds", "KEYWORD='abc'", "OBJECT  = 'M 31", "CONTINUE  12"}) {
        std::string record(text);
        record.resize(80, ' ');
        EXPECT_EQ(write_record(card::parse(text), {}), record) << text;
    }
    EXPECT_EQ(write_record(card("EXPTIME", 300.0, "[s]"), {}), write_value_record("EXPTIME", 300.0, "[s]", {}));
    EXPECT_EQ(write_record(card::commentary("HISTORY", "x"), {}), write_commentary_record("HISTORY", "x", {}));
}

} // namespace
