// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "samples/sample_catalog.h"

#include <openfits/io.h>

#include "support/files.h"
#include "support/temp_directory.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>
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
    // The samples of gzip-compressed files, a convention outside the standard, hold A13.
    const std::vector<sample_unit> mef_units = a13.units;
    add(std::move(a13));

    const std::string_view gzip_producer = "Python 3.14.4, its gzip module at level 9 (zlib)";
    const std::string_view gzip_script = "tests/data/scripts/make_gzip.py";
    sample d6;
    d6.id = "D6";
    d6.file = "gzip/d6-gzip.fits.gz";
    d6.content = "A13 compressed whole with gzip, one member without a name and with a time of 0";
    d6.producer = gzip_producer;
    d6.procedure = gzip_script;
    d6.units = mef_units;
    add(std::move(d6));

    sample d6b;
    d6b.id = "D6b";
    d6b.file = "gzip/d6b-gzip-members.fits.gz";
    d6b.content =
        "A13 compressed with gzip as two members, its primary HDU in the first and its extensions in the second";
    d6b.producer = gzip_producer;
    d6b.procedure = gzip_script;
    d6b.units = mef_units;
    add(std::move(d6b));

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
    // INHERIT, a keyword of extensions, which standard Appendix K keeps out of the primary header.
    d1.departures = {errc::reserved_keyword};
    add(std::move(d1));

    sample d2;
    d2.id = "D2";
    d2.file = "astropy/d2-bintable.fits";
    d2.content = "a binary table CATALOG of five rows with every type of Table 18: L, 13X, B, B of signed bytes, I, I "
                 "of unsigned 16-bit integers, J and J unsigned, K and K unsigned, E with a NaN, an infinity and a "
                 "negative zero, D with TUNIT and TDISP, 8A, C, M, 6I with TDIM '(3,2)', 0J, a scaled J, a J with "
                 "TNULL, PJ and QD arrays with emax, 12A4 of the rAw convention, and PA strings";
    d2.producer = astropy;
    d2.procedure = astropy_script;
    d2.set_by_hand = "THEAP = 809 and PCOUNT = 209, which put the heap 104 bytes after the rows; TSCAL18 = 0.25 and "
                     "TZERO18 = 100.0, since astropy 8.0.1 does not write the physical values of a scaled integer "
                     "column, which it was given as stored";
    d2.units.push_back(unit_of(hdu_kind::primary, 0));
    d2.units.push_back(unit_of(hdu_kind::bintable, (std::uint64_t{141} * 5) + 209, "CATALOG"));
    add(std::move(d2));

    sample d3;
    d3.id = "D3";
    d3.file = "astropy/d3-ascii-table.fits";
    d3.content = "an ASCII table of four rows with A10, I6 (TNULL '*'), F10.3, E12.5 and D20.12 fields";
    d3.producer = astropy;
    d3.procedure = astropy_script;
    d3.set_by_hand = "the I6 field of row 2 blank, which is 0, and its F10.3 field 12345 without a decimal point, "
                     "12.345 with the implied one; the I6 field of row 3 the TNULLn string";
    d3.units.push_back(unit_of(hdu_kind::primary, 0));
    d3.units.push_back(unit_of(hdu_kind::table, std::uint64_t{58} * 4, "ASCII"));
    // The implied decimal point, which standard §7.2.5 deprecates.
    d3.report_departures = {errc::invalid_field};
    add(std::move(d3));

    sample d4;
    d4.id = "D4";
    d4.file = "astropy/d4-random-groups.fits";
    d4.content = "random groups: three groups of a 7 x 5 array of 32-bit integers, each value 100 g plus its index, "
                 "and four parameters, UU, VV and DATE twice, which are summed";
    d4.producer = astropy;
    d4.procedure = astropy_script;
    d4.set_by_hand = "PSCAL1 = 1E-09, PZERO3 = 2460000.5 and PSCAL4 = 0.001, since astropy 8.0.1 writes wrong stored "
                     "values for parameters given with a scaling";
    d4.units.push_back(unit_of(hdu_kind::random_groups, std::uint64_t{3} * (4 + 35) * 4));
    add(std::move(d4));

    // The samples of checksums. astropy writes the time of the run as their comment, which would change the bytes at
    // each run, unless given one.
    const std::string_view checksum_time = "the comment of CHECKSUM and DATASUM, 2026-11-02T00:00:00, given to "
                                           "astropy's add_checksum(), which writes the time of the run otherwise";
    const auto checked = [](sample_unit unit) {
        unit.datasum = checksum_state::valid;
        unit.checksum = checksum_state::valid;
        return unit;
    };
    sample c1;
    c1.id = "C1";
    c1.file = "astropy/c1-u16-checksum.fits";
    c1.content = "A4 with CHECKSUM and DATASUM after its cards";
    c1.producer = astropy;
    c1.procedure = astropy_script;
    c1.set_by_hand = checksum_time;
    c1.units.push_back(checked(pattern_unit(2, numeric_type::uint16)));
    add(std::move(c1));

    sample c2;
    c2.id = "C2";
    c2.file = "astropy/c2-mef-checksum.fits";
    c2.content = "A13 with CHECKSUM and DATASUM in every unit, the empty primary HDU of DATASUM '0' included";
    c2.producer = astropy;
    c2.procedure = astropy_script;
    c2.set_by_hand = checksum_time;
    c2.units.push_back(checked(unit_of(hdu_kind::primary, 0)));
    c2.units.push_back(checked(image_unit(hdu_kind::image, u16_pattern, numeric_type::uint16, {width, height},
                                          image_content::pattern, "SCI")));
    c2.units.push_back(checked(image_unit(hdu_kind::image, f32_pattern, numeric_type::float32, {width, height},
                                          image_content::pattern, "SCI")));
    c2.units.push_back(checked(
        image_unit(hdu_kind::image, std::uint64_t{11} * 7, numeric_type::uint8, {11, 7}, image_content::mask, "MASK")));
    add(std::move(c2));

    sample c4;
    c4.id = "C4";
    c4.file = "astropy/c4-checksum-blank.fits";
    c4.content = "C1 with CHECKSUM and DATASUM blank strings, which assert nothing of the sums (standard §4.4.2.7)";
    c4.producer = astropy;
    c4.procedure = astropy_script;
    c4.set_by_hand = "the values of CHECKSUM and DATASUM of C1 made blanks of their widths, by the script";
    c4.units.push_back(pattern_unit(2, numeric_type::uint16));
    add(std::move(c4));

    // The samples of fpack: an empty primary HDU of BITPIX 16, then the compressed image, named COMPRESSED_IMAGE.
    const std::string_view fpack = "fpack 1.7.0 of CFITSIO 4.7.0";
    const auto compressed = [](std::uint64_t data_size, numeric_type type, std::string_view name = "COMPRESSED_IMAGE",
                               std::vector<std::uint64_t> axes = {width, height},
                               image_content content = image_content::pattern) {
        return image_unit(hdu_kind::compressed_image, data_size, type, std::move(axes), content, name);
    };
    const auto fpack_image = [&](std::string_view id, std::string_view file, std::string_view content,
                                 std::string_view options, sample_unit unit) {
        sample entry;
        entry.id = id;
        entry.file = file;
        entry.content = content;
        entry.producer = fpack;
        entry.procedure = options;
        entry.units.push_back(unit_of(hdu_kind::primary, 0));
        entry.units.push_back(std::move(unit));
        add(std::move(entry));
    };
    fpack_image("B1", "fpack/b1-u16-rice.fits", "A4 compressed with RICE_1 in tiles of one row, BYTEPIX 2",
                "fpack -C -r, tests/data/scripts/make_fpack.py", compressed(1219, numeric_type::uint16));
    fpack_image("B2", "fpack/b2-u16-gzip1.fits", "A4 compressed with GZIP_1 in tiles of one row",
                "fpack -C -g1, tests/data/scripts/make_fpack.py", compressed(2415, numeric_type::uint16));
    fpack_image("B3", "fpack/b3-u16-gzip2.fits", "A4 compressed with GZIP_2 in tiles of one row",
                "fpack -C -g2, tests/data/scripts/make_fpack.py", compressed(2303, numeric_type::uint16));
    fpack_image("B4", "fpack/b4-u16-plio.fits",
                "A4 compressed with PLIO_1 in tiles of one row, the unsigned values plus 32768 of CFITSIO",
                "fpack -C -p, tests/data/scripts/make_fpack.py", compressed(2292, numeric_type::uint16));
    fpack_image("B5", "fpack/b5-u16-hcomp.fits",
                "A4 compressed with HCOMPRESS_1 at scale 0, lossless, in one tile of the 23 rows of the image",
                "fpack -C -h, tests/data/scripts/make_fpack.py", compressed(768, numeric_type::uint16));
    fpack_image("B6", "fpack/b6-u16-rice-tiles.fits",
                "A4 compressed with RICE_1 in tiles of 16 x 16 pixels, which divide neither axis",
                "fpack -C -r -t 16,16, tests/data/scripts/make_fpack.py", compressed(1147, numeric_type::uint16));
    fpack_image("B7", "fpack/b7-u16-nocompress.fits",
                "A4 as NOCOMPRESS tiles of one row, in the column UNCOMPRESSED_DATA of 16-bit integers",
                "fpack -C -d, tests/data/scripts/make_fpack.py", compressed(2070, numeric_type::uint16));
    fpack_image("B8", "fpack/b8-f32-rice-q4.fits",
                "A9 for RICE_1 at q = 4 under SUBTRACTIVE_DITHER_1 with seed 1: its rows have no noise, so that "
                "every tile falls back to GZIP_COMPRESSED_DATA, lossless",
                "fpack -C -r -q1 4, tests/data/scripts/make_fpack.py", compressed(4010, numeric_type::float32));
    fpack_image("B9", "fpack/b9-f32-gzip-lossless.fits",
                "A9 compressed losslessly with GZIP_1, with CFITSIO's ZQUANTIZ = 'NONE'",
                "fpack -C -g -q 0, tests/data/scripts/make_fpack.py", compressed(3976, numeric_type::float32));
    // ZQUANTIZ = 'NONE', a convention outside the standard, which verify() reports.
    samples.back().report_departures = {errc::invalid_compression};
    sample_unit b10 = compressed(1672, numeric_type::float32);
    b10.tolerance = 0.5e-4;
    for (std::uint64_t y = 0; y < height; ++y) {
        b10.specials.push_back({.index = y * width, .value = 0.0});
    }
    b10.specials.push_back({.index = (7 * width) + 5, .value = std::numeric_limits<double>::quiet_NaN()});
    fpack_image(
        "B10", "fpack/b10-f32-rice-dither2.fits",
        "the pattern of A9 with zeros in its first column and a NaN at (5, 7), quantized at the spacing 0.0001 "
        "under SUBTRACTIVE_DITHER_2 with seed 1 and compressed with RICE_1: the zeros exact, ZBLANK -2147483647",
        "fpack -C -r -qz1 -0.0001 of an input of astropy, tests/data/scripts/make_fpack.py", std::move(b10));
    sample_unit b11 = compressed(752, numeric_type::float32);
    b11.tolerance = 0.5e-4;
    fpack_image("B11", "fpack/b11-f32-hcomp-q4.fits",
                "A9 quantized at the spacing 0.0001 under SUBTRACTIVE_DITHER_1 with seed 1 and compressed with "
                "HCOMPRESS_1 at scale 0",
                "fpack -C -h -q1 -0.0001, tests/data/scripts/make_fpack.py", std::move(b11));
    fpack_image("B12", "fpack/b12-u8-rice.fits", "A1 compressed with RICE_1, BYTEPIX 1",
                "fpack -C -r, tests/data/scripts/make_fpack.py", compressed(410, numeric_type::uint8));
    fpack_image("B13", "fpack/b13-i32-rice.fits", "A5 compressed with RICE_1, BYTEPIX 4",
                "fpack -C -r, tests/data/scripts/make_fpack.py", compressed(2967, numeric_type::int32));

    sample b14;
    b14.id = "B14";
    b14.file = "fpack/b14-i64.fits";
    b14.content = "A7 compressed with GZIP_2 in tiles of one row: fpack 1.7.0 refuses 64-bit images (\"Bad image "
                  "datatype\"), so that astropy writes it";
    b14.producer = astropy;
    b14.procedure = "astropy's CompImageHDU with compression_type GZIP_2, tests/data/scripts/make_fpack.py";
    b14.units.push_back(unit_of(hdu_kind::primary, 0));
    b14.units.push_back(compressed(6296, numeric_type::int64));
    add(std::move(b14));

    sample b15;
    b15.id = "B15";
    b15.file = "fpack/b15-mef.fits.fz";
    b15.content =
        "A13 compressed with RICE_1, each image extension a compressed image of its EXTNAME, the empty primary "
        "HDU kept; the floating-point image at q = 4 falls back to GZIP_COMPRESSED_DATA";
    b15.producer = fpack;
    b15.procedure = "fpack -C, tests/data/scripts/make_fpack.py";
    b15.units.push_back(unit_of(hdu_kind::primary, 0));
    b15.units.push_back(compressed(1219, numeric_type::uint16, "SCI"));
    b15.units.push_back(compressed(4010, numeric_type::float32, "SCI"));
    b15.units.push_back(compressed(98, numeric_type::uint8, "MASK", {11, 7}, image_content::mask));
    add(std::move(b15));

    sample b16;
    b16.id = "B16";
    b16.file = "fpack/b16-table.fits.fz";
    b16.content = "a table of 100 rows of the column types of D2 compressed as a tiled table in one tile, its "
                  "variable-length columns included; fpack leaves a table of one block uncompressed, as D2 is, and "
                  "compresses the complex columns with GZIP_2 without shuffling them, which funpack does not read";
    b16.producer = fpack;
    b16.procedure = "fpack -C -table of an input of astropy, tests/data/scripts/make_fpack.py; its cells those that "
                    "dump_cells.py reads of that input";
    b16.units.push_back(unit_of(hdu_kind::primary, 0));
    b16.units.push_back(unit_of(hdu_kind::compressed_table, std::uint64_t{240} + 5489, "CATALOG"));
    add(std::move(b16));

    sample c3;
    c3.id = "C3";
    c3.file = "fpack/c3-rice-checksum.fits";
    c3.content = "C1 compressed with RICE_1, with the CHECKSUM and DATASUM of fpack in both units, and ZHECKSUM and "
                 "ZDATASUM, those of C1";
    c3.producer = fpack;
    c3.procedure = "fpack, with its checksums, tests/data/scripts/make_fpack.py";
    c3.set_by_hand = "nothing; the comments of the checksums hold the time of the run of fpack";
    c3.units.push_back(checked(unit_of(hdu_kind::primary, 0)));
    c3.units.push_back(checked(compressed(1219, numeric_type::uint16)));
    add(std::move(c3));

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

std::string sample_content(const sample& unit)
{
    std::string bytes = read_file(path_of(sample_path(unit)));
    if (bytes.size() < 2 || bytes[0] != static_cast<char>(0x1F) || bytes[1] != static_cast<char>(0x8B)) {
        return bytes;
    }
    const memory_source compressed(std::as_bytes(std::span(bytes)));
    const gzip_source content(compressed);
    std::string inflated(content.bytes().size(), char{});
    std::ranges::transform(content.bytes(), inflated.begin(), [](std::byte value) { return static_cast<char>(value); });
    return inflated;
}

void PrintTo(const sample& unit, std::ostream* output)
{
    *output << unit.id;
}

} // namespace openfits::test
