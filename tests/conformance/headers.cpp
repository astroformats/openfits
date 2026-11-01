// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Standard §4.1 to §4.4 read through headers of constructed files: commentary keywords (§4.4.2.4), long strings
// (§4.2.1.2), the fill space before END (§4.4.2.4), the reserved keywords of §4.4.2 and of world coordinates and time
// (§8, §9, Table 22), INHERIT (Appendix K), and the HIERARCH convention. The departures are the rows of tolerance.cpp.

#include <openfits/error.h>
#include <openfits/io.h>
#include <openfits/reader.h>

#include "support/fixture_builder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using openfits::diagnostic;
using openfits::errc;
using openfits::header;
using openfits::reader;
using openfits::severity;
using openfits::test::card_record;
using openfits::test::fixture;
using openfits::test::primary_records;
using openfits::test::record;
using openfits::test::with;

// The source owns a copy of the bytes, so that a temporary fixture can be opened.
reader open(const fixture& file)
{
    return reader(std::make_unique<openfits::memory_source>(std::vector<std::byte>(file.bytes())));
}

// The primary header of a file of the records given after the mandatory ones, which must read without diagnostics.
header clean(const std::vector<std::string>& records)
{
    const reader input = open(fixture().header(with(primary_records(), records)));
    for (const diagnostic& found : input.hdu(0).diagnostics()) {
        ADD_FAILURE() << found.message;
    }
    return input.hdu(0).header();
}

std::vector<diagnostic> diagnostics_of(const std::vector<std::string>& records)
{
    return open(fixture().header(with(primary_records(), records))).hdu(0).diagnostics();
}

bool has(const std::vector<diagnostic>& diagnostics, errc code, severity level, std::string_view keyword)
{
    return std::ranges::any_of(diagnostics, [&](const diagnostic& found) {
        return found.code == code && found.severity == level && found.context.keyword == keyword;
    });
}

// §4.4.2.4: COMMENT, HISTORY and the blank keyword repeat as often as they like, and have no value.
TEST(conformance_headers, commentary_keywords_repeat_without_a_value)
{
    const header cards =
        clean({record("COMMENT   first"), record("COMMENT = not a value"), record("HISTORY   1"), record("HISTORY   2"),
               record("          blank keyword text"), record("        = blank")});
    EXPECT_EQ(cards.count("COMMENT"), 2U);
    EXPECT_EQ(cards.count("HISTORY"), 2U);
    EXPECT_EQ(cards.count(""), 2U);
    EXPECT_FALSE(cards[4].has_value_indicator());
    EXPECT_EQ(cards[4].comment(), "= not a value");
    EXPECT_EQ(cards.comment(""), "  blank keyword text");
}

// §4.4.2.4: the blank records before END are fill space.
TEST(conformance_headers, blank_records_before_end_are_fill_space)
{
    std::vector<std::string> records{card_record("OBJECT", "'x'")};
    records.insert(records.end(), 5, record(""));
    const header cards = clean(records);
    EXPECT_EQ(cards.size(), 9U);
    EXPECT_EQ(cards.free_cards(), 5U);
    EXPECT_EQ(cards.to_string().size(), 5U * 80U);
}

// §4.2.1.2: a string continued over CONTINUE records, with and without comments.
TEST(conformance_headers, continue_records_continue_a_string)
{
    const header cards =
        clean({record("LONG    = 'This is a long string, &' / first part"), record("CONTINUE  'continued here, &'"),
               record("CONTINUE  'and ended.'  / last"), record("ALONE   = 'one record&'"), record("CONTINUE  ''"),
               record("EMPTY   = '&'"), record("CONTINUE  '&' / comment"), record("CONTINUE  ''")});
    EXPECT_EQ(cards.value<std::string>("LONG"), "This is a long string, continued here, and ended.");
    EXPECT_EQ(cards.comment("LONG"), "first part last");
    EXPECT_EQ(cards.value<std::string>("ALONE"), "one record");
    EXPECT_EQ(cards.value<std::string>("EMPTY"), "");
    EXPECT_EQ(cards.comment("EMPTY"), "comment");
    EXPECT_EQ(cards.count("CONTINUE"), 5U);
}

