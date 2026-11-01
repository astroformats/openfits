// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "core/quote.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using openfits::detail::quote;

TEST(quote, puts_printable_text_in_single_quotes)
{
    EXPECT_EQ(quote("EXPTIME"), "'EXPTIME'");
    EXPECT_EQ(quote(""), "''");
}

TEST(quote, escapes_quotes_backslashes_and_other_bytes)
{
    EXPECT_EQ(quote("O'HARA"), "'O\\'HARA'");
    EXPECT_EQ(quote("a\\b"), "'a\\\\b'");
    EXPECT_EQ(quote(std::string("\t\x7F\xB0\0", 4)), "'\\x09\\x7F\\xB0\\x00'");
}

TEST(quote, cuts_long_text_after_40_bytes)
{
    const std::string text(41, 'x');
    EXPECT_EQ(quote(text), "'" + std::string(40, 'x') + "'...");
    EXPECT_EQ(quote(text.substr(0, 40)), "'" + std::string(40, 'x') + "'");
}

} // namespace
