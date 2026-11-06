// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/openfits.h>

#include "benchmarks.h"

#include <benchmark/benchmark.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openfits::bench {

namespace {

// The values of an image of a codec: the sky as 16-bit values of the unsigned convention, as 32-bit integers, as
// floats, or a mask of 16-bit values of 0 and 1, the image that PLIO_1 is for.
enum class values_kind : std::uint8_t
{
    u16,
    i32,
    f32,
    mask,
};

struct codec
{
    std::string_view name{};
    compression type = compression::rice;
    values_kind values = values_kind::u16;
    // Floating-point values quantized, at the level of fpack's default.
    bool quantized = false;
};

constexpr std::array<codec, 13> codecs{{
    {.name = "rice/u16", .type = compression::rice, .values = values_kind::u16},
    {.name = "rice/i32", .type = compression::rice, .values = values_kind::i32},
    {.name = "rice/f32_quantized", .type = compression::rice, .values = values_kind::f32, .quantized = true},
    {.name = "gzip_1/u16", .type = compression::gzip_1, .values = values_kind::u16},
    {.name = "gzip_1/i32", .type = compression::gzip_1, .values = values_kind::i32},
    {.name = "gzip_1/f32", .type = compression::gzip_1, .values = values_kind::f32},
    {.name = "gzip_2/u16", .type = compression::gzip_2, .values = values_kind::u16},
    {.name = "gzip_2/i32", .type = compression::gzip_2, .values = values_kind::i32},
    {.name = "gzip_2/f32", .type = compression::gzip_2, .values = values_kind::f32},
    {.name = "plio/mask", .type = compression::plio, .values = values_kind::mask},
    {.name = "hcompress/u16", .type = compression::hcompress, .values = values_kind::u16},
    {.name = "hcompress/i32", .type = compression::hcompress, .values = values_kind::i32},
    {.name = "hcompress/f32_quantized", .type = compression::hcompress, .values = values_kind::f32, .quantized = true},
}};

// The shapes of the tiles: a row, a band of 16 rows, and squares of 256 x 256.
struct tile_shape
{
    std::string_view name{};
    std::uint64_t width = 0;
    std::uint64_t height = 0;
};

constexpr std::array<tile_shape, 3> shapes{{{.name = "row", .width = 0, .height = 1},
                                            {.name = "rows_16", .width = 0, .height = 16},
                                            {.name = "square_256", .width = 256, .height = 256}}};

struct image_size
{
    std::string_view name{};
    std::size_t width = 0;
    std::size_t height = 0;

