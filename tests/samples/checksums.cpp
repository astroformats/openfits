// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The checksums of the samples of other software (standard §4.4.2.7): what the catalogue says of the DATASUM and the
// CHECKSUM of each unit, found by the reader, at a strict open and in the reads of whole images, which verify DATASUM.

#include <openfits/checksum.h>
#include <openfits/reader.h>

#include "samples/sample_catalog.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

namespace {

using openfits::checksum_state;
using openfits::reader;
using openfits::test::image_content;
using openfits::test::sample;

class samples_checksums : public testing::TestWithParam<sample>
{};

TEST_P(samples_checksums, verify_as_the_catalogue_says)
{
    const sample& entry = GetParam();
    const reader file(openfits::test::sample_path(entry));
    ASSERT_EQ(file.hdu_count(), entry.units.size());
    for (std::size_t i = 0; i < entry.units.size(); ++i) {
        const openfits::checksum_report report = file.verify(i);
        EXPECT_EQ(report.datasum, entry.units[i].datasum) << i;
        EXPECT_EQ(report.checksum, entry.units[i].checksum) << i;
        if (entry.units[i].checksum == checksum_state::valid) {
            EXPECT_EQ(report.hdu_sum, 0xFFFFFFFFU) << i;
        }
    }
}

TEST_P(samples_checksums, open_strictly_and_read_their_images_whole)
{
    const sample& entry = GetParam();
    const reader file(openfits::test::sample_path(entry), {.strict = true});
    for (std::size_t i = 0; i < entry.units.size(); ++i) {
        if (entry.units[i].content != image_content::none && entry.units[i].datasum == checksum_state::valid) {
            EXPECT_NO_THROW((void)file.read_pixels<double>(i)) << i;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(samples, samples_checksums, testing::ValuesIn(openfits::test::all_samples()),
                         [](const testing::TestParamInfo<sample>& instance) { return std::string(instance.param.id); });

} // namespace
