// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "header/keywords.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>

namespace {

using openfits::detail::is_reserved_keyword;
using openfits::detail::is_structural_keyword;
using openfits::detail::keyword_index;
using openfits::detail::keyword_prefix;
using openfits::detail::reserved_value_class;
using openfits::detail::value_class;

TEST(keywords, reads_the_index_of_an_indexed_keyword)
{
    EXPECT_EQ(keyword_index("NAXIS1", "NAXIS"), 1U);
    EXPECT_EQ(keyword_index("NAXIS999", "NAXIS"), 999U);
    EXPECT_EQ(keyword_index("TFORM42", "TFORM"), 42U);
    EXPECT_EQ(keyword_index("NAXIS", "NAXIS"), std::nullopt);
    EXPECT_EQ(keyword_index("NAXIS0", "NAXIS"), std::nullopt);
    EXPECT_EQ(keyword_index("NAXIS01", "NAXIS"), std::nullopt);
    EXPECT_EQ(keyword_index("NAXIS1000", "NAXIS"), std::nullopt);
    EXPECT_EQ(keyword_index("NAXIS1A", "NAXIS"), std::nullopt);
    EXPECT_EQ(keyword_index("TFORM1", "NAXIS"), std::nullopt);
}

TEST(keywords, names_the_structural_keywords)
{
    EXPECT_TRUE(is_structural_keyword("SIMPLE"));
    EXPECT_TRUE(is_structural_keyword("TNULL5"));
    EXPECT_TRUE(is_structural_keyword("ZVAL3"));
    EXPECT_TRUE(is_structural_keyword("CONTINUE"));
    // The writer of tables writes the names, units and display formats of columns from their models.
    EXPECT_TRUE(is_structural_keyword("TTYPE1"));
    EXPECT_TRUE(is_structural_keyword("TUNIT2"));
    EXPECT_TRUE(is_structural_keyword("TDISP999"));
    EXPECT_FALSE(is_structural_keyword("TDMIN1"));
    EXPECT_FALSE(is_structural_keyword("PTYPE1"));
    EXPECT_FALSE(is_structural_keyword("ZSOURCE"));
    EXPECT_FALSE(is_structural_keyword("EXTNAME"));
}

TEST(keywords, names_the_reserved_keywords)
{
    for (const char* name :
         {"SIMPLE",   "TFORM3",  "OBJECT",  "COMMENT", "",        "DATE",     "DATE-OBS", "DATEXXXX",
          "EXTNAME",  "INHERIT", "BLOCKED", "EPOCH",   "WCSAXES", "WCSAXESA", "EQUINOXZ", "CTYPE1",
          "CTYPE12B", "CRPIX3",  "PC1_2",   "PC1_2A",  "CD2_1",   "PV1_3",    "PS1_0",    "1CTYP7",
          "12CD3A",   "TCRPX3",  "TP3_4",   "2V3_X",   "WCSN5A",  "OBSGEO-X", "MJD-OBS",  "TSTART",
          "TIMESYS",  "PTYPE2",  "TTYPE9",  "TDISP2",  "FZALGOR", "FZALG3",   "ZQUANTIZ", "ZTILE2"}) {
        EXPECT_TRUE(is_reserved_keyword(name)) << name;
    }
    for (const char* name : {"EXPTIME", "GAIN", "CTYPE", "NAXIS0", "SWCREATE", "BAYERPAT", "XBAYROFF", "ROWORDER",
                             "ESO DET DIT", "CTYPE1AB", "WCSAXESAB"}) {
        EXPECT_FALSE(is_reserved_keyword(name)) << name;
    }
}

TEST(keywords, gives_the_type_of_the_values_of_reserved_keywords)
{
    EXPECT_EQ(reserved_value_class("BLOCKED"), value_class::logical);
    EXPECT_EQ(reserved_value_class("INHERIT"), value_class::logical);
    EXPECT_EQ(reserved_value_class("EXTVER"), value_class::integer);
    EXPECT_EQ(reserved_value_class("WCSAXESB"), value_class::integer);
    EXPECT_EQ(reserved_value_class("WCAX3"), value_class::integer);
    EXPECT_EQ(reserved_value_class("OBJECT"), value_class::string);
    EXPECT_EQ(reserved_value_class("CTYPE2"), value_class::string);
    EXPECT_EQ(reserved_value_class("CUNIT2A"), value_class::string);
    EXPECT_EQ(reserved_value_class("3CTYP4"), value_class::string);
    EXPECT_EQ(reserved_value_class("TCTYP4"), value_class::string);
    EXPECT_EQ(reserved_value_class("RADESYSA"), value_class::string);
    EXPECT_EQ(reserved_value_class("DATE"), value_class::date);
    EXPECT_EQ(reserved_value_class("DATE-OBS"), value_class::date);
    EXPECT_EQ(reserved_value_class("DOBS7"), value_class::date);
    EXPECT_EQ(reserved_value_class("CRPIX1"), value_class::number);
    EXPECT_EQ(reserved_value_class("CD1_2B"), value_class::number);
    EXPECT_EQ(reserved_value_class("12PC3"), value_class::number);
    EXPECT_EQ(reserved_value_class("EQUINOX"), value_class::number);
    EXPECT_EQ(reserved_value_class("MJD-OBS"), value_class::number);
    EXPECT_EQ(reserved_value_class("XPOSURE"), value_class::number);
    EXPECT_EQ(reserved_value_class("DATAMIN"), value_class::number);
    // Structural keywords, those of tables and compression, and the others have none.
    for (const char* name : {"BITPIX", "BSCALE", "EXTEND", "TTYPE1", "TUNIT1", "ZCMPTYPE", "EXPTIME", "DATE-MAP"}) {
        EXPECT_EQ(reserved_value_class(name), std::nullopt) << name;
    }
}

} // namespace

namespace {

// The prefix holds the first eight bytes of a keyword, completed with zeros: keywords that share them have the same
// prefix, as HIERARCH names do, and so have those that differ in zero bytes at their end, which their sizes tell apart.
TEST(keywords, gives_the_first_eight_bytes_of_a_keyword_as_its_prefix)
{
    EXPECT_EQ(keyword_prefix(""), 0U);
    EXPECT_EQ(keyword_prefix("A"), std::uint64_t{'A'} << 56U);
    EXPECT_EQ(keyword_prefix("ABCDEFGH"), keyword_prefix("ABCDEFGHIJ"));
    EXPECT_EQ(keyword_prefix("A"), keyword_prefix(std::string("A") + '\0'));
    EXPECT_NE(keyword_prefix("AB"), keyword_prefix("AC"));
    EXPECT_NE(keyword_prefix("ESO DET "), keyword_prefix("ESO DET"));
}

} // namespace
