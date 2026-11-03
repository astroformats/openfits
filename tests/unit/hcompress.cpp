// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "compress/hcompress.h"

#include <openfits/error.h>

#include "support/sequence.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <vector>

namespace {

using openfits::errc;
using openfits::integrity_error;
using openfits::usage_error;
using openfits::detail::hcompress_decode;
using openfits::detail::hcompress_encode;
using openfits::detail::hcompress_max_magnitude;
using openfits::test::splitmix64;
using openfits::test::throws;

template <typename T>
std::vector<std::byte> encode(const std::vector<T>& values, std::size_t rows, std::size_t cols, std::int64_t scale = 0)
{
    std::vector<std::byte> stream;
    hcompress_encode(std::span<const T>(values), rows, cols, scale, stream);
    return stream;
}

template <typename T>
std::vector<T> decode(const std::vector<std::byte>& stream, std::size_t rows, std::size_t cols, bool smooth = false)
{
    // A copy of exactly the bytes of the stream, so that a read beyond them leaves its allocation.
    const std::vector<std::byte> exact(stream.begin(), stream.end());
    std::vector<T> values(rows * cols);
    hcompress_decode(exact, rows, cols, smooth, std::span<T>(values));
    return values;
}

testing::AssertionResult corrupt(const std::vector<std::byte>& stream, std::size_t rows, std::size_t cols,
                                 bool smooth = false)
{
    return throws<integrity_error>(errc::corrupt_compressed_data,
                                   [&] { (void)decode<std::int64_t>(stream, rows, cols, smooth); });
}

std::vector<std::byte> bytes_of(std::initializer_list<int> values)
{
    std::vector<std::byte> bytes;
    for (const int value : values) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

template <typename T, typename U> std::vector<T> converted(const std::vector<U>& values)
{
    std::vector<T> result(values.size());
    std::ranges::transform(values, result.begin(), [](U value) { return static_cast<T>(value); });
    return result;
}

// A copy of stream with the byte at index set to value.
std::vector<std::byte> patched(std::vector<std::byte> stream, std::size_t index, int value)
{
    stream[index] = static_cast<std::byte>(value);
    return stream;
}

// A copy of stream with the first coefficient, bytes 14 to 21 of the header, set to sum.
std::vector<std::byte> with_sum(std::vector<std::byte> stream, std::uint64_t sum)
{
    for (std::size_t i = 0; i < 8; ++i) {
        stream[14 + i] = static_cast<std::byte>((sum >> (8 * (7 - i))) & 0xFFU);
    }
    return stream;
}

// A 16-bit image of 4 rows of 5 values, one tile (ZTILE1 = 5, ZTILE2 = 4), and the streams that fpack 1.7.0 of
// CFITSIO 4.7.0 writes for it: losslessly, and with -s -4, at the scale 4.
std::vector<std::int64_t> tile_4x5()
{
    return {10, 12, 11, 9, 8, 13, 15, 14, 10, 7, 12, 11, 10, 9, 9, 8, 9, 30, 12, 10};
}

std::vector<std::byte> lossless_4x5()
{
    return bytes_of({0xdd, 0x99, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb0, 0x05, 0x05, 0x05, 0xf6, 0x81, 0x68,
                     0x08, 0xbd, 0x1f, 0xfb, 0xc7, 0xfd, 0xfe, 0xf5, 0xff, 0x7b, 0x1e, 0xc0, 0x70, 0x05,
                     0x57, 0xfd, 0xe3, 0xfe, 0xff, 0xbf, 0x5e, 0x00, 0x91, 0x38, 0x80});
}

std::vector<std::byte> scale_4_4x5()
{
    return bytes_of({0xdd, 0x99, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x04, 0x00,
                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2c, 0x03, 0x03, 0x03, 0xf6, 0x81, 0x68, 0x08, 0xbc,
                     0x7f, 0xdf, 0xef, 0x63, 0xd8, 0x0e, 0x0f, 0x1f, 0xf7, 0xfc, 0x00, 0x91, 0x28});
}

// The stream of 6 rows of 7 values of 16-bit noise that fpack 1.7.0 writes with -h: most planes in direct coding.
std::vector<std::int64_t> noise_6x7()
{
    return {145,  108, -171, -43, 133,  192,  -281, 199, -299, -61, -36,  -190, 198,  16,
            -171, 267, 231,  -10, -200, 22,   -208, 99,  -88,  1,   141,  -168, -251, -155,
            197,  -44, -106, 287, 218,  -241, 1,    23,  40,   199, -216, 231,  96,   102};
}

std::vector<std::byte> lossless_6x7()
{
    return bytes_of({0xdd, 0x99, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90, 0x0a, 0x0a, 0x0a, 0xfa, 0x50, 0x08, 0x91, 0x9f,
                     0x6b, 0x40, 0x5a, 0x18, 0x05, 0xf9, 0x80, 0xcd, 0x80, 0x03, 0x11, 0x80, 0x2d, 0x89, 0xe1,
                     0x7f, 0xdf, 0xc6, 0xfd, 0x30, 0x6a, 0x80, 0x03, 0x88, 0x8f, 0xa7, 0x01, 0xc0, 0x91, 0xf6,
                     0x7f, 0x03, 0x24, 0x0f, 0xeb, 0x53, 0xff, 0x7a, 0x5f, 0xe6, 0xdf, 0xed, 0x22, 0x1e, 0xf9,
                     0x9f, 0x7d, 0xb0, 0x7e, 0x88, 0x00, 0x98, 0xc0, 0x68, 0x48, 0x0e, 0xfc, 0xcf, 0xfb, 0xe9,
                     0x10, 0x9a, 0xc8, 0xf6, 0xff, 0xcd, 0xe0, 0x39, 0x20, 0x7d, 0x98, 0x09, 0x84, 0x0f, 0xe3,
                     0xf7, 0xd3, 0x5f, 0x75, 0x80, 0xf0, 0xd2, 0x7a, 0xdd, 0x71, 0x00});
}

// A 16-bit ramp of 8 rows of 9 values that fpack 1.7.0 writes with -h -s -6, at the scale 6, large enough for every
// pass of the smoothing.
std::vector<std::int64_t> ramp_8x9()
{
    return {1000, 1023, 1020, 1047, 1067, 1081, 1082, 1114, 1128, 1020, 1030, 1051, 1070, 1087, 1096, 1118, 1133, 1136,
            1043, 1059, 1082, 1106, 1116, 1127, 1127, 1156, 1169, 1061, 1076, 1094, 1133, 1146, 1154, 1154, 1158, 1172,
            1090, 1109, 1126, 1149, 1178, 1177, 1187, 1195, 1193, 1097, 1127, 1146, 1159, 1201, 1196, 1200, 1217, 1226,
            1111, 1132, 1147, 1179, 1199, 1197, 1224, 1220, 1234, 1142, 1152, 1165, 1197, 1199, 1225, 1241, 1249, 1266};
}

std::vector<std::byte> scale_6_8x9()
{
    return bytes_of({0xdd, 0x99, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x06, 0x00,
                     0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x30, 0x08, 0x04, 0x03, 0xf6, 0xd7, 0xbb, 0x71, 0xee,
                     0x9e, 0xc0, 0xb4, 0x1e, 0x80, 0x07, 0x69, 0x28, 0x00, 0x02, 0x91, 0x28, 0x00, 0xe9, 0xa8,
                     0x80, 0x98, 0x2d, 0xa0, 0x15, 0x24, 0x02, 0xa5, 0x98, 0x24, 0xd4, 0x00, 0xc8, 0xe4, 0x11,
                     0x80, 0x86, 0x82, 0xe4, 0x37, 0x80, 0x0e, 0xa0, 0x72, 0x83, 0x56, 0x9a, 0xab, 0xe4, 0x80,
                     0x94, 0xe1, 0x04, 0xc0, 0xe0, 0x00, 0x00, 0x04, 0x80, 0x01, 0xe0, 0x40, 0xc0, 0x80});
}

// A 32-bit image of 5 rows of 6 values near -2 x 10^9 that fpack 1.7.0 writes with -h, in the 64-bit coder of CFITSIO:
// its first coefficient needs 36 bits.
std::vector<std::int64_t> noise_5x6()
{
    return {-1999953932, -1999976481, -1999980894, -2000047238, -2000042979, -2000040046, -1999983380, -2000029023,
            -2000005932, -1999955920, -2000027617, -2000003684, -1999957325, -2000035732, -2000039629, -2000042022,
            -2000029849, -1999996887, -1999977133, -2000028506, -2000008961, -2000006362, -1999952795, -2000038046,
            -2000048540, -2000005291, -1999988339, -1999982161, -2000006170, -1999960953};
}

std::vector<std::byte> lossless_5x6()
{
    return bytes_of({0xdd, 0x99, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff,
                     0xff, 0xf8, 0x8c, 0xa6, 0x44, 0x60, 0x12, 0x11, 0x11, 0xfb, 0x20, 0x0a, 0x30, 0x3e, 0xc9, 0xfe,
                     0xb5, 0x20, 0xc1, 0x90, 0x0d, 0x18, 0x00, 0xd5, 0x80, 0x07, 0x10, 0x00, 0xc5, 0x10, 0x06, 0x49,
                     0x1f, 0xd6, 0x87, 0xf5, 0xa3, 0xf4, 0x80, 0x2c, 0x64, 0x7f, 0xec, 0xa0, 0x18, 0x88, 0xff, 0xbf,
                     0xef, 0xeb, 0x75, 0xf6, 0x48, 0x15, 0x44, 0x03, 0x42, 0x47, 0xd5, 0xeb, 0xee, 0xbf, 0xfb, 0x2b,
                     0xdf, 0xff, 0xd6, 0xd0, 0x3c, 0x40, 0x07, 0xd6, 0x40, 0x54, 0x40, 0x7d, 0xb3, 0xee, 0x7e, 0x96,
                     0xc0, 0x6a, 0x23, 0xfe, 0x0d, 0x0f, 0x54, 0x11, 0x1e, 0xcf, 0xb3, 0xea, 0xbd, 0x10, 0x58, 0xf7,
                     0x1e, 0xd7, 0xbb, 0x06, 0x20, 0x12, 0x05, 0xa0, 0xa8, 0xf7, 0x5f, 0xf0, 0x21, 0x02, 0x10, 0x75,
                     0x7b, 0x21, 0x30, 0x14, 0x40, 0x84, 0x1b, 0x00, 0x64, 0x0e, 0x5e, 0x8f, 0xb7, 0xee, 0x80, 0xa0,
                     0x30, 0xbd, 0xd7, 0xa2, 0x00, 0x70, 0xb4, 0x49, 0x08});
}

// A 32-bit ramp of 6 rows of 6 values near 10^9 that fpack 1.7.0 writes with -h -s -5000, smoothed in 64 bits.
std::vector<std::int64_t> ramp_6x6()
{
    return {1000018313, 999288495,  998588370,  997892494,  997196695,  996504547,  1000983125, 1000300026, 999600721,
            998892708,  998195920,  997482250,  1002015712, 1001315950, 1000595917, 999896207,  999210626,  998509063,
            1003015251, 1002302396, 1001592147, 1000893717, 1000214597, 999485015,  1003980625, 1003292083, 1002619033,
            1001918461, 1001203622, 1000481088, 1005011473, 1004307736, 1003594160, 1002918221, 1002196902, 1001489025};
}

std::vector<std::byte> scale_5000_6x6()
{
    return bytes_of({0xdd, 0x99, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x13, 0x88, 0x00,
                     0x00, 0x00, 0x00, 0x00, 0x30, 0xdf, 0x3e, 0x0d, 0x09, 0x04, 0xf6, 0x7d, 0xa0, 0x4a, 0xc0,
                     0xfa, 0xb3, 0xed, 0x7b, 0x3f, 0xab, 0xaf, 0xb7, 0xaf, 0xb2, 0x41, 0xe1, 0x00, 0x08, 0xa2,
                     0x01, 0xc2, 0x23, 0xed, 0x90, 0xfa, 0xc8, 0xff, 0xbf, 0xef, 0xf8, 0x3e, 0xb2, 0x03, 0xe8,
                     0x23, 0xf6, 0xd9, 0x0a, 0x2c, 0x80, 0xe8, 0x40, 0x0f, 0xac, 0x80, 0xfa, 0xc8, 0xff, 0xbf,
                     0xef, 0xeb, 0x68, 0x2e, 0xb0, 0x3e, 0xcc, 0x06, 0xc2, 0x00, 0x7c, 0x60, 0x7b, 0x7f, 0xa5,
                     0x40, 0xc5, 0x10, 0x14, 0x19, 0x00, 0xfb, 0xe7, 0x86, 0x31, 0x00});
}

// Expects both encoders to write stream for values, and both decoders to restore restored from it, and smoothed with
// smoothing.
void expect_cfitsio(std::size_t rows, std::size_t cols, std::int64_t scale, const std::vector<std::int64_t>& values,
                    const std::vector<std::byte>& stream, const std::vector<std::int64_t>& restored,
                    const std::vector<std::int64_t>& smoothed)
{
    EXPECT_EQ(encode(converted<std::int32_t>(values), rows, cols, scale), stream);
    EXPECT_EQ(encode(values, rows, cols, scale), stream);
    EXPECT_EQ(decode<std::int32_t>(stream, rows, cols), converted<std::int32_t>(restored));
    EXPECT_EQ(decode<std::int64_t>(stream, rows, cols), restored);
    EXPECT_EQ(decode<std::int32_t>(stream, rows, cols, true), converted<std::int32_t>(smoothed));
    EXPECT_EQ(decode<std::int64_t>(stream, rows, cols, true), smoothed);
}

TEST(hcompress, writes_and_reads_the_streams_of_cfitsio)
{
    // funpack restores the lossy streams with ZVAL2 = 0 and 1 as given; the lossless ones are never smoothed.
    expect_cfitsio(4, 5, 0, tile_4x5(), lossless_4x5(), tile_4x5(), tile_4x5());
    expect_cfitsio(4, 5, 4, tile_4x5(), scale_4_4x5(),
                   {10, 12, 11, 9, 8, 12, 14, 13, 11, 8, 11, 11, 10, 8, 10, 9, 9, 30, 12, 10},
                   {10, 12, 10, 9, 8, 12, 14, 12, 11, 8, 11, 11, 10, 8, 10, 9, 9, 30, 12, 10});
    expect_cfitsio(6, 7, 0, noise_6x7(), lossless_6x7(), noise_6x7(), noise_6x7());
    expect_cfitsio(5, 6, 0, noise_5x6(), lossless_5x6(), noise_5x6(), noise_5x6());
    expect_cfitsio(
        8, 9, 6, ramp_8x9(), scale_6_8x9(),
        {997,  1021, 1020, 1047, 1067, 1082, 1082, 1115, 1127, 1018, 1030, 1050, 1071, 1088, 1097, 1118, 1133, 1136,
         1042, 1057, 1080, 1107, 1116, 1125, 1126, 1156, 1169, 1060, 1075, 1095, 1134, 1146, 1155, 1153, 1159, 1172,
         1090, 1108, 1124, 1148, 1177, 1177, 1186, 1195, 1192, 1096, 1126, 1145, 1157, 1201, 1195, 1201, 1216, 1225,
         1110, 1131, 1147, 1180, 1199, 1196, 1224, 1221, 1233, 1143, 1152, 1165, 1198, 1199, 1226, 1242, 1251, 1266},
        {997,  1021, 1020, 1046, 1065, 1082, 1082, 1114, 1127, 1018, 1030, 1050, 1070, 1086, 1097, 1118, 1132, 1136,
         1041, 1056, 1079, 1105, 1115, 1126, 1126, 1155, 1168, 1061, 1076, 1096, 1134, 1144, 1155, 1155, 1160, 1173,
         1089, 1107, 1124, 1148, 1176, 1177, 1186, 1194, 1193, 1097, 1127, 1145, 1160, 1199, 1194, 1203, 1216, 1225,
         1110, 1131, 1148, 1180, 1197, 1196, 1223, 1222, 1233, 1143, 1152, 1166, 1198, 1197, 1226, 1241, 1252, 1266});
    expect_cfitsio(
        6, 6, 5000, ramp_6x6(), scale_5000_6x6(),
        {1000019062, 999289062,  998589062,  997891562,  997197187,  996504687,  1000984062, 1000299062, 999601562,
         998894062,  998194687,  997482187,  1002016562, 1001316562, 1000595312, 999895312,  999209687,  998509687,
         1003016562, 1002301562, 1001592812, 1000892812, 1000214687, 999484687,  1003980312, 1003292812, 1002619062,
         1001919062, 1001204062, 1000481562, 1005010312, 1004307812, 1003594062, 1002919062, 1002196562, 1001489062},
        {1000019062, 999289062,  998588437,  997892187,  997197187,  996504687,  1000984062, 1000299062, 999600937,
         998894687,  998194687,  997482187,  1002015937, 1001315937, 1000595283, 999894092,  999209062,  998509062,
         1003017187, 1002302187, 1001594091, 1000892783, 1000215312, 999485312,  1003980312, 1003292812, 1002619687,
         1001918437, 1001204062, 1000481562, 1005010312, 1004307812, 1003594687, 1002918437, 1002196562, 1001489062});
}

TEST(hcompress, codes_the_planes_of_a_quadrant_without_cells_directly)
{
    // One row of 0 and 1: the sum 4 and the difference 2 along the row, whose two planes are the quadtrees 1111 011
    // and 1111 111110. The two quadrants below the row have no cells but the planes of that difference: each is the
    // nybble 0 alone. The end code and the sign of the difference follow.
    const std::vector<std::byte> header =
        bytes_of({0xdd, 0x99, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x02, 0x00});
    const auto stream_of = [&](std::initializer_list<int> bits) {
        std::vector<std::byte> stream = header;
        const std::vector<std::byte> tail = bytes_of(bits);
        stream.insert(stream.end(), tail.begin(), tail.end());
        return stream;
    };
    const std::vector<std::byte> direct = stream_of({0xf7, 0xff, 0x00, 0x00, 0x00});
    EXPECT_EQ(encode<std::int32_t>({0, 1}, 1, 2), direct);
    EXPECT_EQ(decode<std::int32_t>(direct, 1, 2), (std::vector<std::int32_t>{0, 1}));
    // The quadtrees of nothing that CFITSIO's coder would write there read the same; one that marks a cell is corrupt.
    EXPECT_EQ(decode<std::int32_t>(stream_of({0xf7, 0xff, 0x7f, 0xdf, 0xf0, 0x00, 0x00}), 1, 2),
              (std::vector<std::int32_t>{0, 1}));
    EXPECT_TRUE(corrupt(stream_of({0xf7, 0xff, 0x7b, 0x00, 0x00, 0x00}), 1, 2));
}

// Values of every kind: noise of 16 bits, a ramp with noise, a constant, noise over 32 bits, and the two extremes of
// 32 bits in turn.
std::vector<std::int32_t> sample_values(std::size_t count, std::uint64_t kind, splitmix64& sequence)
{
    constexpr std::int32_t lowest = std::numeric_limits<std::int32_t>::min();
    constexpr std::int32_t highest = std::numeric_limits<std::int32_t>::max();
    const auto constant = static_cast<std::int32_t>(sequence.next());
    std::vector<std::int32_t> values(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t noise = sequence.next();
        if (kind == 0) {
            values[i] = static_cast<std::int32_t>(noise % 65536) - 32768;
        } else if (kind == 1) {
            values[i] = 5000 + (3 * static_cast<std::int32_t>(i % 97)) + static_cast<std::int32_t>(noise % 21);
        } else if (kind == 2) {
            values[i] = constant;
        } else if (kind == 3) {
            values[i] = static_cast<std::int32_t>(noise);
        } else {
            values[i] = i % 3 == 0 ? lowest : highest;
        }
    }
    return values;
}

// Values of magnitude up to largest: the two extremes in turn, noise, and each extreme alone.
std::vector<std::int64_t> extreme_values(std::size_t count, std::uint64_t largest, std::uint64_t kind,
                                         splitmix64& sequence)
{
    const auto high = static_cast<std::int64_t>(largest);
    std::vector<std::int64_t> values(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (kind == 0) {
            values[i] = i % 2 == 0 ? high : -high;
        } else if (kind == 1) {
            values[i] = static_cast<std::int64_t>(sequence.next() % ((2 * largest) + 1)) - high;
        } else {
            values[i] = kind == 2 ? high : -high;
        }
    }
    return values;
}

struct shape
{
    std::size_t rows = 0;
    std::size_t cols = 0;
};

// Tiles of one value, of one row or one column, square, odd, tall and wide.
std::vector<shape> shapes()
{
    return {{.rows = 1, .cols = 1},   {.rows = 1, .cols = 2},   {.rows = 2, .cols = 1},   {.rows = 1, .cols = 7},
            {.rows = 9, .cols = 1},   {.rows = 1, .cols = 64},  {.rows = 2, .cols = 2},   {.rows = 3, .cols = 3},
            {.rows = 4, .cols = 4},   {.rows = 5, .cols = 7},   {.rows = 7, .cols = 5},   {.rows = 16, .cols = 16},
            {.rows = 17, .cols = 33}, {.rows = 33, .cols = 17}, {.rows = 2, .cols = 100}, {.rows = 100, .cols = 2},
            {.rows = 64, .cols = 3},  {.rows = 3, .cols = 65},  {.rows = 31, .cols = 31}};
}

TEST(hcompress, round_trips_tiles_of_every_shape_losslessly)
{
    splitmix64 sequence(1);
    for (const shape& tile : shapes()) {
        for (std::uint64_t kind = 0; kind < 5; ++kind) {
            const std::vector<std::int32_t> values = sample_values(tile.rows * tile.cols, kind, sequence);
            const std::vector<std::byte> stream = encode(values, tile.rows, tile.cols);
            ASSERT_EQ(encode(converted<std::int64_t>(values), tile.rows, tile.cols), stream);
            ASSERT_EQ(decode<std::int32_t>(stream, tile.rows, tile.cols), values) << tile.rows << " x " << tile.cols;
            ASSERT_EQ(decode<std::int64_t>(stream, tile.rows, tile.cols, true), converted<std::int64_t>(values));
            // The scale 1 is lossless too.
            ASSERT_EQ(decode<std::int32_t>(encode(values, tile.rows, tile.cols, 1), tile.rows, tile.cols, true),
                      values);
        }
    }
}

TEST(hcompress, round_trips_64_bit_values_up_to_the_largest_magnitude)
{
    // Above 2^31 for any tile of up to 2^20 values along each axis, lossless or not.
    EXPECT_GT(hcompress_max_magnitude(std::size_t{1} << 20, std::size_t{1} << 20, 0), std::uint64_t{1} << 31);
    EXPECT_GT(hcompress_max_magnitude(std::size_t{1} << 20, 3, 1000), std::uint64_t{1} << 31);
    splitmix64 sequence(2);
    for (const shape& tile : shapes()) {
        const std::uint64_t largest = hcompress_max_magnitude(tile.rows, tile.cols, 0);
        for (std::uint64_t kind = 0; kind < 4; ++kind) {
            const std::vector<std::int64_t> values = extreme_values(tile.rows * tile.cols, largest, kind, sequence);
            ASSERT_EQ(decode<std::int64_t>(encode(values, tile.rows, tile.cols), tile.rows, tile.cols), values)
                << tile.rows << " x " << tile.cols << " " << kind;
        }
    }
}

// The largest error of a lossy value: each coefficient is off by at most half the scale, a value by at most 3/4 of
// the scale and the low bits that each level restores, under 2 a level; the smoothing moves each coefficient by at most
// half the scale more.
std::int64_t lossy_bound(const shape& tile, std::int64_t scale, bool smooth)
{
    std::int64_t levels = 0;
    while ((std::size_t{1} << levels) < std::max(tile.rows, tile.cols)) {
        ++levels;
    }
    return (smooth ? (3 * scale) / 2 : (3 * scale) / 4) + (2 * (levels + 1));
}

template <typename T>
void expect_close(const std::vector<std::byte>& stream, const shape& tile, std::int64_t scale,
                  const std::vector<T>& values)
{
    for (const bool smooth : {false, true}) {
        const std::vector<T> restored = decode<T>(stream, tile.rows, tile.cols, smooth);
        std::int64_t worst = 0;
        for (std::size_t i = 0; i < values.size(); ++i) {
            worst = std::max(worst, std::max<std::int64_t>(restored[i] - values[i], values[i] - restored[i]));
        }
        EXPECT_LE(worst, lossy_bound(tile, scale, smooth))
            << tile.rows << " x " << tile.cols << " at the scale " << scale << (smooth ? ", smoothed" : "");
    }
}

TEST(hcompress, stays_within_the_error_that_the_scale_allows)
{
    splitmix64 sequence(3);
    for (const shape& tile : shapes()) {
        for (const std::int64_t scale : {2, 3, 4, 7, 16, 100, 1000}) {
            // 16-bit noise and a ramp in 32 bits; noise over 32 bits, which the 64-bit decoder restores.
            for (std::uint64_t kind = 0; kind < 2; ++kind) {
                const std::vector<std::int32_t> values = sample_values(tile.rows * tile.cols, kind, sequence);
                const std::vector<std::byte> stream = encode(values, tile.rows, tile.cols, scale);
                ASSERT_EQ(encode(converted<std::int64_t>(values), tile.rows, tile.cols, scale), stream);
                expect_close(stream, tile, scale, values);
            }
            const std::vector<std::int64_t> wide =
                converted<std::int64_t>(sample_values(tile.rows * tile.cols, 3, sequence));
            expect_close(encode(wide, tile.rows, tile.cols, scale), tile, scale, wide);
            // The largest values that the encoder takes at the scale decode, smoothed or not.
            const std::uint64_t largest = hcompress_max_magnitude(tile.rows, tile.cols, scale);
            const std::vector<std::int64_t> extreme = extreme_values(tile.rows * tile.cols, largest, 1, sequence);
            expect_close(encode(extreme, tile.rows, tile.cols, scale), tile, scale, extreme);
        }
    }
}

TEST(hcompress, refuses_what_it_cannot_encode)
{
    const auto refused = [](const std::vector<std::int64_t>& values, std::size_t rows, std::size_t cols,
                            std::int64_t scale) {
        return throws<usage_error>(errc::invalid_argument, [&] { (void)encode(values, rows, cols, scale); });
    };
    const std::vector<std::int64_t> tile(20, 1);
    EXPECT_TRUE(refused(tile, 4, 4, 0));
    EXPECT_TRUE(refused(tile, 0, 20, 0));
    EXPECT_TRUE(refused(tile, 20, 0, 0));
    EXPECT_TRUE(refused({}, 0, 0, 0));
    EXPECT_TRUE(refused(tile, std::size_t{1} << 31, 1, 0));
    EXPECT_TRUE(refused(tile, 4, 5, -1));
    EXPECT_TRUE(refused(tile, 4, 5, std::int64_t{1} << 31));
    for (const std::int64_t scale : {0, 2}) {
        const auto largest = static_cast<std::int64_t>(hcompress_max_magnitude(4, 5, scale));
        std::vector<std::int64_t> values(20, -largest);
        EXPECT_NO_THROW((void)encode(values, 4, 5, scale));
        values[7] = largest + 1;
        EXPECT_TRUE(refused(values, 4, 5, scale));
        values[7] = -largest - 1;
        EXPECT_TRUE(refused(values, 4, 5, scale));
        values[7] = std::numeric_limits<std::int64_t>::min();
        EXPECT_TRUE(refused(values, 4, 5, scale));
    }
    std::vector<std::int32_t> output(19);
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] {
        hcompress_decode(lossless_4x5(), 4, 5, false, std::span<std::int32_t>(output));
    }));
}