// §4.2.1.2: a long string of a keyword of the HIERARCH convention.
TEST(conformance_headers, a_hierarch_keyword_can_be_continued)
{
    const header cards = clean({record("HIERARCH ESO OBS PATH = 'a/b/&'"), record("CONTINUE  'c'")});
    EXPECT_EQ(cards.value<std::string>("ESO OBS PATH"), "a/b/c");
}

// The HIERARCH convention, in each of its shapes.
TEST(conformance_headers, hierarch_keywords_are_read_in_each_shape)
{
    const header cards = clean({record("HIERARCH ESO DET DIT = 1.5 / spaced"), record("HIERARCH ESO DET NDIT= 10"),
                                record("HIERARCH   ESO   INS   MODE = 'X'"), record("HIERARCH A.B-C_D = T"),
                                record("HIERARCH LONGER THAN EIGHT CHARACTERS = 2")});
    EXPECT_EQ(cards.value<double>("ESO DET DIT"), 1.5);
    EXPECT_EQ(cards.comment("HIERARCH ESO DET DIT"), "spaced");
    EXPECT_EQ(cards.value<int>("ESO DET NDIT"), 10);
    EXPECT_EQ(cards.value<std::string>("ESO INS MODE"), "X");
    EXPECT_TRUE(cards.value<bool>("A.B-C_D"));
    EXPECT_EQ(cards.value<int>("LONGER THAN EIGHT CHARACTERS"), 2);
    // A name in lower case is found in upper case, with a warning.
    const std::vector<diagnostic> lower = diagnostics_of({record("HIERARCH eso det dit = 1")});
    EXPECT_TRUE(has(lower, errc::invalid_keyword, severity::warning, "ESO DET DIT"));
}

// Appendix K: INHERIT is read as a keyword of the extension; the primary header is not merged into it.
TEST(conformance_headers, inherit_is_a_keyword)
{
    const reader input = open(fixture()
                                  .header(with(primary_records(), {card_record("TELESCOP", "'T1'")}))
                                  .image({1}, {card_record("INHERIT", "T")}));
    const header& extension = input.hdu(1).header();
    EXPECT_TRUE(extension.value<bool>("INHERIT"));
    EXPECT_FALSE(extension.contains("TELESCOP"));
    EXPECT_TRUE(input.hdu(1).diagnostics().empty());
}

// §4.4.2: the reserved keywords with values of their type read without diagnostics.
TEST(conformance_headers, reserved_keywords_of_their_type_read_without_diagnostics)
{
    (void)clean({card_record("EXTEND", "T"),
                 card_record("BSCALE", "1.5"),
                 card_record("BZERO", "32768"),
                 card_record("BUNIT", "'ADU'"),
                 card_record("BLANK", "-32768"),
                 card_record("DATAMIN", "0"),
                 card_record("DATAMAX", "6.5E4"),
                 card_record("EXTNAME", "'SCI'"),
                 card_record("EXTVER", "2"),
                 card_record("EXTLEVEL", "1"),
                 card_record("DATE", "'2026-10-31T12:00:00.5'"),
                 card_record("DATE-OBS", "'2026-10-30'"),
                 card_record("ORIGIN", "'here'"),
                 card_record("TELESCOP", "'T1'"),
                 card_record("INSTRUME", "'C'"),
                 card_record("OBSERVER", "'O'"),
                 card_record("OBJECT", "'M 31'"),
                 card_record("AUTHOR", "'A'"),
                 card_record("REFERENC", "'R'"),
                 card_record("EQUINOX", "2000.0"),
                 card_record("BLOCKED", "T"),
                 card_record("EPOCH", "2000")});
}

