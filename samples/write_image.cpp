// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Writing an image with keywords. This program makes a frame of unsigned 16-bit values, a sky with a few stars, and
// writes it as the primary HDU of a FITS file with keywords of its own: the target, the exposure with its unit, the
// time of the observation and a HISTORY record. The library writes the structural keywords itself, BZERO = 32768 among
// them, which stores unsigned 16-bit values (standard §5.2.5). A second unit holds the same frame converted to
// floating point and scaled back into 16-bit integers, as programs that keep physical values do. A third holds the
// frame compressed with RICE_1 in tiles of 16 rows (standard §10.1): a binary table that a reader presents as the
// image, its tiles decompressed as they are read. A fourth holds the background of the sky alone, computed row by row
// as the save writes it, which takes the memory of a chunk however large the frame. Every unit gets DATASUM and
// CHECKSUM (standard §4.4.2.7), which let a reader verify it. The file is replaced only once it is complete. The
// program then reads the file back, verifies the checksums of each unit, and prints what it holds; the reads of the
// whole images verify DATASUM as well.
//
// Usage: write_image <path>

#include <openfits/openfits.h>

#include "arguments.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t width = 64;
constexpr std::size_t height = 48;

// A background of about 1000 with a gradient, and three stars.
std::vector<std::uint16_t> frame()
{
    std::vector<std::uint16_t> values(width * height);
    // The position and the peak of each star.
    constexpr std::array<std::array<double, 3>, 3> stars{
        {{12.0, 10.0, 30000.0}, {40.5, 25.0, 52000.0}, {55.0, 40.0, 12000.0}}};
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            double value = 1000.0 + (2.0 * static_cast<double>(x)) + static_cast<double>(y);
            for (const auto& star : stars) {
                const double dx = static_cast<double>(x) - star[0];
                const double dy = static_cast<double>(y) - star[1];
                value += star[2] * std::exp(-((dx * dx) + (dy * dy)) / 4.0);
            }
            values[(y * width) + x] = static_cast<std::uint16_t>(std::lround(std::min(value, 65535.0)));
        }
    }
    return values;
}

std::string_view state_name(openfits::checksum_state state)
{
    switch (state) {
    case openfits::checksum_state::valid:
        return "valid";
    case openfits::checksum_state::invalid:
        return "invalid";
    case openfits::checksum_state::absent:
        break;
    }
    return "absent";
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const std::vector<std::string> arguments = sample::arguments(argc, argv);
        if (arguments.size() != 1) {
            std::cerr << "usage: write_image <path>\n";
            return 2;
        }
        const std::string& path = arguments[0];

        // The header of the image: the keywords of the application. A unit in brackets at the start of a comment is
        // the unit of the value (standard §4.3.2).
        const std::vector<std::uint16_t> values = frame();
        openfits::image_info image;
        image.geometry.axes = {width, height};
        image.header.set("OBJECT", "NGC 7000", "the target");
        image.header.set("EXPTIME", 300.0, "[s] exposure time");
        // The library reads no clock: the time is the application's, here a fixed one.
        const auto observed = std::chrono::sys_days{std::chrono::year{2026} / 10 / 31} + std::chrono::hours{21};
        image.header.set("DATE-OBS", openfits::format_date(observed), "start of the exposure, UTC");
        image.header.add_history("written by the write_image sample of OpenFITS");

        // The writer borrows the values until it saves them; it never modifies them.
        openfits::writer output;
        (void)output.add_image(image, std::span<const std::uint16_t>(values));

        // The same frame in floating point, stored as 16-bit integers with a scaling of the application: each stored
        // value is (value - BZERO) / BSCALE rounded, and NaN is stored as BLANK.
        std::vector<float> physical(values.size());
        for (std::size_t i = 0; i < values.size(); ++i) {
            physical[i] = static_cast<float>(values[i]) * 0.01F;
        }
        physical[0] = std::nanf("");
        openfits::image_info scaled;
        scaled.geometry.axes = {width, height};
        scaled.bitpix = openfits::bitpix::int16;
        scaled.scaling = {.scale = 0.01, .zero = 327.68};
        scaled.blank = -32768;
        scaled.header.set("EXTNAME", "PHYSICAL");
        scaled.header.set("BUNIT", "adu/100");
        (void)output.add_image(scaled, std::span<const float>(physical));
        // The frame compressed, each tile of 16 rows alone, lossless for integers; in parallel in a build with oneTBB.
        openfits::image_info packed = image;
        packed.header.set("EXTNAME", "RICE");
        (void)output.add_image(
            packed, std::span<const std::uint16_t>(values),
            {.compression = openfits::compression_options{.type = openfits::compression::rice, .tile = {width, 16}}});
        // A frame whose values the save asks for chunk after chunk, whole rows of them, as it writes them: computed
        // then, they are never held whole.
        openfits::image_info sky;
        sky.geometry.axes = {width, height};
        sky.header.set("EXTNAME", "SKY");
        (void)output.add_image<std::uint16_t>(sky, [](std::uint64_t first, std::span<std::uint16_t> rows) {
            for (std::size_t i = 0; i < rows.size(); ++i) {
                const std::uint64_t x = (first + i) % width;
                const std::uint64_t y = (first + i) / width;
                rows[i] = static_cast<std::uint16_t>(1000 + (2 * x) + y);
            }
        });
        // The checksums, with the time they were computed as their comment, again the application's.
        output.save(path,
                    {.checksums = true, .checksum_time = openfits::to_date_time(observed + std::chrono::hours{2})});

        // Read back: the keywords and the checksums of each unit, and some values.
        const openfits::reader file(path);
        bool verified = true;
        for (const openfits::hdu_info& unit : file.hdus()) {
            std::cout << "HDU " << unit.index() << ":\n";
            for (const openfits::card& value : unit.header()) {
                const std::string_view text = value.text();
                std::cout << "  | " << text.substr(0, text.find_last_not_of(' ') + 1) << '\n';
            }
            const openfits::checksum_report sums = file.verify(unit.index());
            std::cout << "  DATASUM " << state_name(sums.datasum) << ", CHECKSUM " << state_name(sums.checksum) << '\n';
            verified = verified && sums.datasum == openfits::checksum_state::valid &&
                       sums.checksum == openfits::checksum_state::valid;
        }
        const std::vector<std::uint16_t> read = file.read_pixels<std::uint16_t>(0);
        std::cout << "values: " << read[0] << ' ' << read[(10 * width) + 12] << " (a star)\n";
        const std::vector<double> back = file.read_pixels<double>(file.hdu("PHYSICAL").index());
        std::cout << "physical values: " << back[0] << " (BLANK) " << back[(10 * width) + 12] << '\n';
        const std::size_t rice = file.hdu("RICE").index();
        const std::vector<std::uint16_t> unpacked = file.read_pixels<std::uint16_t>(rice);
        std::cout << "compressed: " << file.hdu(rice).data_size().value_or(0) << " bytes of table for "
                  << values.size() * 2 << " of values, " << (unpacked == values ? "read back as written" : "changed")
                  << '\n';
        const std::vector<std::uint16_t> background = file.read_pixels<std::uint16_t>(file.hdu("SKY").index());
        std::cout << "streamed: " << background.front() << " to " << background.back() << '\n';
        const std::size_t last = background.back();
        const bool computed = last == 1000 + (2 * (width - 1)) + (height - 1);
        return read == values && unpacked == values && computed && verified ? 0 : 1;
    } catch (const std::exception& failure) {
        (void)std::fputs("write_image: ", stderr);
        (void)std::fputs(failure.what(), stderr);
        (void)std::fputs("\n", stderr);
        return 1;
    }
}