TEST(hcompress, refuses_a_stream_that_ends_early)
{
    for (const std::vector<std::byte>& stream : {lossless_4x5(), lossless_6x7(), scale_6_8x9()}) {
        const auto rows = std::to_integer<std::size_t>(stream[5]);
        const auto cols = std::to_integer<std::size_t>(stream[9]);
        ASSERT_NO_THROW((void)decode<std::int64_t>(stream, rows, cols, true));
        for (std::size_t length = 0; length < stream.size(); ++length) {
            const std::vector<std::byte> part(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(length));
            EXPECT_TRUE(corrupt(part, rows, cols, true)) << length;
        }
    }
}

TEST(hcompress, ignores_the_bytes_after_the_signs)
{
    std::vector<std::byte> stream = lossless_4x5();
    stream.insert(stream.end(), 5, std::byte{0xFF});
    EXPECT_EQ(decode<std::int64_t>(stream, 4, 5), tile_4x5());
}

TEST(hcompress, refuses_a_stream_of_another_tile)
{
    EXPECT_TRUE(corrupt(lossless_4x5(), 5, 4));
    EXPECT_TRUE(corrupt(lossless_4x5(), 2, 10));
    EXPECT_TRUE(corrupt(lossless_4x5(), 1, 20));
    EXPECT_TRUE(corrupt(lossless_4x5(), 4, 4));
    EXPECT_TRUE(corrupt(lossless_4x5(), 4, 6));
}

