// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Every sample compressed whole with gzip, a convention outside the standard, read as the plain file is read: its
// units, of every kind that the samples hold, their headers, the bytes of their data, their checksums and the values of
// their images. The samples D6 and D6b, gzip-compressed files of other software, are read through the other tests of
// the samples.

#include <openfits/error.h>
#include <openfits/io.h>
#include <openfits/reader.h>

#include "samples/sample_catalog.h"
#include "support/compressed_fixtures.h"
#include "support/files.h"
#include "support/temp_directory.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using openfits::reader;
using openfits::test::image_content;
using openfits::test::sample;

std::vector<std::byte> bytes_of_file(const std::string& path)
{
    const std::string text = openfits::test::read_file(openfits::test::path_of(path));
    std::vector<std::byte> data(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        data[i] = static_cast<std::byte>(text[i]);
    }
    return data;
}

// The values of the image of a unit, as the bytes of doubles, so that NaN compares equal to itself.
std::vector<double> values_of(const reader& file, std::size_t index)
{
    return file.read_pixels<double>(index, {.verify_checksum = false});
}

void expect_same_files(const reader& plain, const reader& inflated, const sample& entry)
{
    ASSERT_EQ(inflated.hdu_count(), plain.hdu_count());
    EXPECT_EQ(inflated.size(), plain.size());
    for (std::size_t i = 0; i < plain.hdu_count(); ++i) {
        const openfits::hdu_info& expected = plain.hdu(i);
        const openfits::hdu_info& unit = inflated.hdu(i);
        EXPECT_EQ(unit.kind(), expected.kind()) << i;
        EXPECT_EQ(unit.header_offset(), expected.header_offset()) << i;
        EXPECT_EQ(unit.data_size(), expected.data_size()) << i;
        ASSERT_EQ(unit.header().size(), expected.header().size()) << i;
        for (std::size_t c = 0; c < expected.header().size(); ++c) {
            EXPECT_EQ(unit.header()[c], expected.header()[c]) << i << ' ' << c;
        }
        const std::size_t size = expected.data_size().value_or(0);
        std::vector<std::byte> data(size);
        std::vector<std::byte> expected_data(data.size());
        inflated.read_raw(i, 0, data);
        plain.read_raw(i, 0, expected_data);
        EXPECT_EQ(data, expected_data) << i;
        EXPECT_EQ(inflated.verify(i), plain.verify(i)) << i;
        if (i < entry.units.size() && entry.units[i].content != image_content::none) {
            const std::vector<double> values = values_of(inflated, i);
            const std::vector<double> expected_values = values_of(plain, i);
            EXPECT_TRUE(std::ranges::equal(std::as_bytes(std::span(values)), std::as_bytes(std::span(expected_values))))
                << i;
        }
    }
}

class samples_gzip : public testing::TestWithParam<sample>
{};

TEST_P(samples_gzip, read_compressed_whole_as_they_read_plain)
{
    const sample& entry = GetParam();
    const reader plain(openfits::test::sample_path(entry));
    const std::vector<std::byte> file = bytes_of_file(openfits::test::sample_path(entry));
    const reader inflated(std::make_unique<openfits::memory_source>(openfits::test::gzip(file)));
    expect_same_files(plain, inflated, entry);

    // Inflated into a temporary file, from a file on disk.
    const openfits::test::temp_directory directory;
    openfits::test::write_file(directory.path() / "sample.fits.gz", openfits::test::gzip(file));
    const reader spilled(
        directory.file("sample.fits.gz"),
        {.inflate = {.temporary_directory = openfits::test::utf8(directory.path()), .memory_threshold = 0}});
    expect_same_files(plain, spilled, entry);
}

// The samples of plain files.
std::vector<sample> plain_samples()
{
    std::vector<sample> found;
    for (const sample& entry : openfits::test::all_samples()) {
        if (!entry.file.ends_with(".gz")) {
            found.push_back(entry);
        }
    }
    return found;
}

INSTANTIATE_TEST_SUITE_P(samples, samples_gzip, testing::ValuesIn(plain_samples()),
                         [](const testing::TestParamInfo<sample>& instance) { return std::string(instance.param.id); });

} // namespace
