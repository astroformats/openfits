// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/openfits.h>

#include "benchmarks.h"

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
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

// The images of the benchmarks: 16-bit values of the unsigned convention, floats, and 16-bit values scaled by BSCALE
// 0.01 and BZERO 100; and the first with DATASUM and CHECKSUM.
enum class image_kind
{
    u16,
    f32,
    i16_scaled,
    u16_checked,
};

// The directory of the files, made in the temporary directory of the system when the first file is written.
std::filesystem::path& directory()
{
    static std::filesystem::path path;
    if (path.empty()) {
        std::random_device random;
        path = std::filesystem::temp_directory_path() / ("openfits-benchmarks-" + std::to_string(random()));
        std::filesystem::create_directories(path);
    }
    return path;
}

std::string utf8(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

// The values of the scaled image: the sky times 100, which BSCALE 0.01 and BZERO 100 store in 16 bits.
std::vector<float> physical_sky(const frame_size& size)
{
    std::vector<float> values = sky(size.width, size.height);
    for (float& value : values) {
        value *= 100.0F;
    }
    return values;
}

image_info scaled_image(const frame_size& size)
{
    image_info info;
    info.geometry.axes = {size.width, size.height};
    info.bitpix = bitpix::int16;
    info.scaling = {.scale = 0.01, .zero = 100.0};
    return info;
}

image_info plain_image(const frame_size& size)
{
    image_info info;
    info.geometry.axes = {size.width, size.height};
    return info;
}

// The file of an image of a kind and a size, written with the library the first time it is asked for.
const std::string& file_of(image_kind kind, const frame_size& size)
{
    static std::map<std::string, std::string> files;
    const std::string name = std::to_string(static_cast<int>(kind)) + "-" + std::string(size.name) + ".fits";
    const auto found = files.find(name);
    if (found != files.end()) {
        return found->second;
    }
    std::string path = file_path(name);
    writer output;
    switch (kind) {
    case image_kind::u16: {
        const std::vector<std::uint16_t> values = unsigned_sky(size.width, size.height);
        (void)output.add_image(plain_image(size), std::span<const std::uint16_t>(values));
        output.save(path);
        break;
    }
    case image_kind::f32: {
        const std::vector<float> values = sky(size.width, size.height);
        (void)output.add_image(plain_image(size), std::span<const float>(values));
        output.save(path);
        break;
    }
    case image_kind::i16_scaled: {
        const std::vector<float> values = physical_sky(size);
        (void)output.add_image(scaled_image(size), std::span<const float>(values));
        output.save(path);
        break;
    }
    case image_kind::u16_checked: {
        const std::vector<std::uint16_t> values = unsigned_sky(size.width, size.height);
        (void)output.add_image(plain_image(size), std::span<const std::uint16_t>(values));
        output.save(path, {.checksums = true});
        break;
    }
    }
    return files.emplace(name, std::move(path)).first->second;
}

// The file of an empty primary HDU and a thousand IMAGE extensions of 10 x 10 bytes, each with a keyword of its own.
const std::string& thousand_units()
{
    static const std::string path = [] {
        std::string name = file_path("thousand-units.fits");
        static const std::vector<std::uint8_t> values(100, 7);
        writer output;
        (void)output.add_header({});
        for (int i = 0; i < 1000; ++i) {
            image_info info;
            info.geometry.axes = {10, 10};
            info.header.set("EXTNAME", "UNIT" + std::to_string(i));
            (void)output.add_image(info, std::span<const std::uint8_t>(values));
        }
        output.save(name);
        return name;
    }();
    return path;
}

std::size_t stored_size(image_kind kind) noexcept
{
    return kind == image_kind::f32 ? 4 : 2;
}

// A read of the whole image into memory of the application, as values of T: the bytes counted are those stored.
template <typename T> void read_image(benchmark::State& state, image_kind kind, frame_size size)
{
    const reader file(file_of(kind, size));
    std::vector<T> destination(size.pixels());
    while (state.KeepRunning()) {
        (void)file.read_pixels(0, std::span<T>(destination));
        benchmark::DoNotOptimize(destination.data());
        benchmark::ClobberMemory();
    }
    count_bytes(state, size.pixels() * stored_size(kind));
}

// The read of the stacking programs: a region of 512 x 512 in the middle of the image, as float.
void read_region(benchmark::State& state, frame_size size)
{
    const reader file(file_of(image_kind::u16, size));
    constexpr std::uint64_t side = 512;
    const region box{.start = {(size.width - side) / 2, (size.height - side) / 2}, .size = {side, side}};
    std::vector<float> destination(side * side);
    while (state.KeepRunning()) {
        (void)file.read_pixels(0, std::span<float>(destination), {.region = box});
        benchmark::DoNotOptimize(destination.data());
        benchmark::ClobberMemory();
    }
    count_bytes(state, side * side * 2);
}

template <typename T>
void write_image(benchmark::State& state, image_info info, std::vector<T> values, const write_options& options = {})
{
    writer output;
    (void)output.add_image(std::move(info), std::span<const T>(values));
    while (state.KeepRunning()) {
        discarding_sink sink;
        output.save(sink, options);
    }
    count_bytes(state, values.size() * sizeof(T));
}

// The checksums of a unit verified alone: the whole unit read and summed.
void verify_unit(benchmark::State& state, frame_size size)
{
    const reader file(file_of(image_kind::u16_checked, size));
    while (state.KeepRunning()) {
        benchmark::DoNotOptimize(file.verify(0));
    }
    count_bytes(state, size.pixels() * 2);
}

// The walk of every unit of a file of a thousand units, from its open.
void walk_units(benchmark::State& state)
{
    const std::string& path = thousand_units();
    while (state.KeepRunning()) {
        const reader file(path);
        benchmark::DoNotOptimize(file.hdu_count());
    }
    state.SetItemsProcessed(state.iterations() * 1001);
}

} // namespace

