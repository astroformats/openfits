// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Converting an image. This program reads an image of any BITPIX, compressed or not (standard §10.1), as physical
// values, and writes it as an image of another BITPIX, compressed or not: a typed read and a typed write. Into
// integers, integer values that the type holds with an offset keep every value, and others take the linear scaling,
// BSCALE and BZERO, that maps their range onto that of the type; NaN is stored as BLANK. The header goes with the
// image, less its structural keywords, which the writer makes again.
//
// Usage: convert <input> <output> [<bitpix>] [none|rice|gzip1|gzip2|plio|hcompress] [<quantize level>]

#include <openfits/openfits.h>

#include "arguments.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

openfits::compression compression_of(const std::string& name)
{
    if (name == "rice") {
        return openfits::compression::rice;
    }
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
    return openfits::compression::none;
}

// The range of the values that an integer BITPIX stores.
std::pair<double, double> range_of(openfits::bitpix type)
{
    switch (type) {
    case openfits::bitpix::uint8:
        return {0.0, 255.0};
    case openfits::bitpix::int16:
        return {-32768.0, 32767.0};
    case openfits::bitpix::int32:
        return {-2147483648.0, 2147483647.0};
    default:
        return {-9.2e18, 9.2e18};
    }
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const std::vector<std::string> arguments = sample::arguments(argc, argv);
        if (arguments.size() < 2) {
            std::cerr << "usage: convert <input> <output> [<bitpix>] [none|rice|gzip1|gzip2|plio|hcompress] "
                         "[<quantize level>]\n";
            return 2;
        }
        const openfits::reader input(arguments[0]);
        const std::optional<std::size_t> unit = input.first_image();
        if (!unit) {
            std::cerr << "convert: " << arguments[0] << " holds no image\n";
            return 1;
        }
        const openfits::image_info source = input.image(*unit);
        if (!source.bitpix) {
            // A reader gives every image its BITPIX.
            std::cerr << "convert: the image of " << arguments[0] << " has no BITPIX\n";
            return 1;
        }
        const int source_bitpix = static_cast<int>(*source.bitpix);
        // The physical values, NaN for the undefined ones.
        const std::vector<double> values = input.read_pixels<double>(*unit);

        openfits::image_info target;
        target.geometry = source.geometry;
        target.header = openfits::without_structural(source.header);
        const int bitpix = arguments.size() > 2 ? std::stoi(arguments[2]) : source_bitpix;
        target.bitpix = static_cast<openfits::bitpix>(bitpix);
        if (bitpix > 0) {
            double low = std::numeric_limits<double>::max();
            double high = std::numeric_limits<double>::lowest();
            bool integral = true;
            for (const double value : values) {
                if (!std::isnan(value)) {
                    low = std::min(low, value);
                    high = std::max(high, value);
                    integral = integral && value == std::trunc(value);
                }
            }
            // The lowest value of the type stores NaN, when there is one.
            const auto [first, last] = range_of(*target.bitpix);
            const bool undefined = std::ranges::any_of(values, [](double value) { return std::isnan(value); });
            const double bottom = undefined ? first + 1.0 : first;
            if (integral && high - low <= last - bottom) {
                // Integers that the type holds with an offset at most: lossless.
                if (low < bottom || high > last) {
                    target.scaling.zero = low - bottom;
                }
            } else {
                // The range of the values onto that of the type.
                const double span = high > low ? high - low : 1.0;
                target.scaling.scale = span / (last - bottom);
                target.scaling.zero = low - (bottom * target.scaling.scale);
            }
            if (undefined) {
                target.blank = static_cast<std::int64_t>(first);
            }
        }

        openfits::image_write_options options;
        const std::string algorithm = arguments.size() > 3 ? arguments[3] : "none";
        if (algorithm != "none") {
            openfits::compression_options compression{.type = compression_of(algorithm)};
            if (arguments.size() > 4) {
                compression.quantize_level = std::stod(arguments[4]);
            }
            options.compression = compression;
        }
        openfits::writer output;
        const std::size_t written = output.add_image(target, std::span<const double>(values), options);
        output.save(arguments[1]);

        const openfits::reader check(arguments[1]);
        std::cout << arguments[0] << " unit " << *unit << ", BITPIX " << source_bitpix << " -> " << arguments[1]
                  << " unit " << written << ", BITPIX " << bitpix << ", "
                  << (check.hdu(written).kind() == openfits::hdu_kind::compressed_image ? algorithm : "uncompressed")
                  << ", " << check.size() << " bytes\n";
        return 0;
    } catch (const std::exception& failure) {
        (void)std::fputs("convert: ", stderr);
        (void)std::fputs(failure.what(), stderr);
        (void)std::fputs("\n", stderr);
        return 1;
    }
}
