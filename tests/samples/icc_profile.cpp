// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The ICCProfile unit of Siril, which holds the ICC profile of an image after it, a convention of no standard: found by
// its name, copied byte for byte, and kept by the editor through every change of layout. Siril writes it as an IMAGE
// extension of BITPIX 8 and NAXIS 1 whose data are the bytes of the profile (E3), and in a compressed file with the
// keywords of a compressed image in its header as well (E5); PixInsight 1.9.6 writes and reads none.

#include <openfits/editor.h>
#include <openfits/hdu.h>
#include <openfits/header.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/writer.h>

#include "samples/sample_catalog.h"
#include "support/files.h"
#include "support/temp_directory.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using openfits::reader;
using openfits::test::sample;

// The samples whose files hold an ICCProfile unit.
std::vector<sample> with_profiles()
{
    return {openfits::test::sample_by_id("E3"), openfits::test::sample_by_id("E5")};
}

// The profile that the samples were given, tests/data/icc/openfits-gray.icc of make_icc.py.
std::string profile()
{
    return openfits::test::read_file(openfits::test::path_of(OPENFITS_TEST_DATA_DIR "/icc/openfits-gray.icc"));
}

// The bytes of a unit of a file, its header and its data records.
std::string unit_bytes(const std::string& file, const openfits::hdu_info& unit)
{
    const std::uint64_t data = unit.data_size().value_or(0);
    const std::uint64_t records = (data + 2879) / 2880 * 2880;
    return file.substr(unit.header_offset(), unit.header_size() + records);
}

class samples_icc_profile : public testing::TestWithParam<sample>
{};

TEST_P(samples_icc_profile, is_found_by_its_name_with_the_profile_as_its_data)
{
    const sample& entry = GetParam();
    const reader file(openfits::test::sample_path(entry));
    const openfits::hdu_info& unit = file.hdu("ICCProfile");
    EXPECT_EQ(unit.index(), entry.units.size() - 1);
    EXPECT_EQ(unit.kind(), openfits::hdu_kind::image);
    const std::string expected = profile();
    ASSERT_EQ(unit.data_size(), expected.size());
    std::string read(expected.size(), '\0');
    file.read_raw(unit.index(), 0, std::as_writable_bytes(std::span(read)));
    EXPECT_EQ(read, expected);
    // As the image of bytes that its header describes.
    const std::vector<std::uint8_t> values = file.read_pixels<std::uint8_t>(unit.index());
    EXPECT_EQ(std::string(values.begin(), values.end()), expected);
}

// Each unit of the sample copied as it is, the ICCProfile unit among them, makes the same file.
TEST_P(samples_icc_profile, is_copied_byte_for_byte)
{
    const sample& entry = GetParam();
    const std::string original = openfits::test::sample_content(entry);
    const reader file(openfits::test::sample_path(entry));
    openfits::writer copy;
    for (std::size_t i = 0; i < file.hdu_count(); ++i) {
        (void)copy.add_hdu(file, i);
    }
    openfits::memory_sink sink;
    copy.save(sink);
    const std::span<const std::byte> written = sink.data();
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(written.data()), written.size()), original);
}

// Changes of the layout before the unit, through a copy, in place and atomically: the header of the primary HDU grown
// by blocks, and a unit inserted before the ICCProfile unit; the unit is the same bytes afterwards.
TEST_P(samples_icc_profile, is_kept_by_the_editor_through_every_relayout)
{
    const sample& entry = GetParam();
    const std::string original = openfits::test::sample_content(entry);
    const reader source(openfits::test::sample_path(entry));
    const std::string expected = unit_bytes(original, source.hdu("ICCProfile"));
    const openfits::test::temp_directory directory;
    const std::string path = directory.file("edited.fits");

    struct mode
    {
        openfits::relayout relayout = openfits::relayout::safe_copy;
        bool atomic = false;
    };
    for (const mode& commit :
         {mode{.relayout = openfits::relayout::safe_copy}, mode{.relayout = openfits::relayout::in_place},
          mode{.relayout = openfits::relayout::in_place, .atomic = true}}) {
        openfits::test::write_file(openfits::test::path_of(path), std::as_bytes(std::span(original)));
        {
            openfits::editor file(path, {.relayout = commit.relayout, .atomic = commit.atomic});
            openfits::header& primary = file.header(0);
            for (int i = 0; i < 80; ++i) {
                primary.set("NOTE" + std::to_string(i), "a keyword that grows the header by blocks");
            }
            openfits::writer inserted;
            (void)inserted.add_header(openfits::header());
            file.insert_hdu(file.file().hdu("ICCProfile").index(), std::move(inserted));
            file.commit();
        }
        const std::string edited = openfits::test::read_file(openfits::test::path_of(path));
        const reader result(path);
        ASSERT_EQ(result.hdu_count(), entry.units.size() + 1);
        const openfits::hdu_info& unit = result.hdu("ICCProfile");
        EXPECT_EQ(unit.index(), entry.units.size());
        EXPECT_EQ(unit_bytes(edited, unit), expected)
            << "relayout " << static_cast<int>(commit.relayout) << " atomic " << commit.atomic;
    }
}

INSTANTIATE_TEST_SUITE_P(samples, samples_icc_profile, testing::ValuesIn(with_profiles()),
                         [](const testing::TestParamInfo<sample>& instance) { return std::string(instance.param.id); });

} // namespace
