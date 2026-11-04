// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The conformance report of each sample of other software: the departures that the catalogue lists, and nothing else.

#include <openfits/error.h>
#include <openfits/verify.h>

#include "samples/sample_catalog.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using openfits::diagnostic;
using openfits::test::sample;

class samples_verify : public testing::TestWithParam<sample>
{};

TEST_P(samples_verify, report_the_departures_of_the_catalogue_alone)
{
    const sample& entry = GetParam();
    const std::vector<diagnostic> report = openfits::verify(openfits::test::sample_path(entry));
    for (const diagnostic& finding : report) {
        const bool listed = std::ranges::find(entry.departures, finding.code) != entry.departures.end() ||
                            std::ranges::find(entry.report_departures, finding.code) != entry.report_departures.end();
        EXPECT_TRUE(listed) << "[" << finding.section << "] " << finding.message << " ("
                            << static_cast<int>(finding.code) << ")";
        EXPECT_NE(finding.severity, openfits::severity::error) << finding.message;
    }
    for (const openfits::errc code : entry.report_departures) {
        EXPECT_TRUE(std::ranges::any_of(report, [code](const diagnostic& finding) { return finding.code == code; }))
            << static_cast<int>(code);
    }
}

INSTANTIATE_TEST_SUITE_P(samples, samples_verify, testing::ValuesIn(openfits::test::all_samples()),
                         [](const testing::TestParamInfo<sample>& instance) { return std::string(instance.param.id); });

} // namespace
