// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/openfits.h>

#include "benchmarks.h"

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
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

void write_thousand_units(const std::string& name, bool primary_image);

// The file of a primary HDU and a thousand IMAGE extensions of 10 x 10 bytes, each with a keyword of its own; the
// primary is empty, or holds an image of 10 x 10 bytes too.
const std::string& thousand_units(bool primary_image = false)
{
    static const std::array<std::string, 2> paths = [] {
        std::array<std::string, 2> names{file_path("thousand-units.fits"), file_path("thousand-units-image.fits")};
        for (std::size_t k = 0; k < names.size(); ++k) {
            write_thousand_units(names[k], k == 1);
        }
        return names;
    }();
    return paths[primary_image ? 1 : 0];
}

// The header of 100,000 cards: integers, reals and strings with comments, which a reader parses at its open.
const std::string& hundred_thousand_cards()
{
    static const std::string path = [] {
        std::string name = file_path("hundred-thousand-cards.fits");
        image_info info;
        for (int i = 0; i < 100'000; ++i) {
            const std::string keyword = "K" + std::to_string(i);
            switch (i % 3) {
            case 0:
                info.header.append(card(keyword, std::int64_t{i}, "an integer"));
                break;
            case 1:
                info.header.append(card(keyword, i / 7.0, "a real number"));
                break;
            default:
                info.header.append(card(keyword, "value " + std::to_string(i), "a string"));
                break;
            }
        }
        writer output;
        (void)output.add_header(std::move(info.header));
        output.save(name);
        return name;
    }();
    return path;
}