std::string file_path(std::string_view name)
{
    return utf8(directory() / std::filesystem::path(std::u8string(name.begin(), name.end())));
}

void register_image_benchmarks()
{
    for (const frame_size& size : sizes) {
        const std::string suffix = "/" + std::string(size.name);
        benchmark::RegisterBenchmark("read/u16/as_u16" + suffix, read_image<std::uint16_t>, image_kind::u16, size)
            ->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark("read/u16/as_float" + suffix, read_image<float>, image_kind::u16, size)
            ->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark("read/f32/as_float" + suffix, read_image<float>, image_kind::f32, size)
            ->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark("read/i16_scaled/as_float" + suffix, read_image<float>, image_kind::i16_scaled,
                                     size)
            ->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark("read/u16/region_512" + suffix, read_region, size)->Unit(benchmark::kMicrosecond);
        benchmark::RegisterBenchmark("write/u16" + suffix, [size](benchmark::State& state) {
            write_image(state, plain_image(size), unsigned_sky(size.width, size.height));
        })->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark("write/f32" + suffix, [size](benchmark::State& state) {
            write_image(state, plain_image(size), sky(size.width, size.height));
        })->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark("write/f32_as_i16_scaled" + suffix, [size](benchmark::State& state) {
            write_image(state, scaled_image(size), physical_sky(size));
        })->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark("verify/u16" + suffix, verify_unit, size)->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark("read/u16/verified" + suffix, read_image<std::uint16_t>, image_kind::u16_checked,
                                     size)
            ->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark("write/u16/checksums" + suffix, [size](benchmark::State& state) {
            write_image(state, plain_image(size), unsigned_sky(size.width, size.height), {.checksums = true});
        })->Unit(benchmark::kMillisecond);
    }
    benchmark::RegisterBenchmark("walk/thousand_units", walk_units)->Unit(benchmark::kMillisecond);
}

void remove_benchmark_files()
{
    std::filesystem::path& path = directory();
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
}

} // namespace openfits::bench
