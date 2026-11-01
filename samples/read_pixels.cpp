// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Reading the values of an image into memory of the application. This program opens a FITS file, describes the image
// of a unit, and reads its physical values as float into a buffer it owns, with the progress of the read on the way and
// the read cancelled when it asks for it; then it reads a region upside down into the middle of a larger buffer, its
// rows a pitch apart, as a stacking program reads the same region of many frames into one mosaic, and shows its corner.
//
// Usage: read_pixels [--cancel] <path> [<unit>]

#include <openfits/openfits.h>

#include "arguments.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iomanip>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string_view type_name(openfits::numeric_type type)
{
    constexpr std::array<std::string_view, 10> names{"int8",   "uint8", "int16",  "uint16",  "int32",
                                                     "uint32", "int64", "uint64", "float32", "float64"};
    return names.at(static_cast<std::size_t>(type));
}

} // namespace

int main(int argc, char** argv)
{
    try {
        bool cancel = false;
        std::vector<std::string> positional;
        for (const std::string& argument : sample::arguments(argc, argv)) {
            if (argument == "--cancel") {
                cancel = true;
            } else {
                positional.push_back(argument);
            }
        }
        if (positional.empty() || positional.size() > 2) {
            std::cerr << "usage: read_pixels [--cancel] <path> [<unit>]\n";
            return 2;
        }
        const openfits::reader file(positional[0]);
        const std::size_t index = positional.size() > 1 ? std::stoull(positional[1]) : file.first_image().value_or(0);

        // The description of the image, which says how its values are stored and what type holds them.
        const openfits::image_info image = file.image(index);
        // An image of a file has a BITPIX; a model for the writer may leave it to the type of its values.
        std::cout << "HDU " << index << ":";
        if (image.bitpix) {
            std::cout << " BITPIX " << static_cast<int>(*image.bitpix) << ",";
        }
        std::cout << " axes";
        for (const std::uint64_t length : image.geometry.axes) {
            std::cout << ' ' << length;
        }
        std::cout << ", BSCALE " << image.scaling.scale << ", BZERO " << image.scaling.zero;
        if (image.blank) {
            std::cout << ", BLANK " << *image.blank;
        }
        std::cout << "; its values are " << type_name(image.value_type()) << '\n';
        if (image.geometry.element_count() == 0) {
            std::cout << "the image holds no values\n";
            return 0;
        }

        // The physical values as float, undefined ones as NaN, into a buffer of the application. The progress function
        // reports the bytes read, and returning false cancels the read.
        std::vector<float> values(image.geometry.element_count());
        int last = -25;
        const openfits::progress_function progress = [&](std::uint64_t done, std::uint64_t total) {
            const int percent = total == 0 ? 100 : static_cast<int>(done * 100 / total);
            if (percent / 25 != last / 25) {
                std::cout << "read " << percent << "%\n";
                last = percent;
            }
            return !cancel || done == 0;
        };
        try {
            (void)file.read_pixels(index, std::span<float>(values), {.progress = progress});
        } catch (const openfits::cancelled_error&) {
            std::cout << "the read was cancelled\n";
            return 0;
        }
        std::size_t defined = 0;
        float low = 0.0F;
        float high = 0.0F;
        for (const float value : values) {
            if (!std::isnan(value)) {
                low = defined == 0 ? value : std::min(low, value);
                high = defined == 0 ? value : std::max(high, value);
                ++defined;
            }
        }
        std::cout << defined << " defined values of " << values.size() << ", from " << low << " to " << high << '\n';

        // A region of the first two axes, at most 4 x 3 elements, the second axis in reverse, into the middle of a
        // buffer of 8 x 6, its rows 8 elements apart.
        const std::uint64_t width = std::min<std::uint64_t>(4, image.geometry.axes[0]);
        const std::uint64_t height =
            image.geometry.axes.size() > 1 ? std::min<std::uint64_t>(3, image.geometry.axes[1]) : 1;
        openfits::region box;
        box.size.assign(image.geometry.axes.size(), 1);
        box.size[0] = width;
        if (image.geometry.axes.size() > 1) {
            box.size[1] = height;
        }
        box.step.assign(image.geometry.axes.size(), 1);
        if (image.geometry.axes.size() > 1) {
            box.step[1] = -1;
        }
        constexpr std::size_t pitch = 8;
        std::vector<float> mosaic(pitch * 6, 0.0F);
        const std::span<float> middle = std::span<float>(mosaic).subspan((pitch * 2) + 2);
        (void)file.read_pixels(index, middle.first(((height - 1) * pitch) + width), {.region = box, .pitch = pitch});
        std::cout << "the first rows, upside down, in the mosaic:\n";
        for (std::size_t y = 0; y < 6; ++y) {
            for (std::size_t x = 0; x < pitch; ++x) {
                std::cout << ' ' << std::setw(9) << std::setprecision(4) << mosaic[(y * pitch) + x];
            }
            std::cout << '\n';
        }
        return 0;
    } catch (const std::exception& failure) {
        (void)std::fputs("read_pixels: ", stderr);
        (void)std::fputs(failure.what(), stderr);
        (void)std::fputs("\n", stderr);
        return 1;
    }
}
