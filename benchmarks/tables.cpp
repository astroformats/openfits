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
#include <utility>
#include <vector>

namespace openfits::bench {

namespace {

struct table_size
{
    std::string_view name;
    std::size_t rows = 0;
};

constexpr std::array<table_size, 2> sizes{table_size{.name = "small", .rows = 10'000},
                                          table_size{.name = "million", .rows = 1'000'000}};

// The bytes of a row of the catalogue: a column of each type of numbers of standard Table 18, B, I, J, K, E and D, in
// that order, and a name of 16 characters.
constexpr std::array<std::uint64_t, 6> number_sizes{1, 2, 4, 8, 4, 8};
constexpr std::uint64_t numbers_size = 1 + 2 + 4 + 8 + 4 + 8;
constexpr std::uint64_t name_size = 16;

struct catalogue_columns
{
    std::vector<std::uint8_t> bytes;
    std::vector<std::int16_t> shorts;
    std::vector<std::int32_t> ints;
    std::vector<std::int64_t> longs;
    std::vector<float> floats;
    std::vector<double> doubles;
    std::vector<std::string> names;
};

catalogue_columns catalogue_values(std::size_t rows)
{
    catalogue_columns values;
    const std::vector<float> noise = sky(1000, rows / 1000);
    values.bytes.resize(rows);
    values.shorts.resize(rows);
    values.ints.resize(rows);
    values.longs.resize(rows);
    values.floats.resize(rows);
    values.doubles.resize(rows);
    values.names.resize(rows);
    for (std::size_t i = 0; i < rows; ++i) {
        values.bytes[i] = static_cast<std::uint8_t>(i % 251);
        values.shorts[i] = static_cast<std::int16_t>(static_cast<int>(i % 65536) - 32768);
        values.ints[i] = static_cast<std::int32_t>(static_cast<std::uint32_t>(i) * 2654435761U);
        values.longs[i] = static_cast<std::int64_t>(i) * 1'000'003;
        values.floats[i] = noise[i];
        values.doubles[i] = static_cast<double>(noise[i]) * 360.0;
        values.names[i] = "star " + std::to_string(i);
    }
    return values;
}

table_info catalogue_model(std::size_t rows)
{
    return table_info{.kind = hdu_kind::bintable,
                      .rows = rows,
                      .columns = {{.name = "B"},
                                  {.name = "I"},
                                  {.name = "J"},
                                  {.name = "K"},
                                  {.name = "E"},
                                  {.name = "D"},
                                  {.name = "NAME", .format = column_format::parse_binary("16A")}}};
}

std::vector<column_values> catalogue_data(const catalogue_columns& values)
{
    return {std::span(values.bytes),  std::span(values.shorts),  std::span(values.ints), std::span(values.longs),
            std::span(values.floats), std::span(values.doubles), std::span(values.names)};
}

// The arrays of the table of variable-length arrays: row r holds r mod 21 doubles, 10 on average.
struct arrays_columns
{
    std::vector<double> values;
    std::vector<std::uint64_t> offsets;
};

arrays_columns arrays_values(std::size_t rows)
{
    arrays_columns arrays;
    arrays.offsets.reserve(rows + 1);
    arrays.offsets.push_back(0);
    for (std::size_t row = 0; row < rows; ++row) {
        for (std::size_t i = 0; i < row % 21; ++i) {
            arrays.values.push_back(static_cast<double>(row) + (static_cast<double>(i) / 32.0));
        }
        arrays.offsets.push_back(arrays.values.size());
    }
    return arrays;
}

// The files of the benchmarks, written with the library the first time each is asked for: the catalogue, the table of
// variable-length arrays, and an ASCII table of a count and a flux, at each size.
enum class table_kind
{
    catalogue,
    arrays,
    ascii
};

const std::string& file_of(table_kind kind, const table_size& size)
{
    static std::map<std::string, std::string> files;
    constexpr std::array<std::string_view, 3> kinds{"catalogue", "arrays", "ascii"};
    const std::string name =
        std::string(kinds.at(static_cast<std::size_t>(kind))) + "-" + std::string(size.name) + ".fits";
    const auto found = files.find(name);
    if (found != files.end()) {
        return found->second;
    }
    std::string path = file_path(name);
    writer output;
    switch (kind) {
    case table_kind::catalogue: {
        const catalogue_columns values = catalogue_values(size.rows);
        (void)output.add_table(catalogue_model(size.rows), catalogue_data(values));
        output.save(path);
        break;
    }
    case table_kind::arrays: {
        const arrays_columns arrays = arrays_values(size.rows);
        (void)output.add_table(
            table_info{.kind = hdu_kind::bintable, .rows = size.rows, .columns = {{.name = "SPECTRUM"}}},
            std::vector<column_values>{column_values::arrays(std::span(arrays.values), arrays.offsets)});
        output.save(path);
        break;
    }
    case table_kind::ascii: {
        std::vector<std::int32_t> counts(size.rows);
        std::vector<double> fluxes(size.rows);
        for (std::size_t row = 0; row < size.rows; ++row) {
            counts[row] = static_cast<std::int32_t>(row * 7);
            fluxes[row] = static_cast<double>(row) / 3.0;
        }
        (void)output.add_table(
            table_info{.kind = hdu_kind::table,
                       .rows = size.rows,
                       .columns = {{.name = "COUNT"}, {.name = "FLUX", .format = column_format::parse_ascii("F14.6")}}},
            std::vector<column_values>{std::span(counts), std::span(fluxes)});
        output.save(path);
        break;
    }
    }
    return files.emplace(name, std::move(path)).first->second;
}

// A read of a column of numbers of the catalogue into memory of the application, as values of T: the bytes counted are
// those stored.
template <typename T> void read_column(benchmark::State& state, std::size_t column, table_size size)
{
    const reader file(file_of(table_kind::catalogue, size));
    std::vector<T> destination(size.rows);
    while (state.KeepRunning()) {
        (void)file.read_column(1, column, std::span<T>(destination));
        benchmark::DoNotOptimize(destination.data());
        benchmark::ClobberMemory();
    }
    count_bytes(state, size.rows * number_sizes.at(column));
}

// The six columns of numbers of the catalogue as double, in one pass over the rows.
void read_columns(benchmark::State& state, table_size size)
{
    const reader file(file_of(table_kind::catalogue, size));
    std::array<std::vector<double>, 6> destinations;
    std::vector<column_destination> targets;
    for (std::size_t c = 0; c < destinations.size(); ++c) {
        destinations[c].resize(size.rows);
        targets.push_back(column_destination::of(c, std::span<double>(destinations[c])));
    }
    while (state.KeepRunning()) {
        (void)file.read_columns(1, targets);
        benchmark::DoNotOptimize(destinations[0].data());
        benchmark::ClobberMemory();
    }
    count_bytes(state, size.rows * numbers_size);
}

void read_strings(benchmark::State& state, table_size size)
{
    const reader file(file_of(table_kind::catalogue, size));
    while (state.KeepRunning()) {
        std::vector<std::string> names = file.read_strings(1, 6, {.trim_strings = true});
        benchmark::DoNotOptimize(names.data());
    }
    count_bytes(state, size.rows * name_size);
}

// The arrays of every row, with their descriptors: the bytes counted are those of the descriptors and of the heap.
void read_arrays(benchmark::State& state, table_size size)
{
    const reader file(file_of(table_kind::arrays, size));
    const table_info table = file.table(1);
    while (state.KeepRunning()) {
        column_arrays<double> arrays = file.read_arrays<double>(1, 0);
        benchmark::DoNotOptimize(arrays.values.data());
    }
    count_bytes(state, (size.rows * table.row_size) + table.heap_size);
}

void read_ascii(benchmark::State& state, table_size size)
{
    const reader file(file_of(table_kind::ascii, size));
    std::vector<double> destination(size.rows);
    while (state.KeepRunning()) {
        (void)file.read_column(1, 1, std::span<double>(destination));
        benchmark::DoNotOptimize(destination.data());
        benchmark::ClobberMemory();
    }
    // The 14 characters of each field of F14.6.
    count_bytes(state, size.rows * 14);
}

void write_catalogue(benchmark::State& state, table_size size)
{
    const catalogue_columns values = catalogue_values(size.rows);
    const table_info model = catalogue_model(size.rows);
    const std::vector<column_values> data = catalogue_data(values);
    while (state.KeepRunning()) {
        writer output;
        (void)output.add_table(model, data);
        discarding_sink sink;
        output.save(sink);
    }
    count_bytes(state, size.rows * (numbers_size + name_size));
}

void write_arrays(benchmark::State& state, table_size size)
{
    const arrays_columns arrays = arrays_values(size.rows);
    const table_info model{.kind = hdu_kind::bintable, .rows = size.rows, .columns = {{.name = "SPECTRUM"}}};
    const std::vector<column_values> data{column_values::arrays(std::span(arrays.values), arrays.offsets)};
    while (state.KeepRunning()) {
        writer output;
        (void)output.add_table(model, data);
        discarding_sink sink;
        output.save(sink);
    }
    count_bytes(state, (size.rows * 8) + (arrays.values.size() * sizeof(double)));
}

} // namespace

void register_table_benchmarks()
{
    constexpr std::array<std::string_view, 6> types{"B", "I", "J", "K", "E", "D"};
    for (const table_size& size : sizes) {
        const std::string suffix = "/" + std::string(size.name);
        for (std::size_t c = 0; c < types.size(); ++c) {
            benchmark::RegisterBenchmark("table/read/" + std::string(types.at(c)) + "/as_double" + suffix,
                                         read_column<double>, c, size)
                ->Unit(benchmark::kMillisecond);
        }
        benchmark::RegisterBenchmark("table/read/I/as_int16" + suffix, read_column<std::int16_t>, 1, size)
            ->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark("table/read/E/as_float" + suffix, read_column<float>, 4, size)
            ->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark("table/read/six_columns" + suffix, read_columns, size)
            ->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark("table/read/strings" + suffix, read_strings, size)->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark("table/read/arrays" + suffix, read_arrays, size)->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark("table/read/ascii" + suffix, read_ascii, size)->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark("table/write/catalogue" + suffix, write_catalogue, size)
            ->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark("table/write/arrays" + suffix, write_arrays, size)->Unit(benchmark::kMillisecond);
    }
}

} // namespace openfits::bench
