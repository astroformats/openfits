// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>
#include <openfits/hdu.h>
#include <openfits/header.h>
#include <openfits/reader.h>

#include "support/files.h"
#include "support/fixture_builder.h"
#include "support/temp_directory.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace {

using openfits::card;
using openfits::errc;
using openfits::file_specification;
using openfits::header;
using openfits::region;
using openfits::section_range;
using openfits::usage_error;
using openfits::test::card_record;
using openfits::test::fixture;
using openfits::test::throws;

file_specification parse(std::string_view text)
{
    return file_specification::parse(text);
}

testing::AssertionResult refused(std::string_view text, std::string_view named = {})
{
    try {
        (void)file_specification::parse(text);
    } catch (const usage_error& failure) {
        if (failure.code() != errc::invalid_file_specification) {
            return testing::AssertionFailure() << "code " << static_cast<int>(failure.code());
        }
        if (std::string_view(failure.what()).find(named) == std::string_view::npos) {
            return testing::AssertionFailure() << "message without " << named << ": " << failure.what();
        }
        return testing::AssertionSuccess();
    }
    return testing::AssertionFailure() << "parsed " << text;
}

TEST(file_specification, parses_a_path_alone)
{
    EXPECT_EQ(parse("image.fits"), file_specification{.path = "image.fits"});
    EXPECT_EQ(parse("dir/Nebulosa del \xC3\x91"
                    "and\xC3\xBA.fits")
                  .path,
              "dir/Nebulosa del \xC3\x91"
              "and\xC3\xBA.fits");
}

TEST(file_specification, parses_a_unit_by_index_and_by_name)
{
    EXPECT_EQ(parse("a.fits[0]").hdu_index, 0U);
    EXPECT_EQ(parse("a.fits[ 12 ]").hdu_index, 12U);
    const file_specification named = parse("a.fits[SCI]");
    EXPECT_EQ(named.hdu_name, "SCI");
    EXPECT_EQ(named.hdu_version, 0);
    EXPECT_EQ(named.hdu_index, std::nullopt);
    const file_specification versioned = parse("a.fits[ SCI DATA , 2 ]");
    EXPECT_EQ(versioned.hdu_name, "SCI DATA");
    EXPECT_EQ(versioned.hdu_version, 2);
}

TEST(file_specification, parses_an_image_section)
{
    const file_specification section = parse("a.fits[1][1:512, 100:1:2, *, -*, *:3, :4, -*:2]");
    EXPECT_EQ(section.hdu_index, 1U);
    EXPECT_EQ(section.section, (std::vector<section_range>{{.first = 1, .last = 512, .step = 1},
                                                           {.first = 100, .last = 1, .step = 2},
                                                           {},
                                                           {.first = std::nullopt, .last = 1, .step = 1},
                                                           {.first = 1, .last = std::nullopt, .step = 3},
                                                           {.first = 1, .last = std::nullopt, .step = 4},
                                                           {.first = std::nullopt, .last = 1, .step = 2}}));
    const file_specification alone = parse("a.fits[10:20]");
    EXPECT_EQ(alone.hdu_index, std::nullopt);
    EXPECT_TRUE(alone.hdu_name.empty());
    EXPECT_EQ(alone.section, (std::vector<section_range>{{.first = 10, .last = 20, .step = 1}}));
    EXPECT_EQ(parse("a.fits[SCI,1][*]").section.size(), 1U);
}

TEST(file_specification, refuses_what_it_does_not_parse)
{
    EXPECT_TRUE(refused(""));
    EXPECT_TRUE(refused("[1]"));
    EXPECT_TRUE(refused("a.fits[1"));
    EXPECT_TRUE(refused("a.fits[1]x"));
    EXPECT_TRUE(refused("a.fits[]"));
    EXPECT_TRUE(refused("a.fits[ , 2]"));
    EXPECT_TRUE(refused("a.fits[SCI, two]"));
    EXPECT_TRUE(refused("a.fits[0:10]"));
    EXPECT_TRUE(refused("a.fits[1:0]"));
    EXPECT_TRUE(refused("a.fits[1:10:0]"));
    EXPECT_TRUE(refused("a.fits[1:10,5]"));
    EXPECT_TRUE(refused("a.fits[-5:10]"));
    EXPECT_TRUE(refused("a.fits[*x]"));
}

