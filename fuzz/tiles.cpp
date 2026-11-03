// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Compressed images (standard §10.1) on untrusted bytes: the input read as a file under small limits, the values of
// each compressed image read whole, as a region and through a cache, which must give values or an error of the
// library's kinds; and an image of the input written with an algorithm, a type and tiles that its first bytes choose,
// which must read back as written, within the spacing of a quantization. Any other exception escapes and fails the run.

#include <openfits/compression.h>
#include <openfits/error.h>
#include <openfits/image.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/writer.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

namespace {

void require(bool condition)
{
    if (!condition) {
        std::abort();
    }
}

openfits::read_options small_limits()
{
    openfits::read_options options;
    options.limits.max_hdus = 8;
    options.limits.max_header_bytes = std::uint64_t{32} * 2880;
    options.limits.max_total_header_bytes = std::uint64_t{64} * 2880;
    options.limits.max_allocation = std::uint64_t{16} << 20;
    options.limits.max_tile_bytes = std::uint64_t{4} << 20;
    options.limits.max_tiles = 4096;
    options.limits.max_diagnostics = 32;
    return options;
}

void read_file(std::span<const std::byte> input)
{
    try {
        const openfits::reader file(std::make_unique<openfits::memory_source>(input), small_limits());
        openfits::tile_cache cache(std::uint64_t{1} << 20);
        for (std::size_t i = 0; i < file.hdu_count(); ++i) {
            if (file.hdu(i).kind() != openfits::hdu_kind::compressed_image) {
                continue;
            }
            try {
                const openfits::image_info image = file.image(i);
                if (image.geometry.element_count() > (std::uint64_t{1} << 20)) {
                    continue;
                }
                (void)file.read_pixels<float>(i, {.verify_checksum = false});
                (void)file.read_pixels<std::int32_t>(
                    i, {.blank = openfits::blank_policy::value, .overflow = openfits::overflow_policy::clamp});
                openfits::region box;
                for (const std::uint64_t axis : image.geometry.axes) {
                    box.start.push_back(axis / 3);
                    box.size.push_back(axis - (axis / 3));
                    box.step.push_back(axis % 2 == 0 ? 1 : -2);
                }
                (void)file.read_pixels<double>(i, {.region = box, .cache = &cache});
                (void)file.read_pixels<double>(i, {.region = box, .cache = &cache});
            } catch (const openfits::error&) {
                // A compressed image whose keywords or tiles do not read.
                continue;
            }
        }
    } catch (const openfits::error&) {
        // Not a FITS file, or one beyond the limits.
        return;
    }
}

openfits::compression algorithm_of(std::byte selector) noexcept
{
    switch (std::to_integer<unsigned>(selector) % 6U) {
    case 0:
        return openfits::compression::rice;
    case 1:
        return openfits::compression::gzip_1;
    case 2:
        return openfits::compression::gzip_2;
    case 3:
        return openfits::compression::plio;
    case 4:
        return openfits::compression::hcompress;
    default:
        return openfits::compression::nocompress;
    }
}

template <typename T> void round_trip(std::span<const std::byte> input, const openfits::compression_options& options)
{
    const std::size_t width = 4 + (std::to_integer<std::size_t>(input[1]) % 29);
    const std::size_t count = input.size() / sizeof(T);
    if (count < width * 4) {
        return;
    }
    const std::size_t height = count / width;
    std::vector<T> values(width * height);
    std::memcpy(values.data(), input.data(), values.size() * sizeof(T));
    if constexpr (std::is_floating_point_v<T>) {
        for (T& value : values) {
            if (!std::isfinite(value) || std::abs(value) > T{1e6}) {
                value = T{0};
            }
        }
    }
    openfits::image_info info;
    info.geometry.axes = {width, height};
    openfits::writer output;
    try {
        output.add_image(info, std::span<const T>(values), {.compression = options});
    } catch (const openfits::validation_error&) {
        return;
    }
    openfits::memory_sink sink;
    try {
        output.save(sink);
    } catch (const openfits::validation_error&) {
        return;
    }
    const openfits::reader file(std::make_unique<openfits::memory_source>(sink.release()));
    const std::vector<T> back = file.read_pixels<T>(1);
    if (!options.quantize_level) {
        require(back == values);
    } else {
        for (std::size_t i = 0; i < values.size(); ++i) {
            require(std::abs(static_cast<double>(back[i]) - static_cast<double>(values[i])) <=
                    std::abs(*options.quantize_level) * 1.001 + (std::abs(static_cast<double>(values[i])) * 1e-6));
        }
    }
}

} // namespace

// The entry point that libFuzzer, and the replay driver, call for every input. Its name is fixed by libFuzzer.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::span<const std::byte> input(reinterpret_cast<const std::byte*>(data), size);
    read_file(input);
    if (size < 8) {
        return 0;
    }
    openfits::compression_options options;
    options.type = algorithm_of(input[0]);
    const auto shape = std::to_integer<unsigned>(input[2]);
    if ((shape & 3U) == 1) {
        options.tile = {4 + (shape % 13), 4 + (shape % 7)};
    } else if ((shape & 3U) == 2) {
        options.tile = {0, 0};
    }
    const std::span<const std::byte> values = input.subspan(4, std::min<std::size_t>(size - 4, std::size_t{1} << 16));
    switch (std::to_integer<unsigned>(input[3]) % 4U) {
    case 0:
        round_trip<std::uint8_t>(values, options);
        break;
    case 1:
        round_trip<std::int16_t>(values, options);
        break;
    case 2:
        round_trip<std::int32_t>(values, options);
        break;
    default:
        options.quantize_level =
            (std::to_integer<unsigned>(input[3]) & 4U) != 0 ? std::optional<double>(-0.125) : std::nullopt;
        if (!options.quantize_level) {
            options.type = (std::to_integer<unsigned>(input[3]) & 8U) != 0 ? openfits::compression::gzip_1
                                                                           : openfits::compression::gzip_2;
        }
        round_trip<float>(values, options);
        break;
    }
    return 0;
}
