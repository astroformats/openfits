// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "core/diagnostic_log.h"

#include <openfits/error.h>
#include <openfits/limits.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using openfits::errc;
using openfits::severity;
using openfits::detail::diagnostic_log;

constexpr std::uint64_t default_cap = openfits::limits{}.max_diagnostics;

TEST(diagnostic_log, records_every_diagnostic_in_order)
{
    diagnostic_log log(false, default_cap);
    log.info(errc::invalid_value_indicator, "an info", {.hdu = 1, .keyword = "EXTNAME"});
    log.warning(errc::invalid_keyword, "a warning");
    log.error(errc::invalid_integer, "an error", {.offset = 2880});

    const std::vector<openfits::diagnostic> diagnostics = log.release();
    ASSERT_EQ(diagnostics.size(), 3U);
    EXPECT_EQ(diagnostics[0].severity, severity::info);
    EXPECT_EQ(diagnostics[0].code, errc::invalid_value_indicator);
    EXPECT_EQ(diagnostics[0].message, "an info");
    EXPECT_EQ(diagnostics[0].context, (openfits::error_context{.hdu = 1, .keyword = "EXTNAME"}));
    EXPECT_EQ(diagnostics[1].severity, severity::warning);
    EXPECT_EQ(diagnostics[2].severity, severity::error);
    EXPECT_EQ(diagnostics[2].context.offset, 2880U);
    EXPECT_TRUE(log.entries().empty());
}

TEST(diagnostic_log, add_records_a_diagnostic_of_any_severity)
{
    diagnostic_log log(false, default_cap);
    log.add(severity::warning, errc::invalid_logical, "a warning", {.keyword = "SIMPLE"});
    log.add(severity::error, errc::invalid_integer, "an error");

    ASSERT_EQ(log.entries().size(), 2U);
    EXPECT_EQ(log.entries()[0].severity, severity::warning);
    EXPECT_EQ(log.entries()[0].context.keyword, "SIMPLE");
    EXPECT_EQ(log.entries()[1].severity, severity::error);
}

// A header can hold a million problems, each of which would cost a diagnostic.
TEST(diagnostic_log, lists_up_to_its_cap_the_last_diagnostic_counting_those_that_follow)
{
    constexpr std::size_t listed = 10'000;
    diagnostic_log log(false, listed);
    for (std::size_t i = 1; i < listed; ++i) {
        log.info(errc::invalid_value_indicator, "an info", {.offset = i});
    }
    log.warning(errc::invalid_keyword, "the last diagnostic", {.hdu = 3, .keyword = "BITPIX"});
    ASSERT_EQ(log.entries().size(), listed);
    EXPECT_EQ(log.entries().back().message, "the last diagnostic");

    // The last one stands for itself and those after it: their number, the severity of the most serious, and the code
    // of the first of those.
    log.info(errc::invalid_value_indicator, "an info");
    log.error(errc::invalid_integer, "an error", {.keyword = "NAXIS1"});
    log.warning(errc::invalid_keyword, "a warning");
    log.error(errc::invalid_real, "another error");
    const std::vector<openfits::diagnostic> diagnostics = log.release();
    ASSERT_EQ(diagnostics.size(), listed);
    EXPECT_EQ(diagnostics[listed - 2].context.offset, listed - 1);
    const openfits::diagnostic& last = diagnostics.back();
    EXPECT_EQ(last.severity, severity::error);
    EXPECT_EQ(last.code, errc::invalid_integer);
    EXPECT_EQ(last.message, "5 more problems were found, which are not listed");
    EXPECT_EQ(last.context, openfits::error_context{});
}

TEST(diagnostic_log, a_cap_of_zero_lists_every_diagnostic)
{
    diagnostic_log log(false, 0);
    for (int i = 0; i < 20'000; ++i) {
        log.warning(errc::invalid_keyword, "a warning");
    }
    EXPECT_EQ(log.entries().size(), 20'000U);
}

TEST(diagnostic_log, an_error_throws_in_strict_mode)
{
    diagnostic_log log(true, default_cap);
    log.info(errc::invalid_value_indicator, "an info");
    log.warning(errc::invalid_keyword, "a warning");
    try {
        log.error(errc::invalid_integer, "an error", {.hdu = 2, .keyword = "NAXIS"});
        ADD_FAILURE() << "no exception";
    } catch (const openfits::invalid_data_error& failure) {
        EXPECT_EQ(failure.code(), errc::invalid_integer);
        EXPECT_STREQ(failure.what(), "an error (HDU 2, keyword NAXIS)");
    }
    EXPECT_EQ(log.entries().size(), 2U);
}

} // namespace
