// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/card.h>
#include <openfits/error.h>
#include <openfits/table.h>

#include "support/throws.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

using openfits::column_format;
using openfits::column_storage;
using openfits::column_type;
using openfits::display_code;
using openfits::display_format;
using openfits::errc;
using openfits::invalid_data_error;
using openfits::usage_error;
using openfits::test::throws;

testing::AssertionResult refused_binary(std::string_view text)
{
    if (throws<invalid_data_error>(errc::invalid_column_format, [text] { (void)column_format::parse_binary(text); })) {
        return testing::AssertionSuccess();
    }
    return testing::AssertionFailure() << "'" << text << "' was not refused";
}

testing::AssertionResult refused_ascii(std::string_view text)
{
    if (throws<invalid_data_error>(errc::invalid_column_format, [text] { (void)column_format::parse_ascii(text); })) {
        return testing::AssertionSuccess();
    }
    return testing::AssertionFailure() << "'" << text << "' was not refused";
}

// Table 18 of standard §7.3.1: every type, with the bytes of one element.
struct binary_type
{
    char code = 'L';
    column_type type = column_type::logical;
    std::uint64_t element_bytes = 1;
};

TEST(column_format, parses_every_type_of_table_18)
{
    const std::vector<binary_type> types{
        {.code = 'L', .type = column_type::logical, .element_bytes = 1},
        {.code = 'B', .type = column_type::uint8, .element_bytes = 1},
        {.code = 'I', .type = column_type::int16, .element_bytes = 2},
        {.code = 'J', .type = column_type::int32, .element_bytes = 4},
        {.code = 'K', .type = column_type::int64, .element_bytes = 8},
        {.code = 'A', .type = column_type::character, .element_bytes = 1},
        {.code = 'E', .type = column_type::float32, .element_bytes = 4},
        {.code = 'D', .type = column_type::float64, .element_bytes = 8},
        {.code = 'C', .type = column_type::complex_float32, .element_bytes = 8},
        {.code = 'M', .type = column_type::complex_float64, .element_bytes = 16},
    };
    for (const binary_type& expected : types) {
        SCOPED_TRACE(expected.code);
        const column_format alone = column_format::parse_binary(std::string(1, expected.code));
        EXPECT_EQ(alone, (column_format{.type = expected.type}));
        EXPECT_EQ(alone.field_size(), expected.element_bytes);
        const column_format repeated = column_format::parse_binary("12" + std::string(1, expected.code));
        EXPECT_EQ(repeated, (column_format{.type = expected.type, .repeat = 12}));
        EXPECT_EQ(repeated.field_size(), 12 * expected.element_bytes);
    }
}

TEST(column_format, a_bit_field_takes_the_bytes_that_hold_its_bits)
{
    // Standard Table 18, note: the number of eight-bit bytes needed to contain all bits.
    for (const auto& [bits, bytes] : std::vector<std::pair<std::uint64_t, std::uint64_t>>{
             {0, 0}, {1, 1}, {7, 1}, {8, 1}, {9, 2}, {16, 2}, {17, 3}, {64, 8}, {65, 9}}) {
        const column_format format = column_format::parse_binary(std::to_string(bits) + "X");
        EXPECT_EQ(format.type, column_type::bit);
        EXPECT_EQ(format.repeat, bits);
        EXPECT_EQ(format.field_size(), bytes) << bits;
    }
}

TEST(column_format, a_repeat_count_of_0_is_an_empty_field)
{
    // Standard §7.3.1: "A zero element count, indicating an empty field, is permitted."
    const column_format empty = column_format::parse_binary("0J");
    EXPECT_EQ(empty, (column_format{.type = column_type::int32, .repeat = 0}));
    EXPECT_EQ(empty.field_size(), 0U);
    EXPECT_EQ(column_format::parse_binary("0PE").field_size(), 0U);
}

