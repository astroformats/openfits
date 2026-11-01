// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Standard §4.2: the values of keyword records in their fixed and free formats, read and written, and Appendix A's
// productions of values. The fixed format is required for mandatory keywords and recommended for the others.

#include <openfits/card.h>
#include <openfits/error.h>

#include "header/record.h"
#include "support/sequence.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

using openfits::card;
using openfits::card_value;
using openfits::complex_integer;
using openfits::diagnostic;
using openfits::value_kind;
using openfits::detail::card_access;
using openfits::detail::parse_record;
using openfits::detail::write_value_record;

std::string record_of(std::string_view text)
{
    std::string record(text);
    record.resize(80, ' ');
    return record;
}

// The value of the record KEYWORD = value, the value written from byte 11.
card_value value_of(std::string_view value)
{
    return card::parse("KEYWORD = " + std::string(value)).value();
}

// The record that writes value, and the card it reads back as, which must have no departures.
card written(const card_value& value, std::string_view comment = {})
{
    const std::string record = write_value_record("KEYWORD", value, comment, {});
    std::vector<diagnostic> departures;
    const card read_back = parse_record(record, departures);
    EXPECT_TRUE(departures.empty()) << record;
    return read_back;
}

bool same_bits(double a, double b)
{
    return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

// -------------------------------------------------------------------------------------------------------------------
// Character strings (standard §4.2.1.1)

// The example of the standard: O'HARA = 'O''HARA'.
TEST(conformance_value, a_quote_in_a_string_is_two_quotes)
{
    EXPECT_EQ(value_of("'O''HARA'"), card_value("O'HARA"));
    // Seven characters in the record, padded to eight.
    EXPECT_EQ(written("O'HARA").value_text(), "'O''HARA '");
    EXPECT_EQ(value_of("''''"), card_value("'"));
    EXPECT_EQ(value_of("''''''"), card_value("''"));
}

TEST(conformance_value, leading_spaces_are_significant_and_trailing_spaces_are_not)
{
    EXPECT_EQ(value_of("'  M 31  '"), card_value("  M 31"));
    EXPECT_EQ(written("  M 31  ").value(), card_value("  M 31"));
}

// The example of the standard: a null string, an empty string (one significant space), and an undefined value.
TEST(conformance_value, the_null_string_the_empty_string_and_the_undefined_value_differ)
{
    const card keyword1 = card::parse("KEYWORD1= '' / null string keyword");
    const card keyword2 = card::parse("KEYWORD2= ' ' / empty string keyword");
    const card keyword3 = card::parse("KEYWORD3= / undefined keyword");
    EXPECT_EQ(keyword1.value(), card_value(""));
    EXPECT_EQ(keyword2.value(), card_value(" "));
    EXPECT_EQ(keyword3.value().kind(), value_kind::undefined);
    EXPECT_EQ(value_of("'        '"), card_value(" "));

    EXPECT_EQ(written("").value_text(), "''");
    EXPECT_EQ(written("").value(), card_value(""));
    EXPECT_EQ(written(" ").value(), card_value(" "));
    EXPECT_EQ(written("    ").value(), card_value(" "));
}

// A fixed-format string starts in byte 11; a free-format one later, after spaces only.
TEST(conformance_value, a_string_from_byte_11_is_in_fixed_format_and_a_later_one_in_free_format)
{
    const card fixed = card::parse("OBJECT  = 'M 31'");
    EXPECT_TRUE(card_access::fixed_format(fixed));
    const card free = card::parse("OBJECT  =         'M 31' / free format");
    EXPECT_FALSE(card_access::fixed_format(free));
    EXPECT_EQ(free.value(), fixed.value());
    EXPECT_EQ(free.comment(), "free format");
}

// The closing quote may come before byte 20, except for XTENSION; the writer still pads strings to eight characters,
// as readers that expect the fixed format of old want them, which changes nothing of the value.
TEST(conformance_value, short_strings_are_read_as_they_are_and_written_padded_to_eight_characters)
{
    EXPECT_EQ(value_of("'ab'"), card_value("ab"));
    EXPECT_EQ(write_value_record("OBJECT", "ab", {}, {}), record_of("OBJECT  = 'ab      '"));
    EXPECT_EQ(write_value_record("XTENSION", "IMAGE", {}, {}), record_of("XTENSION= 'IMAGE   '"));
    EXPECT_EQ(write_value_record("OBJECT", "abcdefghij", {}, {}), record_of("OBJECT  = 'abcdefghij'"));
}

// At most 68 characters fit in one record, with the quotes in bytes 11 and 80.
TEST(conformance_value, a_string_of_68_characters_fills_a_record)
{
    const std::string longest(68, 's');
    const std::string record = write_value_record("LONGEST", longest, {}, {});
    EXPECT_EQ(record[10], '\'');
    EXPECT_EQ(record[79], '\'');
    EXPECT_EQ(card::parse(record).value(), card_value(longest));

    // 34 quotes are 68 characters in the record.
    const std::string quotes(34, '\'');
    EXPECT_EQ(card::parse(write_value_record("QUOTES", quotes, {}, {})).value(), card_value(quotes));

    EXPECT_TRUE(openfits::test::throws<openfits::validation_error>(
        openfits::errc::value_too_long, [&longest] { (void)write_value_record("LONGER", longest + 's', {}, {}); }));
    EXPECT_TRUE(openfits::test::throws<openfits::validation_error>(
        openfits::errc::value_too_long, [&quotes] { (void)write_value_record("QUOTES", quotes + '\'', {}, {}); }));
}

// -------------------------------------------------------------------------------------------------------------------
// Logicals (standard §4.2.2)

TEST(conformance_value, a_logical_is_t_or_f_in_byte_30_or_the_first_character_of_a_free_value)
{
    const card fixed = card::parse("SIMPLE  =                    T");
    EXPECT_EQ(fixed.value(), card_value(true));
    EXPECT_TRUE(card_access::fixed_format(fixed));

    const card free = card::parse("EXTEND  = F / free format");
    EXPECT_EQ(free.value(), card_value(false));
    EXPECT_FALSE(card_access::fixed_format(free));

    EXPECT_EQ(write_value_record("SIMPLE", true, {}, {}), record_of("SIMPLE  =                    T"));
    EXPECT_EQ(write_value_record("EXTEND", false, {}, {}), record_of("EXTEND  =                    F"));
}

// -------------------------------------------------------------------------------------------------------------------
// Integers (standard §4.2.3)

TEST(conformance_value, an_integer_is_right_justified_in_bytes_11_to_30_or_anywhere_in_free_format)
{
    const card fixed = card::parse("NAXIS1  =                 6000");
    EXPECT_EQ(fixed.value(), card_value(6000));
    EXPECT_TRUE(card_access::fixed_format(fixed));

    const card free = card::parse("NAXIS1  = 6000");
    EXPECT_EQ(free.value(), card_value(6000));
    EXPECT_FALSE(card_access::fixed_format(free));

    EXPECT_EQ(write_value_record("NAXIS1", 6000, {}, {}), record_of("NAXIS1  =                 6000"));
    EXPECT_EQ(write_value_record("NAXIS", 0, {}, {}), record_of("NAXIS   =                    0"));
}

TEST(conformance_value, an_integer_has_an_optional_sign_and_leading_zeros_that_are_not_significant)
{
    EXPECT_EQ(value_of("+5"), card_value(5));
    EXPECT_EQ(value_of("-5"), card_value(-5));
    EXPECT_EQ(value_of("0005"), card_value(5));
    EXPECT_EQ(value_of("-0"), card_value(0));
    EXPECT_EQ(value_of("+0"), card_value(0));
    // 25 digits, of value 1.
    EXPECT_EQ(value_of("0000000000000000000000001"), card_value(1));
}

// The standard does not limit the range (standard §4.2.3): the library holds -2^63 to 2^64 - 1, 2^63 being the BZERO
// of the 64-bit unsigned convention (standard §5.2.5), and keeps the text of any other integer.
TEST(conformance_value, integers_from_minus_2_to_63_to_2_to_64_minus_1_are_held_exactly)
{
    EXPECT_EQ(value_of("-9223372036854775808"), card_value(std::numeric_limits<std::int64_t>::min()));
    EXPECT_EQ(value_of("9223372036854775807"), card_value(std::numeric_limits<std::int64_t>::max()));
    EXPECT_EQ(value_of("9223372036854775808").get<std::uint64_t>(), std::uint64_t{1} << 63U);
    EXPECT_EQ(value_of("18446744073709551615").get<std::uint64_t>(), std::numeric_limits<std::uint64_t>::max());

    for (const char* beyond : {"-9223372036854775809", "18446744073709551616", "1234567890123456789012345"}) {
        const card record = card::parse(std::string("BIG     = ") + beyond);
        EXPECT_EQ(record.value().kind(), value_kind::none) << beyond;
        EXPECT_EQ(record.value_text(), beyond);
    }

    // The 20 characters of the extremes fill the fixed format.
    EXPECT_EQ(write_value_record("BZERO", std::numeric_limits<std::int64_t>::min(), {}, {}),
              record_of("BZERO   = -9223372036854775808"));
    EXPECT_EQ(write_value_record("BZERO", std::uint64_t{1} << 63U, {}, {}),
              record_of("BZERO   =  9223372036854775808"));
    EXPECT_EQ(write_value_record("BZERO", std::numeric_limits<std::uint64_t>::max(), {}, {}),
              record_of("BZERO   = 18446744073709551615"));
}

// -------------------------------------------------------------------------------------------------------------------
// Reals (standard §4.2.4)

// A decimal number with an integer part, a fractional part or both, and an optional exponent of letter E or D.
TEST(conformance_value, a_real_is_a_decimal_number_with_an_optional_exponent)
{
    EXPECT_EQ(value_of("1.5"), card_value(1.5));
    EXPECT_EQ(value_of("1."), card_value(1.0));
    EXPECT_EQ(value_of(".5"), card_value(0.5));
    EXPECT_EQ(value_of("+.5"), card_value(0.5));
    EXPECT_EQ(value_of("-1.5"), card_value(-1.5));
    EXPECT_EQ(value_of("1.5E3"), card_value(1500.0));
    EXPECT_EQ(value_of("1.5E+3"), card_value(1500.0));
    EXPECT_EQ(value_of("1.5E-3"), card_value(0.0015));
    EXPECT_EQ(value_of("1E3"), card_value(1000.0));
    EXPECT_EQ(value_of(".5E1"), card_value(5.0));
    EXPECT_EQ(value_of("1.E1"), card_value(10.0));
    EXPECT_EQ(value_of("00012.500"), card_value(12.5));
}

// The letter D is traditionally used for more precision than single precision, with no other distinction.
TEST(conformance_value, the_exponent_letter_d_is_read_as_e)
{
    EXPECT_EQ(value_of("1.5D3"), card_value(1500.0));
    EXPECT_EQ(value_of("-2.718281828459045D-01"), card_value(-0.2718281828459045));
    EXPECT_EQ(value_of("1D0"), card_value(1.0));
}

TEST(conformance_value, a_real_is_right_justified_in_bytes_11_to_30_or_anywhere_in_free_format)
{
    const card fixed = card::parse("EXPTIME =                300.0");
    EXPECT_EQ(fixed.value(), card_value(300.0));
    EXPECT_TRUE(card_access::fixed_format(fixed));
    const card free = card::parse("EXPTIME = 300.0 / free format");
    EXPECT_FALSE(card_access::fixed_format(free));
    EXPECT_EQ(free.value(), fixed.value());
}

// The writer's form of reals: the shortest text that reads back as the value, an upper-case E, and a decimal point
// when there is no exponent, so that the value reads as a real.
TEST(conformance_value, reals_are_written_as_the_shortest_text_that_reads_back)
{
    EXPECT_EQ(write_value_record("EXPTIME", 300.0, {}, {}), record_of("EXPTIME =                 300."));
    EXPECT_EQ(written(0.1).value_text(), "0.1");
    EXPECT_EQ(written(1e20).value_text(), "1E+20");
    EXPECT_EQ(written(1.5e-7).value_text(), "1.5E-07");
    EXPECT_EQ(written(-2.5).value_text(), "-2.5");
    EXPECT_EQ(written(0.0).value_text(), "0.");
    EXPECT_EQ(written(-0.0).value_text(), "-0.");
    EXPECT_TRUE(std::signbit(written(-0.0).value().get<double>()));
    EXPECT_EQ(written(0.1F).value_text(), "0.1");
}

// The fixed format when the text has at most 20 characters, the free format from byte 11 when it has more.
TEST(conformance_value, a_real_of_20_characters_is_fixed_and_one_of_21_is_free)
{
    const std::string fixed = write_value_record("X", 1.2345678901234e300, {}, {});
    EXPECT_EQ(fixed, record_of("X       = 1.2345678901234E+300"));
    EXPECT_TRUE(card_access::fixed_format(card::parse(fixed)));

    const std::string free = write_value_record("X", 1.23456789012345e300, "a comment", {});
    EXPECT_EQ(free, record_of("X       = 1.23456789012345E+300 / a comment"));
    EXPECT_FALSE(card_access::fixed_format(card::parse(free)));
    EXPECT_EQ(card::parse(free).value(), card_value(1.23456789012345e300));

    EXPECT_EQ(written(-std::numeric_limits<double>::denorm_min()).value_text(), "-5E-324");
    EXPECT_EQ(written(std::numeric_limits<double>::lowest()).value_text(), "-1.7976931348623157E+308");
}

TEST(conformance_value, reals_reach_the_subnormals_and_the_ends_of_the_range_of_double)
{
    EXPECT_TRUE(
        same_bits(value_of("4.9406564584124654E-324").get<double>(), std::numeric_limits<double>::denorm_min()));
    EXPECT_TRUE(same_bits(value_of("2.2250738585072014E-308").get<double>(), std::numeric_limits<double>::min()));
    EXPECT_TRUE(same_bits(value_of("1.7976931348623157E+308").get<double>(), std::numeric_limits<double>::max()));
    // Below the smallest subnormal, the nearest double is a zero of the same sign.
    EXPECT_TRUE(same_bits(value_of("1E-400").get<double>(), 0.0));
    EXPECT_TRUE(same_bits(value_of("-1D-400").get<double>(), -0.0));
    // More digits than a double holds round to the nearest.
    EXPECT_EQ(value_of("3.14159265358979323846264338327950288"), card_value(3.141592653589793));
    // Beyond the largest double, no value.
    EXPECT_EQ(value_of("1.8E+308").kind(), value_kind::none);
}

// Every double reads back as itself, bit for bit.
TEST(conformance_value, every_real_round_trips_through_its_record)
{
    openfits::test::splitmix64 bits(20261026);
    int checked = 0;
    while (checked < 50'000) {
        const auto value = std::bit_cast<double>(bits.next());
        if (!std::isfinite(value)) {
            continue;
        }
        const card read_back = card::parse(write_value_record("X", value, {}, {}));
        ASSERT_TRUE(same_bits(read_back.value().get<double>(), value)) << read_back.text();
        ++checked;
    }
}

// -------------------------------------------------------------------------------------------------------------------
// Complex numbers (standard §4.2.5, §4.2.6)

// The examples of the standard: (123, 45) and (123.23, -45.7), with spaces allowed around the parts.
TEST(conformance_value, a_complex_value_is_two_parts_in_parentheses)
{
    EXPECT_EQ(value_of("(123, 45)"), card_value(complex_integer{.real = 123, .imag = 45}));
    EXPECT_EQ(value_of("( -1 ,+2 )"), card_value(complex_integer{.real = -1, .imag = 2}));
    EXPECT_EQ(value_of("(123.23, -45.7)"), card_value(std::complex<double>(123.23, -45.7)));
    EXPECT_EQ(value_of("(1.5D2,2E-1)"), card_value(std::complex<double>(150, 0.2)));
    // A floating value may omit its decimal point: two parts, one of them real, make a complex real.
    EXPECT_EQ(value_of("(1, 2.5)"), card_value(std::complex<double>(1, 2.5)));
}

// There is no fixed format for complex values: they may be anywhere in bytes 11 to 80.
TEST(conformance_value, a_complex_value_has_no_fixed_format)
{
    const card right = card::parse("CVAL    =            (123, 45)");
    EXPECT_FALSE(card_access::fixed_format(right));
    EXPECT_EQ(card::parse("CVAL    =                                       (123, 45) / far").value(),
              card_value(complex_integer{.real = 123, .imag = 45}));

    // Right-justified in bytes 11 to 30 all the same, where readers expect values.
    EXPECT_EQ(write_value_record("CVAL", complex_integer{.real = 123, .imag = 45}, {}, {}),
              record_of("CVAL    = " + std::string(11, ' ') + "(123, 45)"));
    EXPECT_EQ(written(std::complex<double>(123.23, -45.7)).value_text(), "(123.23, -45.7)");
    EXPECT_EQ(written(std::complex<double>(1, 0)).value_text(), "(1., 0.)");
}

// -------------------------------------------------------------------------------------------------------------------
// Round trips

TEST(conformance_value, every_kind_of_value_round_trips_through_its_record)
{
    const std::vector<card_value> values{
        card_value(openfits::undefined),
        card_value(true),
        card_value(false),
        card_value(0),
        card_value(-1),
        card_value(std::numeric_limits<std::int64_t>::min()),
        card_value(std::numeric_limits<std::int64_t>::max()),
        card_value(std::uint64_t{1} << 63U),
        card_value(std::numeric_limits<std::uint64_t>::max()),
        card_value(0.0),
        card_value(-0.0),
        card_value(1.0 / 3.0),
        card_value(-6.02214076e23),
        card_value(std::numeric_limits<double>::max()),
        card_value(std::numeric_limits<double>::min()),
        card_value(std::numeric_limits<double>::denorm_min()),
        card_value(complex_integer{.real = std::numeric_limits<std::int64_t>::min(), .imag = 0}),
        card_value(std::complex<double>(-1.5e-300, 2.5e300)),
        card_value(""),
        card_value(" "),
        card_value("O'HARA"),
        card_value("  leading spaces"),
        card_value("/ not a comment"),
        card_value("'"),
        card_value(std::string(68, '/')),
    };
    for (const card_value& value : values) {
        const card read_back = written(value);
        EXPECT_EQ(read_back.value(), value) << read_back.text();
    }
}

} // namespace