TEST(hcompress, refuses_malformed_streams)
{
    // The bytes 0xDD 0x99 first.
    EXPECT_TRUE(corrupt(patched(lossless_4x5(), 0, 0xde), 4, 5));
    EXPECT_TRUE(corrupt(patched(lossless_4x5(), 1, 0x98), 4, 5));
    // 64 planes, beyond 64-bit coefficients.
    EXPECT_TRUE(corrupt(patched(lossless_4x5(), 22, 64), 4, 5));
    EXPECT_TRUE(corrupt(patched(lossless_4x5(), 24, 255), 4, 5));
    // A plane in the format 5, neither direct (0) nor a quadtree (15).
    EXPECT_TRUE(corrupt(patched(lossless_4x5(), 25, 0x56), 4, 5));
    // A tile of one value: no plane, then the end code 0, here 5.
    const std::vector<std::byte> single = encode<std::int64_t>({5}, 1, 1);
    ASSERT_EQ(single.size(), 26U);
    EXPECT_EQ(decode<std::int64_t>(single, 1, 1), (std::vector<std::int64_t>{5}));
    EXPECT_TRUE(corrupt(patched(single, 25, 0x50), 1, 1));
}

TEST(hcompress, refuses_coefficients_beyond_the_range_of_the_inverse_transform)
{
    // A first coefficient of 2^62, or of -2^63, is beyond the sums of the inverse in 64 bits.
    const std::vector<std::byte> single = encode<std::int64_t>({5}, 1, 1);
    EXPECT_TRUE(corrupt(with_sum(single, std::uint64_t{1} << 62), 1, 1));
    EXPECT_TRUE(corrupt(with_sum(single, std::uint64_t{1} << 63), 1, 1));
    EXPECT_EQ(decode<std::int64_t>(with_sum(single, std::uint64_t{1} << 59), 1, 1),
              (std::vector<std::int64_t>{std::int64_t{1} << 59}));
    // 2^55 at the scale 1000 overflows the scaling.
    const std::vector<std::byte> scaled = encode<std::int64_t>({5}, 1, 1, 1000);
    EXPECT_TRUE(corrupt(with_sum(scaled, std::uint64_t{1} << 55), 1, 1));
    // 2^57 once scaled inverts, but leaves the smoothing no room.
    const std::vector<std::byte> zeros =
        with_sum(encode(std::vector<std::int64_t>(16, 0), 4, 4, 2), std::uint64_t{1} << 56);
    EXPECT_NO_THROW((void)decode<std::int64_t>(zeros, 4, 4));
    EXPECT_TRUE(corrupt(zeros, 4, 4, true));
}

TEST(hcompress, refuses_values_beyond_32_bits_in_the_32_bit_decoder)
{
    std::vector<std::int64_t> values(16);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = (std::int64_t{1} << 40) + static_cast<std::int64_t>(i);
    }
    const std::vector<std::byte> stream = encode(values, 4, 4);
    EXPECT_EQ(decode<std::int64_t>(stream, 4, 4), values);
    EXPECT_TRUE(
        throws<integrity_error>(errc::corrupt_compressed_data, [&] { (void)decode<std::int32_t>(stream, 4, 4); }));
}

} // namespace