TEST(column_format, parses_the_descriptors_of_variable_length_arrays)
{
    // Standard §7.3.5: rPt(emax) and rQt(emax), r 0, 1 or absent.
    EXPECT_EQ(column_format::parse_binary("PB(1800)"),
              (column_format{.type = column_type::uint8, .storage = column_storage::variable_p, .max_elements = 1800}));
    EXPECT_EQ(column_format::parse_binary("1PE(10)"),
              (column_format{.type = column_type::float32, .storage = column_storage::variable_p, .max_elements = 10}));
    EXPECT_EQ(column_format::parse_binary("1QD(5000000000)"),
              (column_format{
                  .type = column_type::float64, .storage = column_storage::variable_q, .max_elements = 5000000000U}));
    EXPECT_EQ(column_format::parse_binary("1PJ"),
              (column_format{.type = column_type::int32, .storage = column_storage::variable_p}));
    EXPECT_EQ(column_format::parse_binary("0QA"),
              (column_format{.type = column_type::character, .repeat = 0, .storage = column_storage::variable_q}));
    EXPECT_EQ(column_format::parse_binary("1PX(64)").type, column_type::bit);
    EXPECT_EQ(column_format::parse_binary("1PA(80)").max_elements, 80U);
    // The bytes of the descriptors in the row: two 32-bit or two 64-bit integers.
    EXPECT_EQ(column_format::parse_binary("1PE(10)").field_size(), 8U);
    EXPECT_EQ(column_format::parse_binary("QM").field_size(), 16U);
}

TEST(column_format, ignores_the_characters_after_the_form)
{
    // Standard §7.3.1, rTa: "The additional characters a are optional and are not further defined"; §7.3.5: "There may
    // be additional characters in the TFORMn keyword following the emax."
    EXPECT_EQ(column_format::parse_binary("1E extra"), (column_format{.type = column_type::float32}));
    EXPECT_EQ(column_format::parse_binary("3J(4)"), (column_format{.type = column_type::int32, .repeat = 3}));
    EXPECT_EQ(column_format::parse_binary("1PE(10) and more").max_elements, 10U);
    EXPECT_EQ(column_format::parse_binary("  2I  "), (column_format{.type = column_type::int16, .repeat = 2}));
}

TEST(column_format, parses_the_raw_convention_of_substrings)
{
    // rAw, a convention outside the standard: a field of r / w strings of w characters.
    EXPECT_EQ(column_format::parse_binary("60A10"),
              (column_format{.type = column_type::character, .repeat = 60, .substring_width = 10}));
    EXPECT_EQ(column_format::parse_binary("20A20").substring_width, 20U);
    EXPECT_EQ(column_format::parse_binary("20A").substring_width, std::nullopt);
    // A width beyond the field gives no substrings, as CFITSIO reads it.
    EXPECT_EQ(column_format::parse_binary("10A20"), (column_format{.type = column_type::character, .repeat = 10}));
    EXPECT_TRUE(refused_binary("60A0"));
}

TEST(column_format, refuses_what_is_no_binary_format)
{
    for (const std::string_view text : {"",
                                        "   ",
                                        "Z",
                                        "2",
                                        "e",
                                        "1e",
                                        "2pe",
                                        "P",
                                        "1P",
                                        "PP",
                                        "PQE",
                                        "QPE",
                                        "2PE",
                                        "10QJ(4)",
                                        "1PE(",
                                        "1PE(x)",
                                        "1PE()",
                                        "1PE(12",
                                        "18446744073709551616E",
                                        "-1E",
                                        "+1E",
                                        "1.5E",
                                        " U",
                                        "V",
                                        "W",
                                        "S"}) {
        EXPECT_TRUE(refused_binary(text));
    }
}

