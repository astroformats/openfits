// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/openfits.h>

#include "benchmarks.h"

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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
    // The side of the square regions that fill the image.
    std::size_t side = 0;

    [[nodiscard]] std::size_t pixels() const noexcept
    {
        return width * height;
    }
};

constexpr std::array<frame_size, 2> sizes{{{.name = "small", .width = 1024, .height = 1024, .side = 256},
                                           {.name = "frame", .width = 6000, .height = 4000, .side = 500}}};

std::filesystem::path native(const std::string& path)
{
    return {std::u8string(path.begin(), path.end())};
}

// A file made once, and a copy of it that the benchmarks change, made again before each change that needs it.
struct edited_file
{
    std::string original;
    std::string work;

    void restore() const
    {
        std::filesystem::copy_file(native(original), native(work), std::filesystem::copy_options::overwrite_existing);
    }
};

edited_file edited(std::string_view name, writer& output)
{
    edited_file file{.original = file_path(std::string(name) + ".fits"),
                     .work = file_path(std::string(name) + "-work.fits")};
    if (!std::filesystem::exists(native(file.original))) {
        output.save(file.original);
    }
    file.restore();
    return file;
}

// An image reserved by the writer, filled by an editor region by region, as a program writes its output tile by tile,
// and committed: the regions written in place, without a pass over the unit.
void fill_reserved(benchmark::State& state, frame_size size)
{
    writer output;
    image_info info;
    info.geometry.axes = {size.width, size.height};
    info.bitpix = bitpix::int16;
    info.scaling = {.scale = 1.0, .zero = 32768.0};
    (void)output.add_reserved_image(info);
    const edited_file file = edited("reserved-" + std::string(size.name), output);
    const std::vector<std::uint16_t> values = unsigned_sky(size.side, size.side);
    while (state.KeepRunning()) {
        editor changes(file.work);
        for (std::uint64_t y = 0; y + size.side <= size.height; y += size.side) {
            for (std::uint64_t x = 0; x + size.side <= size.width; x += size.side) {
                changes.write_pixels(0, std::span<const std::uint16_t>(values),
                                     {.region = region{.start = {x, y}, .size = {size.side, size.side}}});
            }
        }
        changes.commit();
    }
    count_bytes(state, size.pixels() * 2);
}

// A keyword set in the primary header of the 16-bit image, in place, its blocks rewritten alone.
void update_keyword(benchmark::State& state)
{
    writer output;
    image_info info;
    info.geometry.axes = {sizes[1].width, sizes[1].height};
    const std::vector<std::uint16_t> values = unsigned_sky(sizes[1].width, sizes[1].height);
    (void)output.add_image(info, std::span<const std::uint16_t>(values));
    const edited_file file = edited("keyword-frame", output);
    int turn = 0;
    while (state.KeepRunning()) {
        editor changes(file.work);
        changes.header(0).set("OBSERVER", "observer " + std::to_string(++turn % 10));
        changes.commit();
    }
    state.SetItemsProcessed(state.iterations());
}

// A thousand rows appended to a table of 10,000 rows at the end of its file, in place: the data unit extended and
// NAXIS2 changed. The file is restored before each append, outside the time measured.
void append_rows(benchmark::State& state)
{
    constexpr std::size_t rows = 10'000;
    constexpr std::size_t added = 1'000;
    std::vector<std::int32_t> ids(rows);
    std::vector<double> fluxes(rows);
    for (std::size_t i = 0; i < rows; ++i) {
        ids[i] = static_cast<std::int32_t>(i);
        fluxes[i] = static_cast<double>(i) / 7.0;
    }
    writer output;
    (void)output.add_header({});
    const std::vector<column_values> columns{std::span<const std::int32_t>(ids), std::span<const double>(fluxes)};
    (void)output.add_table(
        table_info{.kind = hdu_kind::bintable, .rows = rows, .columns = {{.name = "ID"}, {.name = "FLUX"}}}, columns);
    const edited_file file = edited("rows", output);
    const std::vector<column_values> more{std::span<const std::int32_t>(ids).first(added),
                                          std::span<const double>(fluxes).first(added)};
    while (state.KeepRunning()) {
        state.PauseTiming();
        file.restore();
        state.ResumeTiming();
        editor changes(file.work);
        changes.append_rows(1, more);
        changes.commit();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(added));
}

// Every value of an image of RICE_1 tiles of a row written by an editor, whose commit decodes, changes and encodes
// every tile again and rewrites the unit through a copy of the file, on threads threads with oneTBB.
void recompress(benchmark::State& state, frame_size size, unsigned threads)
{
    writer output;
    image_info info;
    info.geometry.axes = {size.width, size.height};
    const std::vector<std::uint16_t> values = unsigned_sky(size.width, size.height);
    (void)output.add_image(info, std::span<const std::uint16_t>(values),
                           {.compression = compression_options{.type = compression::rice}});
    const edited_file file = edited("recompress-" + std::string(size.name), output);
    with_threads(threads, [&] {
        while (state.KeepRunning()) {
            editor changes(file.work);
            changes.write_pixels(1, std::span<const std::uint16_t>(values));
            changes.commit();
        }
    });
    count_bytes(state, size.pixels() * 2);
}

} // namespace

void register_edit_benchmarks()
{
    for (const frame_size& size : sizes) {
        const std::string suffix = "/" + std::string(size.name);
        benchmark::RegisterBenchmark("edit/fill_reserved/u16" + suffix, fill_reserved, size)
            ->Unit(benchmark::kMillisecond);
        for (const unsigned threads : {1U, 8U}) {
            if (threads == 1 || parallel_library) {
                benchmark::RegisterBenchmark("edit/recompress/u16_rice" + suffix + "/threads_" +
                                                 std::to_string(threads),
                                             recompress, size, threads)
                    ->Unit(benchmark::kMillisecond)
                    ->UseRealTime();
            }
        }
    }
    benchmark::RegisterBenchmark("edit/update_keyword/frame", update_keyword)->Unit(benchmark::kMillisecond);
    benchmark::RegisterBenchmark("edit/append_rows/thousand", append_rows)->Unit(benchmark::kMillisecond);
}

} // namespace openfits::bench