// §8, §9, Table 22: the keywords of world coordinates and time, in their three forms and with alternates.
TEST(conformance_headers, coordinate_and_time_keywords_of_their_type_read_without_diagnostics)
{
    (void)clean({card_record("WCSAXES", "2"),          card_record("CTYPE1", "'RA---TAN'"),
                 card_record("CTYPE2A", "'DEC--TAN'"), card_record("CUNIT1", "'deg'"),
                 card_record("CRVAL1", "10.68"),       card_record("CRPIX1", "512.5"),
                 card_record("CDELT1", "-2.8E-4"),     card_record("PC1_2", "0.0"),
                 card_record("CD2_2B", "1.0"),         card_record("PV2_1", "45"),
                 card_record("PS1_0", "'p'"),          card_record("WCSNAME", "'primary'"),
                 card_record("CNAME1", "'x'"),         card_record("CRDER1", "0.1"),
                 card_record("CSYER1", "0.2"),         card_record("LONPOLE", "180"),
                 card_record("LATPOLEA", "0"),         card_record("RADESYS", "'ICRS'"),
                 card_record("RESTFRQ", "1.4E9"),      card_record("SPECSYS", "'TOPOCENT'"),
                 card_record("VELOSYS", "0"),          card_record("OBSGEO-X", "1.0E6"),
                 card_record("MJD-OBS", "60000.5"),    card_record("TIMESYS", "'UTC'"),
                 card_record("MJDREF", "0"),           card_record("TSTART", "0"),
                 card_record("XPOSURE", "300"),        card_record("TIMEUNIT", "'s'"),
                 card_record("1CTYP2", "'RA---TAN'"),  card_record("2CRPX3", "1.0"),
                 card_record("TCTYP4", "'GLON-CAR'"),  card_record("TCRPX4", "10"),
                 card_record("12PC3A", "0.5"),         card_record("WCAX3", "2"),
                 card_record("DOBS5", "'2026-01-01'"), card_record("MJDOB5", "1.0")});
}

// §4.4.2, §8, §9: a reserved keyword of another type than the standard gives it is read with a warning.
TEST(conformance_headers, reserved_keywords_of_another_type_read_with_a_warning)
{
    const std::vector<std::pair<std::string, std::string>> cases{
        {"EXTEND", "1"},   {"BSCALE", "'1'"},   {"BLANK", "1.5"},  {"BUNIT", "5"},         {"EXTNAME", "T"},
        {"EXTVER", "1.5"}, {"DATAMIN", "'0'"},  {"OBJECT", "31"},  {"EQUINOX", "'J2000'"}, {"BLOCKED", "1"},
        {"CTYPE1", "5"},   {"CRPIX1", "'512'"}, {"CDELT2A", "T"},  {"WCSAXES", "2.0"},     {"MJD-OBS", "'60000'"},
        {"TIMESYS", "1"},  {"1CTYP2", "2"},     {"TCRPX4", "'x'"}, {"INHERIT", "'T'"}};
    for (const auto& [keyword, value] : cases) {
        const std::vector<diagnostic> found = diagnostics_of({card_record(keyword, value)});
        const bool flagged = has(found, errc::reserved_keyword, severity::warning, keyword) ||
                             has(found, errc::keyword_type, severity::warning, keyword);
        EXPECT_TRUE(flagged) << keyword << " = " << value;
    }
    EXPECT_TRUE(has(diagnostics_of({card_record("DATE-OBS", "'yesterday'")}), errc::invalid_date, severity::warning,
                    "DATE-OBS"));
    EXPECT_TRUE(
        has(diagnostics_of({card_record("DATE", "'2026-02-30'")}), errc::invalid_date, severity::warning, "DATE"));
}

// §4.4.2: the keywords that only arrays have are not those of tables.
TEST(conformance_headers, the_keywords_of_arrays_are_not_checked_in_tables)
{
    const reader input = open(fixture()
                                  .primary()
                                  .header(with(openfits::test::extension_records("BINTABLE", 8, {4, 1}, 0, 1),
                                               {card_record("BSCALE", "0"), card_record("BLANK", "'x'")}))
                                  .data(4));
    EXPECT_TRUE(input.hdu(1).diagnostics().empty());
}

// The diagnostics of a header give the unit, the keyword and the offset of the record.
TEST(conformance_headers, diagnostics_give_the_place_of_their_record)
{
    const reader input = open(fixture().primary().image({1}, {record("lower   =                    1")}));
    const std::vector<diagnostic>& found = input.hdu(1).diagnostics();
    ASSERT_EQ(found.size(), 1U);
    EXPECT_EQ(found[0].context.hdu, 1U);
    EXPECT_EQ(found[0].context.keyword, "LOWER");
    EXPECT_EQ(found[0].context.offset, 2880U + (6U * 80U));
}

} // namespace
