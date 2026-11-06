// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/openfits.h>

#include "benchmarks.h"

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openfits::bench {

namespace {

struct frame_size
{
    std::string_view name{};
    std::size_t width = 0;
    std::size_t height = 0;

    [[nodiscard]] std::size_t pixels() const noexcept
    {
        return width * height;
    }
};

constexpr std::array<frame_size, 2> sizes{
    {{.name = "small", .width = 1024, .height = 1024}, {.name = "frame", .width = 6000, .height = 4000}}};

// A compressed image: its values, 16-bit unsigned or float, and how they are compressed.
struct packed_kind
{
    std::string_view name{};
    bool floating = false;
    compression type = compression::rice;
    // Rows of each tile, the first axis whole; 0 for the default of the algorithm.
    std::uint64_t rows = 0;
    bool quantized = false;
};

constexpr std::array<packed_kind, 6> kinds{{
    {.name = "u16_rice", .type = compression::rice},
    {.name = "u16_rice_band", .type = compression::rice, .rows = 16},
    {.name = "u16_gzip2", .type = compression::gzip_2},
    {.name = "u16_hcompress", .type = compression::hcompress},
    {.name = "f32_quantized", .floating = true, .type = compression::rice, .quantized = true},
    {.name = "f32_gzip2", .floating = true, .type = compression::gzip_2},
}};

compression_options options_of(const packed_kind& kind, const frame_size& size)
{
    compression_options options{.type = kind.type};
    if (kind.rows != 0) {
        options.tile = {size.width, kind.rows};
    }
    if (kind.quantized) {
        options.quantize_level = 4.0;
    }
    return options;
}

image_info image_of(const frame_size& size)
{
    image_info info;
    info.geometry.axes = {size.width, size.height};
    return info;
}

// The image of a kind, added to output, whose values stay in values until the save.
void add_packed(writer& output, const packed_kind& kind, const frame_size& size, std::vector<std::uint16_t>& integers,
                std::vector<float>& reals)
{
    const image_write_options options{.compression = options_of(kind, size)};
    if (kind.floating) {
        reals = sky(size.width, size.height);
        (void)output.add_image(image_of(size), std::span<const float>(reals), options);
    } else {
        integers = unsigned_sky(size.width, size.height);
        (void)output.add_image(image_of(size), std::span<const std::uint16_t>(integers), options);
    }
}

// The file of a compressed image, written the first time it is asked for: an empty primary and the image, unit 1.
const std::string& file_of(const packed_kind& kind, const frame_size& size)
{
    static std::map<std::string, std::string> files;
    const std::string name = "packed-" + std::string(kind.name) + "-" + std::string(size.name) + ".fits";
    const auto found = files.find(name);
    if (found != files.end()) {
        return found->second;
    }
    std::vector<std::uint16_t> integers;
    std::vector<float> reals;
    writer output;
    add_packed(output, kind, size, integers, reals);
    std::string path = file_path(name);
    output.save(path);
    return files.emplace(name, std::move(path)).first->second;
}

// A read of the whole image into memory of the application as its own type: the bytes counted are the values.
template <typename T> void read_whole(benchmark::State& state, packed_kind kind, frame_size size)
{
    const reader file(file_of(kind, size));
    std::vector<T> destination(size.pixels());
    while (state.KeepRunning()) {
        (void)file.read_pixels(1, std::span<T>(destination));
        benchmark::DoNotOptimize(destination.data());
        benchmark::ClobberMemory();
    }
    count_bytes(state, size.pixels() * sizeof(T));
}

// The image read one row after the other, as a viewer or a stacking program reads it, through a tile cache or not.
void read_rows(benchmark::State& state, packed_kind kind, frame_size size, bool cached)
{
    const reader file(file_of(kind, size));
    std::vector<std::uint16_t> row(size.width);
    while (state.KeepRunning()) {
        // A cache of each pass, so that each pass decodes every tile once.
        tile_cache cache(std::uint64_t{16} << 20);
        for (std::uint64_t y = 0; y < size.height; ++y) {
            (void)file.read_pixels(
                1, std::span<std::uint16_t>(row),
                {.region = region{.start = {0, y}, .size = {size.width, 1}}, .cache = cached ? &cache : nullptr});
            benchmark::DoNotOptimize(row.data());
        }
        benchmark::ClobberMemory();
    }
    count_bytes(state, size.pixels() * 2);
}

// A hundred regions of 64 x 64 pixels read from the image, at places spread over it as a viewer or a program that
// samples an image reads them, through a tile cache or not: each region decodes the tiles that it meets, which a
// cache keeps for the regions after it.
void read_regions(benchmark::State& state, packed_kind kind, frame_size size, bool cached)
{
    const reader file(file_of(kind, size));
    constexpr std::uint64_t side = 64;
    std::vector<std::uint16_t> values(side * side);
    std::vector<region> boxes;
    std::uint64_t seed = 1;
    for (int i = 0; i < 100; ++i) {
        seed = (seed * 6364136223846793005U) + 1442695040888963407U;
        const std::uint64_t x = (seed >> 33U) % (size.width - side);
        const std::uint64_t y = (seed >> 13U) % (size.height - side);
        boxes.push_back(region{.start = {x, y}, .size = {side, side}});
    }
    while (state.KeepRunning()) {
        // A cache of each pass, so that each pass decodes each tile once at most.
        tile_cache cache(std::uint64_t{16} << 20);
        for (const region& box : boxes) {
            (void)file.read_pixels(1, std::span<std::uint16_t>(values),
                                   {.region = box, .cache = cached ? &cache : nullptr});
            benchmark::DoNotOptimize(values.data());
        }
        benchmark::ClobberMemory();
    }
    count_bytes(state, 100 * side * side * 2);
}

// The compression of the image and its save, into a sink that keeps no bytes.
void write_packed(benchmark::State& state, packed_kind kind, frame_size size)
{
    std::vector<std::uint16_t> integers;
    std::vector<float> reals;
    writer output;
    add_packed(output, kind, size, integers, reals);
    while (state.KeepRunning()) {
        discarding_sink sink;
        output.save(sink);
    }
    count_bytes(state, size.pixels() * (kind.floating ? 4 : 2));
}

} // namespace

void register_compression_benchmarks()
{
    for (const frame_size& size : sizes) {
        const std::string suffix = "/" + std::string(size.name);
        for (const packed_kind& kind : kinds) {
            if (kind.floating) {
                benchmark::RegisterBenchmark("read/" + std::string(kind.name) + "/whole" + suffix, read_whole<float>,
                                             kind, size)
                    ->Unit(benchmark::kMillisecond);
            } else {
                benchmark::RegisterBenchmark("read/" + std::string(kind.name) + "/whole" + suffix,
                                             read_whole<std::uint16_t>, kind, size)
                    ->Unit(benchmark::kMillisecond);
            }
            benchmark::RegisterBenchmark("write/" + std::string(kind.name) + suffix, write_packed, kind, size)
                ->Unit(benchmark::kMillisecond);
        }
        benchmark::RegisterBenchmark("read/u16_rice_band/rows" + suffix, read_rows, kinds[1], size, false)
            ->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark("read/u16_rice_band/rows_cached" + suffix, read_rows, kinds[1], size, true)
            ->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark("read/u16_rice_band/regions_100" + suffix, read_regions, kinds[1], size, false)
            ->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark("read/u16_rice_band/regions_100_cached" + suffix, read_regions, kinds[1], size,
                                     true)
            ->Unit(benchmark::kMillisecond);
    }
}

} // namespace openfits::bench
