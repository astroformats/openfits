// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Compressing the images of a file, as fpack does. This program reads every image of a file, compressed or not, and
// writes each compressed with the algorithm given (standard §10.1), its tiles compressed on every core in a build with
// oneTBB; its stored values are written as they are, under the scaling of its header, so that integer images stay
// lossless, and floating-point images are quantized with fpack's level of 4 unless GZIP keeps them or a level is
// given. It prints the time of the compression and the sizes. Units that hold no image are left out: copying them
// as they are comes with the writer of a later version.
//
// Usage: compress <input> <output> [rice|gzip1|gzip2|plio|hcompress|nocompress] [<quantize level>]

#include <openfits/openfits.h>

#include "arguments.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

openfits::compression compression_of(const std::string& name)
{
    if (name == "gzip1") {
        return openfits::compression::gzip_1;
    }
    if (name == "gzip2") {
        return openfits::compression::gzip_2;
    }
    if (name == "plio") {
        return openfits::compression::plio;
    }
    if (name == "hcompress") {
        return openfits::compression::hcompress;
    }
    if (name == "nocompress") {
        return openfits::compression::nocompress;
    }
    if (name == "rice") {
        return openfits::compression::rice;
    }
    return openfits::compression::automatic;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const std::vector<std::string> arguments = sample::arguments(argc, argv);
        if (arguments.size() < 2) {
            std::cerr << "usage: compress <input> <output> [rice|gzip1|gzip2|plio|hcompress|nocompress] "
                         "[<quantize level>]\n";
            return 2;
        }
        openfits::compression_options compression;
        compression.type = arguments.size() > 2 ? compression_of(arguments[2]) : openfits::compression::automatic;
        if (arguments.size() > 3) {
            compression.quantize_level = std::stod(arguments[3]);
        }

        const openfits::reader input(arguments[0]);
        // The stored values of each image, in the byte order of the host, which the writer borrows until the save.
        std::vector<std::vector<std::byte>> stored;
        openfits::writer output;
        std::uint64_t bytes = 0;
        for (const std::size_t unit : input.images()) {
            openfits::image_info image = input.image(unit);
            image.header = openfits::without_structural(image.header);
            const openfits::numeric_type type = [&image] {
                switch (*image.bitpix) {
                case openfits::bitpix::uint8:
                    return openfits::numeric_type::uint8;
                case openfits::bitpix::int16:
                    return openfits::numeric_type::int16;
                case openfits::bitpix::int32:
                    return openfits::numeric_type::int32;
                case openfits::bitpix::int64:
                    return openfits::numeric_type::int64;
                case openfits::bitpix::float32:
                    return openfits::numeric_type::float32;
                case openfits::bitpix::float64:
                    break;
                }
                return openfits::numeric_type::float64;
            }();
            std::vector<std::byte>& values =
                stored.emplace_back(image.geometry.element_count() * openfits::size_of(type));
            (void)input.read_pixels(unit, type, values, {.scaling = openfits::scaling_policy::raw});
            bytes += values.size();
            // fpack quantizes floating-point images with a level of 4 unless told otherwise; the library keeps them as
            // they are unless asked.
            openfits::compression_options options = compression;
            const bool floating = type == openfits::numeric_type::float32 || type == openfits::numeric_type::float64;
            if (floating && !options.quantize_level && options.type != openfits::compression::gzip_1 &&
                options.type != openfits::compression::gzip_2 && options.type != openfits::compression::nocompress) {
                options.quantize_level = 4.0;
            }
            (void)output.add_image(image, type, values,
                                   {.scaling = openfits::scaling_policy::raw, .compression = options});
        }
        if (output.hdu_count() == 0) {
            std::cerr << "compress: " << arguments[0] << " holds no image\n";
            return 1;
        }
        // The library reads no clock; the program does.
        const auto start = std::chrono::steady_clock::now();
        output.save(arguments[1]);
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

        const openfits::reader check(arguments[1]);
        std::cout << arguments[1] << ": " << stored.size() << " images, " << bytes << " bytes of values in "
                  << check.size() << " bytes, " << elapsed.count() << " s\n";
        return 0;
    } catch (const std::exception& failure) {
        (void)std::fputs("compress: ", stderr);
        (void)std::fputs(failure.what(), stderr);
        (void)std::fputs("\n", stderr);
        return 1;
    }
}