TEST(column_format, reads_the_largest_repeat_count)
{
    const column_format format = column_format::parse_binary("18446744073709551615B");
    EXPECT_EQ(format.repeat, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(format.field_size(), std::numeric_limits<std::uint64_t>::max());
    EXPECT_TRUE(throws<usage_error>(errc::arithmetic_overflow,
                                    [] { (void)column_format::parse_binary("4611686018427387904J").field_size(); }));
}

TEST(column_format, parses_every_format_of_table_15)
{
    // Standard §7.2.1, Table 15: Aw, Iw, Fw.d, Ew.d and Dw.d.
    EXPECT_EQ(column_format::parse_ascii("A10"),
              (column_format{.type = column_type::character,
                             .repeat = 10,
                             .field = display_format{.code = display_code::character, .width = 10}}));
    EXPECT_EQ(column_format::parse_ascii("I8"),
              (column_format{.type = column_type::int64,
                             .field = display_format{.code = display_code::integer, .width = 8}}));
    EXPECT_EQ(column_format::parse_ascii("F10.3"),
              (column_format{.type = column_type::float64,
                             .field = display_format{.code = display_code::fixed, .width = 10, .digits = 3}}));
    EXPECT_EQ(column_format::parse_ascii("E15.7"),
              (column_format{.type = column_type::float64,
                             .field = display_format{.code = display_code::exponential, .width = 15, .digits = 7}}));
    EXPECT_EQ(
        column_format::parse_ascii("D25.17"),
        (column_format{.type = column_type::float64,
                       .field = display_format{.code = display_code::double_exponential, .width = 25, .digits = 17}}));
    EXPECT_EQ(column_format::parse_ascii(" F8.0 ").field,
              (display_format{.code = display_code::fixed, .width = 8, .digits = 0}));
    EXPECT_EQ(column_format::parse_ascii("A10").field_size(), 10U);
    EXPECT_EQ(column_format::parse_ascii("D25.17").field_size(), 25U);
}

TEST(column_format, refuses_what_is_no_ascii_format)
{
    for (const std::string_view text :
         {"",     "A",       "A0", "I0", "F10", "F10.", "F.3", "F0.0", "F10.10", "E5.7",     "f10.3",    "a10", "G10.3",
          "I8.2", "E15.7E3", "L1", "Z8", "B8",  "O8",   "10A", "1E",   "F10.3x", "I8 extra", "A1048577", "I-3"}) {
        EXPECT_TRUE(refused_ascii(text));
    }
    EXPECT_EQ(column_format::parse_ascii("A1048576").repeat, display_format::max_width);
}

TEST(column_format, writes_what_it_reads)
{
    for (const std::string_view text : {"1L", "8X", "1B", "2I", "3J", "1K", "20A", "1E", "1D", "4C", "1M", "0J",
                                        "60A10", "1PE(10)", "1QD(5000000000)", "0PA", "1PX"}) {
        const column_format format = column_format::parse_binary(text);
        EXPECT_EQ(format.to_string(), text);
        EXPECT_EQ(column_format::parse_binary(format.to_string()), format);
    }
    for (const std::string_view text : {"A10", "I8", "F10.3", "E15.7", "D25.17"}) {
        const column_format format = column_format::parse_ascii(text);
        EXPECT_EQ(format.to_string(), text);
        EXPECT_EQ(column_format::parse_ascii(format.to_string()), format);
    }
    // The repeat count is always written.
    EXPECT_EQ(column_format::parse_binary("E").to_string(), "1E");
    EXPECT_EQ(column_format::parse_binary("PB(1800)").to_string(), "1PB(1800)");
}

TEST(column_format, refuses_to_write_a_format_that_no_tformn_holds)
{
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [] {
        (void)column_format{.type = column_type::float64, .field = display_format{.code = display_code::general}}
            .to_string();
    }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [] {
        (void)column_format{.type = column_type::logical, .field = display_format{.code = display_code::logical}}
            .to_string();
    }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [] {
        (void)column_format{.type = column_type::int32, .repeat = 2, .storage = column_storage::variable_p}.to_string();
    }));
}

} // namespace
