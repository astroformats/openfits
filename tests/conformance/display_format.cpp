// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Standard §7.3.4, Tables 16 and 20: the display formats of TDISPn, parsed and rendered as the output formats of
// Fortran that they are.

#include <openfits/card.h>
#include <openfits/error.h>

#include "support/throws.h"

#include <gtest/gtest.h>

#include <complex>
#include <cstdint>
#include <limits>
#include <string>

namespace {

using openfits::display_code;
using openfits::display_format;
using openfits::errc;
using openfits::format_display;
using openfits::test::throws;

display_format format(const char* text)
{
    return display_format::parse(text);
}

// Every code of Table 20, with and without its optional parts, reads back from its text.
TEST(conformance_display_format, every_code_of_the_tables_is_parsed)
{
    EXPECT_EQ(format("A10"), (display_format{.code = display_code::character, .width = 10}));
    EXPECT_EQ(format("L1"), (display_format{.code = display_code::logical, .width = 1}));
    EXPECT_EQ(format("I10"), (display_format{.code = display_code::integer, .width = 10}));
    EXPECT_EQ(format("I10.5"), (display_format{.code = display_code::integer, .width = 10, .digits = 5}));
    EXPECT_EQ(format("B8.8"), (display_format{.code = display_code::binary, .width = 8, .digits = 8}));
    EXPECT_EQ(format("O6"), (display_format{.code = display_code::octal, .width = 6}));
    EXPECT_EQ(format("Z4.4"), (display_format{.code = display_code::hexadecimal, .width = 4, .digits = 4}));
    EXPECT_EQ(format("F10.3"), (display_format{.code = display_code::fixed, .width = 10, .digits = 3}));
    EXPECT_EQ(format("E15.7"), (display_format{.code = display_code::exponential, .width = 15, .digits = 7}));
    EXPECT_EQ(format("E15.7E3"),
              (display_format{.code = display_code::exponential, .width = 15, .digits = 7, .exponent_digits = 3}));
    EXPECT_EQ(format("EN12.3"), (display_format{.code = display_code::engineering, .width = 12, .digits = 3}));
    EXPECT_EQ(format("ES12.3"), (display_format{.code = display_code::scientific, .width = 12, .digits = 3}));
    EXPECT_EQ(format("G15.7"), (display_format{.code = display_code::general, .width = 15, .digits = 7}));
    EXPECT_EQ(format("G15.7E3"),
              (display_format{.code = display_code::general, .width = 15, .digits = 7, .exponent_digits = 3}));
    EXPECT_EQ(
        format("D25.17E3"),
        (display_format{.code = display_code::double_exponential, .width = 25, .digits = 17, .exponent_digits = 3}));
    // The trailing spaces of a string value are not significant.
    EXPECT_EQ(format(" F8.3  "), format("F8.3"));

    for (const char* text : {"A10", "L1", "I10", "I10.5", "B8.8", "O6", "Z4.4", "F10.3", "F8.0", "E15.7", "E15.7E3",
                             "EN12.3", "ES12.0", "G15.7", "G15.7E3", "D25.17", "D25.17E3", "A1048576"}) {
        EXPECT_EQ(format(text).to_string(), text);
    }
}

// The format codes are in upper case; Fortran's repetition and scale factors, and other edit descriptors, are not
// permitted; the digits a code requires are present, and fit in the width.
TEST(conformance_display_format, other_text_is_refused)
{
    for (const char* text :
         {"",        "f10.3",   "i5",    "X10",     "F10",  "E10",  "G10",    "EN12.3E2",       "ES12.3E2", "A10.2",
          "L1.1",    "I10E2",   "I0",    "A0",      "F0.0", "I5.6", "F5.5",   "E10.0",          "D10.0",    "G10.0",
          "E10.2E0", "F10.3 2", "3F8.3", "1PE15.7", "F10.", "I10.", "E10.2E", "F99999999999.1", "A1048577", "F8,3"}) {
        EXPECT_TRUE(throws<openfits::invalid_data_error>(errc::invalid_display_format, [text] {
            (void)display_format::parse(text);
        })) << text;
    }
}

// The exponent of Ew.dEe takes e + 2 characters of the field, which must hold them: a format that asked for more would
// make a field of e digits, billions of them in a TDISPn of a hostile file.
TEST(conformance_display_format, the_exponent_fits_in_the_width)
{
    EXPECT_EQ(format("E16.7E14").exponent_digits, 14U);
    for (const char* text : {"E15.7E14", "G10.2E9", "D8.1E7", "E15.7E3515474474"}) {
        EXPECT_TRUE(throws<openfits::invalid_data_error>(errc::invalid_display_format, [text] {
            (void)display_format::parse(text);
        })) << text;
    }
}

// A format built by the application is checked as one read from TDISPn is, before anything is rendered.
TEST(conformance_display_format, a_format_built_by_the_application_is_checked)
{
    const auto refused = [](const display_format& invalid) {
        return throws<openfits::invalid_data_error>(errc::invalid_display_format,
                                                    [&invalid] { (void)format_display(invalid, 1.5); });
    };
    EXPECT_TRUE(refused(display_format{
        .code = display_code::exponential, .width = 15, .digits = 7, .exponent_digits = 3'515'474'474U}));
    EXPECT_TRUE(refused(display_format{.code = display_code::fixed, .width = 0, .digits = 0}));
    EXPECT_TRUE(refused(display_format{.code = display_code::fixed, .width = 8}));
    EXPECT_TRUE(
        refused(display_format{.code = display_code::general, .width = display_format::max_width + 1, .digits = 1}));
    EXPECT_TRUE(throws<openfits::invalid_data_error>(errc::invalid_display_format, [] {
        (void)format_display(display_format{.code = display_code::character, .width = 5, .digits = 2}, "text");
    }));
}

// Aw: right-justified, or the first w characters of longer data; no quotes.
TEST(conformance_display_format, character_data_are_right_justified_or_cut)
{
    EXPECT_EQ(format_display(format("A10"), "M 31"), "      M 31");
    EXPECT_EQ(format_display(format("A3"), "M 31"), "M 3");
    EXPECT_EQ(format_display(format("A4"), "M 31"), "M 31");
    EXPECT_EQ(format_display(format("A2"), std::string("it's")), "it");
}

// Lw: T or F right-justified.
TEST(conformance_display_format, logical_data_are_t_or_f)
{
    EXPECT_EQ(format_display(format("L1"), true), "T");
    EXPECT_EQ(format_display(format("L3"), false), "  F");
}

// Iw.m: a sign for negative values, at least m digits, asterisks when they do not fit; Iw.0 shows a zero as blanks.
TEST(conformance_display_format, integers_have_a_sign_and_at_least_m_digits)
{
    EXPECT_EQ(format_display(format("I10"), -12), "       -12");
    EXPECT_EQ(format_display(format("I10.5"), 12), "     00012");
    EXPECT_EQ(format_display(format("I10.5"), -12), "    -00012");
    EXPECT_EQ(format_display(format("I3"), 0), "  0");
    EXPECT_EQ(format_display(format("I3.0"), 0), "   ");
    EXPECT_EQ(format_display(format("I3"), -12), "-12");
    EXPECT_EQ(format_display(format("I2"), -12), "**");
    EXPECT_EQ(format_display(format("I2"), 123), "**");
    EXPECT_EQ(format_display(format("I20"), std::numeric_limits<std::int64_t>::min()), "-9223372036854775808");
    EXPECT_EQ(format_display(format("I20"), std::numeric_limits<std::uint64_t>::max()), "18446744073709551615");
    EXPECT_EQ(format_display(format("I4"), std::uint8_t{255}), " 255");
}

// Bw.m, Ow.m and Zw.m: the magnitude in base 2, 8 or 16, hexadecimal in upper case, with no sign.
TEST(conformance_display_format, binary_octal_and_hexadecimal_give_the_magnitude)
{
    EXPECT_EQ(format_display(format("B8"), 5), "     101");
    EXPECT_EQ(format_display(format("B8.8"), 5), "00000101");
    EXPECT_EQ(format_display(format("O6.3"), 8), "   010");
    EXPECT_EQ(format_display(format("Z4"), 255), "  FF");
    EXPECT_EQ(format_display(format("Z4.4"), 255), "00FF");
    EXPECT_EQ(format_display(format("Z4"), -255), "  FF");
    EXPECT_EQ(format_display(format("Z16"), std::numeric_limits<std::int64_t>::min()), "8000000000000000");
    EXPECT_EQ(format_display(format("B2"), 4), "**");
}

// Fw.d: the integer part, the point and d digits, rounded half away from zero; the zero before the point only when
// the field has room for it.
TEST(conformance_display_format, fixed_notation_rounds_to_d_digits)
{
    EXPECT_EQ(format_display(format("F10.3"), 123.4567), "   123.457");
    EXPECT_EQ(format_display(format("F5.2"), 0.125), " 0.13");
    EXPECT_EQ(format_display(format("F5.2"), -0.125), "-0.13");
    EXPECT_EQ(format_display(format("F4.2"), 0.5), "0.50");
    EXPECT_EQ(format_display(format("F3.2"), 0.5), ".50");
    EXPECT_EQ(format_display(format("F6.2"), -0.001), " -0.00");
    EXPECT_EQ(format_display(format("F5.0"), 3.7), "   4.");
    EXPECT_EQ(format_display(format("F4.1"), 1234.5), "****");
    EXPECT_EQ(format_display(format("F10.2"), 5), "      5.00");
    EXPECT_EQ(format_display(format("F25.0"), 1e20), "   100000000000000000000.");
}

// Ew.dEe and Dw.dEe: 0.1 <= fraction < 1, the fraction in a field of w - e - 2 characters, then the letter, the sign
// and e digits; e + 1 digits without the letter; asterisks beyond.
TEST(conformance_display_format, exponential_notation_has_a_fraction_below_one)
{
    EXPECT_EQ(format_display(format("E12.4"), 1234.5), "  0.1235E+04");
    EXPECT_EQ(format_display(format("E12.4E3"), 1234.5), " 0.1235E+004");
    EXPECT_EQ(format_display(format("D12.4"), 1234.5), "  0.1235D+04");
    EXPECT_EQ(format_display(format("E12.4"), 0.0), "  0.0000E+00");
    EXPECT_EQ(format_display(format("E12.4"), -1.5e-5), " -0.1500E-04");
    EXPECT_EQ(format_display(format("E12.4"), 1e100), "  0.1000+101");
    EXPECT_EQ(format_display(format("E10.3E1"), 1e10), "  0.100+11");
    EXPECT_EQ(format_display(format("E10.3E1"), 1e100), "**********");
    EXPECT_EQ(format_display(format("E9.4"), 1234.5), ".1235E+04");
    EXPECT_EQ(format_display(format("E8.4"), 1234.5), "********");
    EXPECT_EQ(format_display(format("E12.4"), 0.99996), "  0.1000E+01");
}

// ESw.d: 1 <= fraction < 10.
TEST(conformance_display_format, scientific_notation_has_one_digit_before_the_point)
{
    EXPECT_EQ(format_display(format("ES12.4"), 1234.5), "  1.2345E+03");
    EXPECT_EQ(format_display(format("ES12.4"), 0.0), "  0.0000E+00");
    EXPECT_EQ(format_display(format("ES10.2"), 9.999), "  1.00E+01");
    EXPECT_EQ(format_display(format("ES10.2"), -0.015), " -1.50E-02");
}

// ENw.d: an exponent that is a multiple of three, and 1 <= fraction < 1000.
TEST(conformance_display_format, engineering_notation_has_an_exponent_multiple_of_three)
{
    EXPECT_EQ(format_display(format("EN12.3"), 12345.678), "  12.346E+03");
    EXPECT_EQ(format_display(format("EN12.3"), 0.00123), "   1.230E-03");
    EXPECT_EQ(format_display(format("EN12.3"), 123456.0), " 123.456E+03");
    EXPECT_EQ(format_display(format("EN10.3"), 999.9996), " 1.000E+03");
    EXPECT_EQ(format_display(format("EN12.3"), 0.0), "   0.000E+00");
}

// Gw.dEe: Fw'.d' and e + 2 spaces for 0.1 - 0.5 x 10^(-d-1) <= value < 10^d - 0.5, Ew.dEe otherwise; zero as
// F(w - e - 2).(d - 1), as Fortran writes it; Iw, Lw and Aw for other data.
TEST(conformance_display_format, general_notation_chooses_fixed_or_exponential)
{
    EXPECT_EQ(format_display(format("G12.4"), 1234.5), "   1235.    ");
    EXPECT_EQ(format_display(format("G12.4"), 99.5), "   99.50    ");
    EXPECT_EQ(format_display(format("G12.4"), 0.1), "  0.1000    ");
    EXPECT_EQ(format_display(format("G12.4"), 12345.0), "  0.1235E+05");
    EXPECT_EQ(format_display(format("G12.4"), 9999.6), "  0.1000E+05");
    EXPECT_EQ(format_display(format("G12.4"), 0.01), "  0.1000E-01");
    EXPECT_EQ(format_display(format("G12.4E3"), 1234.5), "  1235.     ");
    EXPECT_EQ(format_display(format("G12.4"), 0.0), "   0.000    ");
    EXPECT_EQ(format_display(format("G10.3"), 42), "        42");
    EXPECT_EQ(format_display(format("G5.1"), "abc"), "  abc");
    EXPECT_EQ(format_display(format("G3.1"), true), "  T");
}

// NaN and infinities as Fortran writes them.
TEST(conformance_display_format, nan_and_infinities_are_named)
{
    EXPECT_EQ(format_display(format("F8.2"), std::numeric_limits<double>::quiet_NaN()), "     NaN");
    EXPECT_EQ(format_display(format("F10.2"), -std::numeric_limits<double>::infinity()), " -Infinity");
    EXPECT_EQ(format_display(format("E12.4"), std::numeric_limits<double>::infinity()), "    Infinity");
    EXPECT_EQ(format_display(format("F5.1"), std::numeric_limits<double>::infinity()), "  Inf");
    EXPECT_EQ(format_display(format("F2.0"), std::numeric_limits<double>::infinity()), "**");
}

// Complex data: each part with a code of reals, recommended as (real,imaginary) in 2w + 3 characters.
TEST(conformance_display_format, complex_data_show_both_parts_in_parentheses)
{
    EXPECT_EQ(format_display(format("F6.2"), std::complex<double>(1.5, -2.25)), "(  1.50, -2.25)");
    EXPECT_EQ(format_display(format("E10.2"), std::complex<double>(0, 100)), "(  0.00E+00,  0.10E+03)");
}

// Each code displays the data the standard gives it.
TEST(conformance_display_format, a_code_that_cannot_display_the_data_is_refused)
{
    const auto refused = [](auto&& function) {
        return throws<openfits::invalid_data_error>(errc::invalid_display_format, function);
    };
    EXPECT_TRUE(refused([] { (void)format_display(format("A10"), 1.5); }));
    EXPECT_TRUE(refused([] { (void)format_display(format("A10"), 5); }));
    EXPECT_TRUE(refused([] { (void)format_display(format("I10"), 1.5); }));
    EXPECT_TRUE(refused([] { (void)format_display(format("L1"), 1); }));
    EXPECT_TRUE(refused([] { (void)format_display(format("F8.2"), "text"); }));
    EXPECT_TRUE(refused([] { (void)format_display(format("I8"), true); }));
    EXPECT_TRUE(refused([] { (void)format_display(format("Z8"), std::complex<double>(1, 2)); }));
}

} // namespace