    [[nodiscard]] std::size_t pixels() const noexcept
    {
        return width * height;
    }
};

constexpr image_size small_image{.name = "small", .width = 1024, .height = 1024};
// The 100 MB frame of the parallel tiles: 10,000 x 5,000 16-bit values.
constexpr image_size large_image{.name = "large", .width = 10'000, .height = 5'000};

// The values of an image, kept until its writer is saved.
struct image_values
{
    std::vector<std::uint16_t> u16{};
    std::vector<std::int32_t> i32{};
    std::vector<float> f32{};
    std::vector<std::int16_t> mask{};
};

std::size_t value_size(values_kind kind) noexcept
{
    return kind == values_kind::u16 || kind == values_kind::mask ? 2 : 4;
}

compression_options options_of(const codec& kind, const tile_shape& shape, const image_size& size)
{
    compression_options options{.type = kind.type};
    options.tile = {shape.width == 0 ? size.width : shape.width, shape.height};
    if (kind.quantized) {
        options.quantize_level = 4.0;
    }
    return options;
}

// The image of a codec, added to output; its values stay in values until the save.
void add_image(writer& output, const codec& kind, const tile_shape& shape, const image_size& size, image_values& values)
{
    image_info info;
    info.geometry.axes = {size.width, size.height};
    const image_write_options options{.compression = options_of(kind, shape, size)};
    switch (kind.values) {
    case values_kind::u16:
        values.u16 = unsigned_sky(size.width, size.height);
        (void)output.add_image(info, std::span<const std::uint16_t>(values.u16), options);
        break;
    case values_kind::i32: {
        const std::vector<float> sky_values = sky(size.width, size.height);
        values.i32.resize(sky_values.size());
        for (std::size_t i = 0; i < sky_values.size(); ++i) {
            values.i32[i] = static_cast<std::int32_t>(std::lround(sky_values[i] * 1.0e6F));
        }
        (void)output.add_image(info, std::span<const std::int32_t>(values.i32), options);
        break;
    }
    case values_kind::f32:
        values.f32 = sky(size.width, size.height);
        (void)output.add_image(info, std::span<const float>(values.f32), options);
        break;
    case values_kind::mask: {
        const std::vector<float> sky_values = sky(size.width, size.height);
        values.mask.resize(sky_values.size());
        for (std::size_t i = 0; i < sky_values.size(); ++i) {
            values.mask[i] = sky_values[i] > 0.1F ? 1 : 0;
        }
        (void)output.add_image(info, std::span<const std::int16_t>(values.mask), options);
        break;
    }
    }
}

// The file of an image of a codec, written the first time it is asked for: an empty primary and the image, unit 1.
const std::string& file_of(const codec& kind, const tile_shape& shape, const image_size& size)
{
    static std::map<std::string, std::string> files;
    std::string name = "codec-" + std::string(kind.name) + "-" + std::string(shape.name) + "-" + std::string(size.name);
    for (char& c : name) {
        if (c == '/') {
            c = '-';
        }
    }
    name += ".fits";
    const auto found = files.find(name);
    if (found != files.end()) {
        return found->second;
    }
    image_values values;
    writer output;
    add_image(output, kind, shape, size, values);
    std::string path = file_path(name);
    output.save(path);
    return files.emplace(name, std::move(path)).first->second;
}

// The decode of every tile: the image read whole into memory of the application as its own type, on threads threads.
void decode(benchmark::State& state, codec kind, tile_shape shape, image_size size, unsigned threads)
{
    const reader file(file_of(kind, shape, size));
    std::vector<std::byte> destination(size.pixels() * value_size(kind.values));
    with_threads(threads, [&] {
        while (state.KeepRunning()) {
            switch (kind.values) {
            case values_kind::u16:
                (void)file.read_pixels(
                    1, std::span<std::uint16_t>(reinterpret_cast<std::uint16_t*>(destination.data()), size.pixels()));
                break;
            case values_kind::i32:
                (void)file.read_pixels(
                    1, std::span<std::int32_t>(reinterpret_cast<std::int32_t*>(destination.data()), size.pixels()));
                break;
            case values_kind::f32:
                (void)file.read_pixels(1,
                                       std::span<float>(reinterpret_cast<float*>(destination.data()), size.pixels()));
                break;
            case values_kind::mask:
                (void)file.read_pixels(
                    1, std::span<std::int16_t>(reinterpret_cast<std::int16_t*>(destination.data()), size.pixels()));
                break;
            }
            benchmark::DoNotOptimize(destination.data());
            benchmark::ClobberMemory();
        }
    });
    count_bytes(state, size.pixels() * value_size(kind.values));
}

// The encode of every tile and the save of the file, into a sink that keeps no bytes, on threads threads.
void encode(benchmark::State& state, codec kind, tile_shape shape, image_size size, unsigned threads)
{
    image_values values;
    writer output;
    add_image(output, kind, shape, size, values);
    with_threads(threads, [&] {
        while (state.KeepRunning()) {
            discarding_sink sink;
            output.save(sink);
        }
    });
    count_bytes(state, size.pixels() * value_size(kind.values));
}

void register_codec(const codec& kind, const tile_shape& shape, const image_size& size, unsigned threads)
{
    const std::string name = "codec/" + std::string(kind.name) + "/" + std::string(shape.name);
    const std::string suffix = "/" + std::string(size.name) + "/threads_" + std::to_string(threads);
    benchmark::RegisterBenchmark(name + "/decode" + suffix, decode, kind, shape, size, threads)
        ->Unit(benchmark::kMillisecond)
        ->UseRealTime();
    benchmark::RegisterBenchmark(name + "/encode" + suffix, encode, kind, shape, size, threads)
        ->Unit(benchmark::kMillisecond)
        ->UseRealTime();
}

} // namespace

void register_codec_benchmarks()
{
    // Each codec in each shape of tiles, for the types it takes; H-Compress takes two-dimensional tiles alone.
    for (const codec& kind : codecs) {
        for (const tile_shape& shape : shapes) {
            if (kind.type == compression::hcompress && shape.height < 4) {
                continue;
            }
            for (const unsigned threads : {1U, 8U}) {
                if (threads == 1 || parallel_library) {
                    register_codec(kind, shape, small_image, threads);
                }
            }
        }
    }
    // The 100 MB frame of RICE_1 tiles of a row, on 1, 4, 8 and 16 threads.
    for (const unsigned threads : {1U, 4U, 8U, 16U}) {
        if (threads == 1 || parallel_library) {
            register_codec(codecs[0], shapes[0], large_image, threads);
        }
    }
}

} // namespace openfits::bench
