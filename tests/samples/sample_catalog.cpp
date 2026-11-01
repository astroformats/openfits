// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "samples/sample_catalog.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace openfits::test {

namespace {

constexpr std::uint64_t width = 37;
constexpr std::uint64_t height = 23;
constexpr std::uint64_t u16_pattern = width * height * 2;
constexpr std::uint64_t f32_pattern = width * height * 4;

// A unit of an image, built member by member: copies of aggregates of vectors built with nested designated
// initializers trip a false positive of GCC 16 (-Wmaybe-uninitialized).
sample_unit image_unit(hdu_kind kind, std::uint64_t data_size, numeric_type type, std::vector<std::uint64_t> axes,
                       image_content content = image_content::pattern, std::string_view name = {})
{
    sample_unit unit;
    unit.kind = kind;
    unit.name = name;
    unit.data_size = data_size;
    unit.content = content;
    unit.value_type = type;
    unit.axes = std::move(axes);
    return unit;
}

// The primary HDU of the pattern, of stored values of size bytes, read as values of type.
sample_unit pattern_unit(std::uint64_t size, numeric_type type, std::string_view name = {})
{
    return image_unit(hdu_kind::primary, width * height * size, type, {width, height}, image_content::pattern, name);
}

sample_unit unit_of(hdu_kind kind, std::uint64_t data_size, std::string_view name = {})
{
    sample_unit unit;
    unit.kind = kind;
    unit.name = name;
    unit.data_size = data_size;
    return unit;
}

std::vector<sample> make_samples()
{
    std::vector<sample> samples;
    const auto add = [&samples](sample unit) { samples.push_back(std::move(unit)); };
    const std::string_view astropy = "astropy 8.0.1 (numpy 2.5.4)";
    const std::string_view astropy_script = "tests/data/scripts/make_astropy.py";
    const auto astropy_image = [&](std::string_view id, std::string_view file, std::string_view content,
                                   sample_unit unit) {
        sample entry;
        entry.id = id;
        entry.file = file;
        entry.content = content;
        entry.producer = astropy;
        entry.procedure = astropy_script;
        entry.units.push_back(std::move(unit));
        add(std::move(entry));
    };

    astropy_image("A1", "astropy/a1-u8.fits", "the pattern as BITPIX 8", pattern_unit(1, numeric_type::uint8));
    astropy_image("A2", "astropy/a2-s8.fits", "the pattern as BITPIX 8 with BZERO -128, signed bytes",
                  pattern_unit(1, numeric_type::int8));
    astropy_image("A3", "astropy/a3-i16.fits", "the pattern as BITPIX 16", pattern_unit(2, numeric_type::int16));
    astropy_image("A4", "astropy/a4-u16.fits",
                  "the pattern as BITPIX 16 with BZERO 32768, the unsigned convention, and OBJECT; its values rounded "
                  "in double",
                  pattern_unit(2, numeric_type::uint16));
    astropy_image("A5", "astropy/a5-i32.fits", "the pattern as BITPIX 32", pattern_unit(4, numeric_type::int32));
    astropy_image("A6", "astropy/a6-u32.fits", "the pattern as BITPIX 32 with BZERO 2147483648",
                  pattern_unit(4, numeric_type::uint32));
    astropy_image("A7", "astropy/a7-i64.fits", "the pattern as BITPIX 64", pattern_unit(8, numeric_type::int64));
    astropy_image("A8", "astropy/a8-u64.fits", "the pattern as BITPIX 64 with BZERO 9223372036854775808",
                  pattern_unit(8, numeric_type::uint64));
    astropy_image("A9", "astropy/a9-f32.fits", "the pattern as BITPIX -32", pattern_unit(4, numeric_type::float32));

    sample_unit a10 = pattern_unit(8, numeric_type::float64);
    a10.specials = {{.index = 1, .value = std::numeric_limits<double>::quiet_NaN()},
                    {.index = 2, .value = std::numeric_limits<double>::infinity()},
                    {.index = 3, .value = -0.0}};
    astropy_image("A10", "astropy/a10-f64.fits", "the pattern as BITPIX -64, with NaN, +inf and -0.0 at pixels 1 to 3",
                  std::move(a10));

    sample_unit a11 = image_unit(hdu_kind::primary, u16_pattern, numeric_type::float32, {width, height},
                                 image_content::scaled_pattern);
    a11.specials = {{.index = 0, .value = std::numeric_limits<double>::quiet_NaN()},
                    {.index = 400, .value = std::numeric_limits<double>::quiet_NaN()},
                    {.index = 850, .value = std::numeric_limits<double>::quiet_NaN()}};
    astropy_image("A11", "astropy/a11-scaled.fits",
                  "the stored values of A3 with BSCALE 0.01, BZERO 100 and BLANK -32768, at pixels 0, 400 and 850",
                  std::move(a11));
    astropy_image("A12", "astropy/a12-rgb-cube.fits",
                  "the three channels of the pattern as NAXIS3 = 3, BITPIX 16 with BZERO 32768, and CTYPE3 = 'RGB'",
                  image_unit(hdu_kind::primary, u16_pattern * 3, numeric_type::uint16, {width, height, 3}));

    sample a13;
    a13.id = "A13";
    a13.file = "astropy/a13-mef.fits";
    a13.content = "an empty primary HDU with ORIGIN, and three IMAGE extensions: the pattern as BITPIX 16 (SCI 1, with "
                  "FILTER), as BITPIX -32 (SCI 2, with GAIN), and an 11 x 7 BITPIX 8 image (MASK, with MASKED)";
    a13.producer = astropy;
    a13.procedure = astropy_script;
    a13.units.push_back(unit_of(hdu_kind::primary, 0));
    a13.units.push_back(
        image_unit(hdu_kind::image, u16_pattern, numeric_type::uint16, {width, height}, image_content::pattern, "SCI"));
    a13.units.push_back(image_unit(hdu_kind::image, f32_pattern, numeric_type::float32, {width, height},
                                   image_content::pattern, "SCI"));
    a13.units.push_back(
        image_unit(hdu_kind::image, std::uint64_t{11} * 7, numeric_type::uint8, {11, 7}, image_content::mask, "MASK"));
    add(std::move(a13));

    sample a14;
    a14.id = "A14";
    a14.file = "astropy/a14-header-only.fits";
    a14.content = "a primary HDU of NAXIS = 0 with a header of an observation, invented, COMMENT and HISTORY";
    a14.producer = astropy;
    a14.procedure = astropy_script;
    a14.units.push_back(unit_of(hdu_kind::primary, 0));
    add(std::move(a14));

    astropy_image(
        "A15", "astropy/a15-4d.fits", "a 5 x 4 x 3 x 2 cube of BITPIX 16, each value its index",
        image_unit(hdu_kind::primary, std::uint64_t{120} * 2, numeric_type::int16, {5, 4, 3, 2}, image_content::index));

    sample d1;
    d1.id = "D1";
    d1.file = "astropy/d1-rich-header.fits";
    d1.content = "A4 with every value form: logicals, integers to the 64-bit extremes, reals with E and D exponents, "
                 "complex integers and reals, the undefined value, strings with quotes, leading and trailing spaces, "
                 "the null and the empty string, COMMENT, HISTORY and blank cards, a long string with a continued "
                 "comment, LONGSTRN, HIERARCH cards of each shape, units in comments, DATE, DATE-OBS, WCS keywords "
                 "with an alternate, INHERIT, and 40 blank cards before END";
    d1.producer = astropy;
    d1.procedure = astropy_script;
    d1.set_by_hand = "the records after BZERO, given to astropy as card images, which it writes as they are";
    d1.units.push_back(pattern_unit(2, numeric_type::uint16));
    add(std::move(d1));

    sample e1;
    e1.id = "E1";
    e1.file = "pixinsight/e1-pixinsight-u16.fits";
    e1.content = "the pattern written by PixInsight as BITPIX 16 with BZERO 32768, with OBJECT, EXPTIME, a HISTORY "
                 "record, and its own keywords: PROGRAM, HDUNAME, ROWORDER and COMMENT records. The FITS module "
                 "writes no ICCProfile unit in automation mode";
    e1.producer = "PixInsight 1.9.5 build 1706, FITS module 1.2.3";
    e1.procedure = "tests/data/scripts/make_pixinsight.js, run with PixInsight 1.9.5; E2 is from a run with 1.9.6";
    e1.units.push_back(pattern_unit(2, numeric_type::uint16, "openfits_sample_e1"));
    add(std::move(e1));

    sample e2;
    e2.id = "E2";
    e2.file = "pixinsight/e2-pixinsight-rgb-f32.fits";
    e2.content = "the three channels of the pattern written by PixInsight as an RGB image of BITPIX -32, a cube of "
                 "NAXIS3 = 3, with OBJECT, PROGRAM, HDUNAME, ROWORDER = 'TOP-DOWN' and COMMENT records";
    e2.producer = "PixInsight 1.9.6 build 1713, FITS module 1.3.0";
    e2.procedure = "tests/data/scripts/make_pixinsight.js";
    e2.units.push_back(image_unit(hdu_kind::primary, f32_pattern * 3, numeric_type::float32, {width, height, 3},
                                  image_content::pattern, "openfits_sample_e2"));
    add(std::move(e2));

    sample e3;
    e3.id = "E3";
    e3.file = "siril/e3-siril-f32.fits";
    e3.content = "the pattern saved by Siril as BITPIX -32, with PROGRAM, MIPS-FLO, DATE, the ROWORDER of its input, "
                 "a HISTORY record of the sRGB profile assigned, and the ICCProfile unit of the profile, 624 bytes";
    e3.producer = "Siril 1.4.4";
    e3.procedure = "tests/data/scripts/make_siril.py with make_siril.ssf";
    e3.units.push_back(pattern_unit(4, numeric_type::float32));
    e3.units.push_back(unit_of(hdu_kind::image, 624, "ICCProfile"));
    add(std::move(e3));

    sample e4;
    e4.id = "E4";
    e4.file = "siril/e4-siril-fitseq.fits";
    e4.content = "a FITSEQ sequence of three frames of the pattern as BITPIX -32, each with the FILENAME of its input";
    e4.producer = "Siril 1.4.4";
    e4.procedure = "tests/data/scripts/make_siril.py with make_siril.ssf (convert -fitseq)";
    e4.units.push_back(pattern_unit(4, numeric_type::float32));
    for (int frame = 0; frame < 2; ++frame) {
        e4.units.push_back(image_unit(hdu_kind::image, f32_pattern, numeric_type::float32, {width, height}));
    }
    add(std::move(e4));

    sample e6;
    e6.id = "E6";
    e6.file = "siril/e6-siril-cfa.fits";
    e6.content = "a synthetic CFA frame of the pattern as BITPIX 16 with BZERO 32768, with BAYERPAT, XBAYROFF and "
                 "YBAYROFF";
    e6.producer = "Siril 1.4.4";
    e6.procedure = "tests/data/scripts/make_siril.py with make_siril.ssf";
    e6.set_by_hand = "the Bayer keywords of the input, which Siril keeps";
    e6.units.push_back(pattern_unit(2, numeric_type::uint16));
    add(std::move(e6));

    const std::string_view capture = "tests/data/scripts/make_capture.py";
    const auto capture_sample = [&](std::string_view id, std::string_view file, std::string_view content) {
        sample entry;
        entry.id = id;
        entry.file = file;
        entry.content = content;
        entry.producer = "a script, from the documented format";
        entry.procedure = capture;
        entry.set_by_hand = "every value, invented";
        entry.units.push_back(pattern_unit(2, numeric_type::uint16));
        return entry;
    };
    add(capture_sample("E7a", "capture/e7-capture-sbfitsext.fits",
                       "the keywords of the SBFITSEXT 1.0 convention, as MaxIm DL writes them, with invented values"));
    add(capture_sample("E7b", "capture/e7-capture-roworder.fits",
                       "the keywords of a capture program for a colour camera: BAYERPAT, XBAYROFF, YBAYROFF, "
                       "ROWORDER = 'TOP-DOWN', units in brackets in comments, DATE-LOC"));
    sample e7c = capture_sample(
        "E7c", "capture/e7-capture-latin1.fits",
        "comments with the degree sign of Latin-1, byte 0xB0, and a string with an accented letter, byte 0xED");
    e7c.departures = {errc::invalid_character};
    add(std::move(e7c));
    return samples;
}

} // namespace

const std::vector<sample>& all_samples()
{
    static const std::vector<sample> samples = make_samples();
    return samples;
}

const sample& sample_by_id(std::string_view id)
{
    const std::vector<sample>& samples = all_samples();
    const auto found = std::ranges::find(samples, id, &sample::id);
    if (found == samples.end()) {
        throw std::logic_error("no sample " + std::string(id));
    }
    return *found;
}

std::string sample_path(const sample& unit)
{
    return std::string(OPENFITS_TEST_DATA_DIR) + "/" + std::string(unit.file);
}

void PrintTo(const sample& unit, std::ostream* output)
{
    *output << unit.id;
}

} // namespace openfits::test