// The open of the file of a header of 100,000 cards, which reads and parses them.
void parse_header(benchmark::State& state)
{
    const std::string& path = hundred_thousand_cards();
    while (state.KeepRunning()) {
        const reader file(path);
        benchmark::DoNotOptimize(file.hdu(0).header().size());
    }
    state.SetItemsProcessed(state.iterations() * 100'000);
}

// The open of the file of a thousand units and the read of the values of its primary, or of its last unit, which the
// walk reaches past the headers of the others.
void open_and_read(benchmark::State& state, bool last)
{
    const std::string& path = thousand_units(true);
    std::vector<std::uint8_t> values(100);
    while (state.KeepRunning()) {
        const reader file(path);
        (void)file.read_pixels(last ? 1000 : 0, std::span<std::uint8_t>(values));
        benchmark::DoNotOptimize(values.data());
    }
    state.SetItemsProcessed(state.iterations());
}

// The conformance report of the file of a thousand units: a strict walk of every unit and the checks of their data.
void verify_units(benchmark::State& state)
{
    const std::string& path = thousand_units();
    while (state.KeepRunning()) {
        const reader file(path);
        benchmark::DoNotOptimize(verify(file).size());
    }
    state.SetItemsProcessed(state.iterations() * 1001);
}

// The floor of a read of a whole image: its stored bytes read from the file into memory and copied into the
// destination, without a conversion.
void read_raw(benchmark::State& state, frame_size size)
{
    const std::string& path = file_of(image_kind::u16, size);
    const reader file(path);
    const std::uint64_t offset = file.hdu(0).data_offset();
    const file_source source(path);
    std::vector<std::byte> staging(size.pixels() * 2);
    std::vector<std::byte> destination(size.pixels() * 2);
    while (state.KeepRunning()) {
        source.read(offset, staging);
        std::memcpy(destination.data(), staging.data(), staging.size());
        benchmark::DoNotOptimize(destination.data());
        benchmark::ClobberMemory();
    }
    count_bytes(state, size.pixels() * 2);
}

void write_thousand_units(const std::string& name, bool primary_image)
{
    static const std::vector<std::uint8_t> values(100, 7);
    writer output;
    if (primary_image) {
        image_info info;
        info.geometry.axes = {10, 10};
        (void)output.add_image(info, std::span<const std::uint8_t>(values));
    } else {
        (void)output.add_header({});
    }
    {
        for (int i = 0; i < 1000; ++i) {
            image_info info;
            info.geometry.axes = {10, 10};
            info.header.set("EXTNAME", "UNIT" + std::to_string(i));
            (void)output.add_image(info, std::span<const std::uint8_t>(values));
        }
    }
    output.save(name);
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

// The checksums of a unit verified alone: the whole unit read and summed, from the file, or from its bytes in memory,
// which leaves the copies of the system out.
void verify_unit(benchmark::State& state, frame_size size, bool in_memory)
{
    const std::string& path = file_of(image_kind::u16_checked, size);
    std::vector<std::byte> bytes;
    if (in_memory) {
        const file_source source(path);
        bytes.resize(source.size());
        source.read(0, bytes);
    }
    const reader file =
        in_memory ? reader(std::make_unique<memory_source>(std::span<const std::byte>(bytes))) : reader(path);
    while (state.KeepRunning()) {
        benchmark::DoNotOptimize(file.verify(0));
    }
    count_bytes(state, size.pixels() * 2);
}

// The files of count copies of the 16-bit image of a size, for reads of many files at once.
std::vector<std::string> copies_of(const frame_size& size, unsigned count)
{
    std::vector<std::string> paths;
    const std::string& original = file_of(image_kind::u16, size);
    for (unsigned i = 0; i < count; ++i) {
        std::string path = file_path("copy-" + std::to_string(i) + "-" + std::string(size.name) + ".fits");
        const std::filesystem::path target(std::u8string(path.begin(), path.end()));
        if (!std::filesystem::exists(target)) {
            std::filesystem::copy_file(std::filesystem::path(std::u8string(original.begin(), original.end())), target);
        }
        paths.push_back(std::move(path));
    }
    return paths;
}

// Rows [first, last) of a unit of width columns, read as 16-bit values into the same rows of destination.
void read_band(const reader& file, std::uint64_t width, std::uint64_t first, std::uint64_t last,
               std::span<std::uint16_t> destination)
{
    const region band{.start = {0, first}, .size = {width, last - first}};
    (void)file.read_pixels(0, destination.subspan(first * width, (last - first) * width), {.region = band});
}

// threads threads reading one image through one reader, each a band of its rows, as a decoder of strips reads it.
void read_one_file(benchmark::State& state, frame_size size, unsigned threads)
{
    const reader file(file_of(image_kind::u16, size));
    std::vector<std::uint16_t> destination(size.pixels());
    while (state.KeepRunning()) {
        std::vector<std::thread> workers;
        workers.reserve(threads);
        for (unsigned t = 0; t < threads; ++t) {
            workers.emplace_back([&, t] {
                read_band(file, size.width, size.height * t / threads, size.height * (t + 1) / threads, destination);
            });
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
        benchmark::DoNotOptimize(destination.data());
        benchmark::ClobberMemory();
    }
    count_bytes(state, size.pixels() * 2);
}

// threads threads, each opening a file of its own and reading its image whole, as a stacking program reads its frames.
void read_many_files(benchmark::State& state, frame_size size, unsigned threads)
{
    const std::vector<std::string> paths = copies_of(size, threads);
    std::vector<std::vector<std::uint16_t>> destinations(threads, std::vector<std::uint16_t>(size.pixels()));
    while (state.KeepRunning()) {
        std::vector<std::thread> workers;
        workers.reserve(threads);
        for (unsigned t = 0; t < threads; ++t) {
            workers.emplace_back([&, t] {
                const reader file(paths[t]);
                (void)file.read_pixels(0, std::span<std::uint16_t>(destinations[t]));
            });
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
        benchmark::ClobberMemory();
    }
    count_bytes(state, size.pixels() * 2 * threads);
}

// The frames of a stack: copies of the 16-bit frame of a camera, each a file of its own.
constexpr unsigned stack_frames = 100;

// The read of a stacking program: the same region of width x height pixels, in the middle of the frame, from each of
// the frames of a stack, the files open for the whole stack, as float.
void read_stack(benchmark::State& state, std::uint64_t width, std::uint64_t height)
{
    const frame_size& size = sizes[1];
    const std::vector<std::string> paths = copies_of(size, stack_frames);
    std::vector<reader> files;
    files.reserve(paths.size());
    for (const std::string& path : paths) {
        files.emplace_back(path);
    }
    const region box{.start = {(size.width - width) / 2, (size.height - height) / 2}, .size = {width, height}};
    std::vector<float> destination(width * height);
    while (state.KeepRunning()) {
        for (const reader& file : files) {
            (void)file.read_pixels(0, std::span<float>(destination), {.region = box});
            benchmark::DoNotOptimize(destination.data());
        }
        benchmark::ClobberMemory();
    }
    count_bytes(state, std::uint64_t{stack_frames} * width * height * 2);
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
        benchmark::RegisterBenchmark("verify/u16" + suffix, verify_unit, size, false)->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark("read/u16/verified" + suffix, read_image<std::uint16_t>, image_kind::u16_checked,
                                     size)
            ->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark("write/u16/checksums" + suffix, [size](benchmark::State& state) {
            write_image(state, plain_image(size), unsigned_sky(size.width, size.height), {.checksums = true});
        })->Unit(benchmark::kMillisecond);
    }
    benchmark::RegisterBenchmark("walk/thousand_units", walk_units)->Unit(benchmark::kMillisecond);
    benchmark::RegisterBenchmark("header/parse/hundred_thousand_cards", parse_header)->Unit(benchmark::kMillisecond);
    benchmark::RegisterBenchmark("open/thousand_units/read_first", open_and_read, false)->Unit(benchmark::kMicrosecond);
    benchmark::RegisterBenchmark("open/thousand_units/read_last", open_and_read, true)->Unit(benchmark::kMillisecond);
    benchmark::RegisterBenchmark("verify/thousand_units", verify_units)->Unit(benchmark::kMillisecond);
    for (const frame_size& size : sizes) {
        benchmark::RegisterBenchmark("read/u16/raw_read_and_copy/" + std::string(size.name), read_raw, size)
            ->Unit(benchmark::kMillisecond);
    }
    // The checksum of 100 MB, a unit of 10,000 x 5,000 16-bit values.
    constexpr frame_size large{.name = "large", .width = 10'000, .height = 5'000};
    benchmark::RegisterBenchmark("verify/u16/large", verify_unit, large, false)->Unit(benchmark::kMillisecond);
    benchmark::RegisterBenchmark("verify/u16/large_in_memory", verify_unit, large, true)->Unit(benchmark::kMillisecond);
    // The stacking read, for regions from 64 x 64 to bands of whole rows.
    struct stack_region
    {
        std::string_view name{};
        std::uint64_t width = 0;
        std::uint64_t height = 0;
    };
    for (const stack_region& box : {stack_region{.name = "64", .width = 64, .height = 64},
                                    stack_region{.name = "256", .width = 256, .height = 256},
                                    stack_region{.name = "512", .width = 512, .height = 512},
                                    stack_region{.name = "2048", .width = 2048, .height = 2048},
                                    stack_region{.name = "rows_16", .width = 6000, .height = 16}}) {
        benchmark::RegisterBenchmark("read/u16/stack/" + std::string(box.name) + "/frame", read_stack, box.width,
                                     box.height)
            ->Unit(benchmark::kMillisecond);
    }
    for (const frame_size& size : sizes) {
        for (const unsigned threads : {1U, 4U, 8U, 16U}) {
            const std::string suffix = "/" + std::string(size.name) + "/threads_" + std::to_string(threads);
            benchmark::RegisterBenchmark("threads/read/u16/one_file" + suffix, read_one_file, size, threads)
                ->Unit(benchmark::kMillisecond)
                ->UseRealTime();
            benchmark::RegisterBenchmark("threads/read/u16/many_files" + suffix, read_many_files, size, threads)
                ->Unit(benchmark::kMillisecond)
                ->UseRealTime();
        }
    }
}

void remove_benchmark_files()
{
    std::filesystem::path& path = directory();
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
}

} // namespace openfits::bench