TEST(file_specification, refuses_the_filters_and_names_them)
{
    EXPECT_TRUE(refused("a.fits[1][col X; Y]", "columns"));
    EXPECT_TRUE(refused("a.fits[EVENTS][PHA > 5]", "rows"));
    EXPECT_TRUE(refused("a.fits[EVENTS][bin X,Y]", "histogram"));
    EXPECT_TRUE(refused("a.fits[1][pix X * 2]", "pixels"));
    EXPECT_TRUE(refused("a.fits[1][compress]", "compression"));
    EXPECT_TRUE(refused("a.fits[PHA > 5]", "rows"));
    EXPECT_TRUE(refused("a.fits[col X]", "columns"));
    EXPECT_TRUE(refused("a.fits[EVENTS, 2, BINTABLE]", "type"));
    EXPECT_TRUE(refused("a.fits[3; images(17)]", "table cell"));
    EXPECT_TRUE(refused("a.fits[1][1:2][3:4]", "rows"));
    EXPECT_TRUE(refused("in.fits(out.fits)", "output"));
}

TEST(file_specification, selects_a_unit_and_its_section_in_a_reader)
{
    const openfits::test::temp_directory directory;
    const std::string path = directory.file("spec.fits");
    fixture file;
    file.primary();
    file.image({10, 20, 3}, {card_record("EXTNAME", "'SCI'")});
    file.image({5}, {card_record("EXTNAME", "'SCI'"), card_record("EXTVER", "2")});
    openfits::test::write_file(openfits::test::path_of(path), file.bytes());

    const openfits::reader plain(file_specification::parse(path));
    EXPECT_EQ(plain.selected_hdu(), 0U);
    EXPECT_EQ(plain.section(), std::nullopt);

    const openfits::reader named(file_specification::parse(path + "[SCI, 2]"));
    EXPECT_EQ(named.selected_hdu(), 2U);
    EXPECT_EQ(named.section(), std::nullopt);

    const openfits::reader cut(file_specification::parse(path + "[1][2:5, 20:1:4]"));
    EXPECT_EQ(cut.selected_hdu(), 1U);
    EXPECT_EQ(cut.section(), (region{.start = {1, 0, 0}, .size = {4, 20, 3}, .step = {1, -4, 1}}));

    // A section alone applies to the first unit that holds an image.
    const openfits::reader first(file_specification::parse(path + "[-*,*:2]"));
    EXPECT_EQ(first.selected_hdu(), 1U);
    EXPECT_EQ(first.section(), (region{.start = {0, 0, 0}, .size = {10, 20, 3}, .step = {-1, 2, 1}}));

    EXPECT_TRUE(throws<usage_error>(errc::hdu_not_found,
                                    [&] { (void)openfits::reader(file_specification::parse(path + "[3]")); }));
    EXPECT_TRUE(throws<usage_error>(errc::hdu_not_found,
                                    [&] { (void)openfits::reader(file_specification::parse(path + "[MASK]")); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_file_specification,
                                    [&] { (void)openfits::reader(file_specification::parse(path + "[1][1:11]")); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_file_specification,
                                    [&] { (void)openfits::reader(file_specification::parse(path + "[1][*,*,*,*]")); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_file_specification,
                                    [&] { (void)openfits::reader(file_specification::parse(path + "[0][1:1]")); }));
}

header image_header()
{
    std::vector<card> cards;
    for (const char* record :
         {"NAXIS   =                    2", "NAXIS1  =                  100", "NAXIS2  =                  200",
          "CRPIX1  =                 50.5 / reference", "CRPIX2  =                100.0",
          "CDELT1  =               -0.001", "CDELT2  =                0.001", "CD1_1   =                  2.0",
          "CD2_1   =                  3.0", "CD1_2   =                  5.0", "CRPIX1A =                 10.0",
          "CDELT1A =                  1.5", "CDELT1B =                  7.0", "CRPIX2B =                  1.0"}) {
        cards.push_back(card::parse(record));
    }
    return header(std::move(cards));
}

TEST(file_specification, adjusts_the_header_of_a_section)
{
    // Pixels 11 to 60 of the first axis in steps of 2, and 200 to 1 of the second, in reverse.
    const header adjusted =
        openfits::adjust_for_section(image_header(), {.start = {10, 0}, .size = {50, 200}, .step = {2, -1}});
    EXPECT_EQ(adjusted.value<int>("NAXIS1"), 25);
    EXPECT_EQ(adjusted.value<int>("NAXIS2"), 200);
    EXPECT_DOUBLE_EQ(adjusted.value<double>("CRPIX1"), ((50.5 - 11.0) / 2.0) + 1.0);
    EXPECT_EQ(adjusted.comment("CRPIX1"), "reference");
    EXPECT_DOUBLE_EQ(adjusted.value<double>("CRPIX2"), (200.0 - 100.0) + 1.0);
    EXPECT_DOUBLE_EQ(adjusted.value<double>("CDELT1"), -0.002);
    EXPECT_DOUBLE_EQ(adjusted.value<double>("CDELT2"), -0.001);
    EXPECT_DOUBLE_EQ(adjusted.value<double>("CD1_1"), 4.0);
    EXPECT_DOUBLE_EQ(adjusted.value<double>("CD2_1"), 6.0);
    EXPECT_DOUBLE_EQ(adjusted.value<double>("CD1_2"), -5.0);
    EXPECT_DOUBLE_EQ(adjusted.value<double>("CRPIX1A"), ((10.0 - 11.0) / 2.0) + 1.0);
    EXPECT_DOUBLE_EQ(adjusted.value<double>("CDELT1A"), 3.0);
    // Without CRPIX1B, CDELT1B stays as it is.
    EXPECT_DOUBLE_EQ(adjusted.value<double>("CDELT1B"), 7.0);
    EXPECT_DOUBLE_EQ(adjusted.value<double>("CRPIX2B"), (200.0 - 1.0) + 1.0);
}

TEST(file_specification, leaves_the_coordinates_of_a_section_from_the_first_pixel_as_they_are)
{
    const header adjusted = openfits::adjust_for_section(image_header(), {.start = {0, 0}, .size = {10, 200}});
    EXPECT_EQ(adjusted.value<int>("NAXIS1"), 10);
    EXPECT_EQ(adjusted.value<double>("CRPIX1"), 50.5);
    EXPECT_EQ(adjusted.value<double>("CDELT1"), -0.001);
    EXPECT_EQ(adjusted[3].text(), image_header()[3].text());
    // A shift alone moves the reference pixel, and keeps the increments.
    const header shifted = openfits::adjust_for_section(image_header(), {.start = {5, 0}, .size = {10, 200}});
    EXPECT_DOUBLE_EQ(shifted.value<double>("CRPIX1"), 45.5);
    EXPECT_EQ(shifted.value<double>("CDELT1"), -0.001);
}

TEST(file_specification, rounds_the_values_it_changes_to_16_significant_digits)
{
    const header adjusted =
        openfits::adjust_for_section(image_header(), {.start = {2, 0}, .size = {3, 200}, .step = {3, 1}});
    // (50.5 - 3) / 3 + 1 is 16.833333333333336 in double, which 16 significant digits write as 1.683333333333334E+01.
    EXPECT_EQ(adjusted.value<double>("CRPIX1"), 16.83333333333334);
}

TEST(file_specification, refuses_a_region_that_is_not_one_of_the_image)
{
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [] {
        (void)openfits::adjust_for_section(image_header(), {.start = {0}, .size = {1}});
    }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [] {
        (void)openfits::adjust_for_section(image_header(), {.start = {99, 0}, .size = {2, 1}});
    }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [] {
        (void)openfits::adjust_for_section(image_header(), {.start = {0, 0}, .size = {0, 1}});
    }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [] {
        (void)openfits::adjust_for_section(image_header(), {.start = {0, 0}, .size = {1, 1}, .step = {0, 1}});
    }));
    EXPECT_TRUE(throws<openfits::invalid_data_error>(errc::keyword_not_found,
                                                     [] { (void)openfits::adjust_for_section(header(), {}); }));
}

} // namespace
