// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Compressed tables (standard §10.3) on untrusted bytes: the input read as a file under small limits, every column of
// each compressed table read as strings or as arrays, which must give values or an error of the library's kinds; and a
// table of the input written with an algorithm and tiles that its first bytes choose, which must read back as written.
// Any other exception escapes and fails the run.

#include <openfits/compression.h>
#include <openfits/error.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/table.h>
#include <openfits/writer.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
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
    options.limits.max_heap_bytes = std::uint64_t{4} << 20;
    options.limits.max_vla_elements = std::uint64_t{1} << 16;
    options.limits.max_diagnostics = 32;
    return options;
}

void read_file(std::span<const std::byte> input)
{
    try {
        const openfits::reader file(std::make_unique<openfits::memory_source>(input), small_limits());
        for (std::size_t i = 0; i < file.hdu_count(); ++i) {
            if (file.hdu(i).kind() != openfits::hdu_kind::compressed_table) {
                continue;
            }
            try {
                const openfits::table_info table = file.table(i);
                for (std::size_t c = 0; c < table.columns.size() && c < 16; ++c) {
                    try {
                        (void)file.read_strings(i, c);
                    } catch (const openfits::error&) {
                        (void)file.read_arrays<double>(i, c);
                    }
                }
            } catch (const openfits::error&) {
                // A compressed table whose keywords, tiles or columns do not read.
                continue;
            }
        }
    } catch (const openfits::error&) {
        // Not a FITS file, or one beyond the limits.
        return;
    }
}

openfits::compression algorithm_of(unsigned selector) noexcept
{
    switch (selector % 5U) {
    case 0:
        return openfits::compression::automatic;
    case 1:
        return openfits::compression::rice;
    case 2:
        return openfits::compression::gzip_1;
    case 3:
        return openfits::compression::gzip_2;
    default:
        return openfits::compression::nocompress;
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
    // A table of rows of an integer, a real, a byte and an array of 16-bit integers of the input.
    const std::span<const std::byte> payload = input.subspan(4, std::min<std::size_t>(size - 4, 1 << 14));
    const std::size_t rows = payload.size() / 16;
    std::vector<std::int32_t> integers(rows);
    std::vector<double> reals(rows);
    std::vector<std::uint8_t> bytes(rows);
    std::vector<std::int16_t> elements;
    std::vector<std::uint64_t> offsets{0};
    for (std::size_t r = 0; r < rows; ++r) {
        std::memcpy(&integers[r], payload.data() + (r * 16), 4);
        std::int64_t whole = 0;
        std::memcpy(&whole, payload.data() + (r * 16) + 4, 8);
        reals[r] = static_cast<double>(whole % 1'000'000) / 64.0;
        bytes[r] = std::to_integer<std::uint8_t>(payload[(r * 16) + 12]);
        const std::size_t length = std::to_integer<std::size_t>(payload[(r * 16) + 13]) % 6;
        for (std::size_t k = 0; k < length; ++k) {
            elements.push_back(
                static_cast<std::int16_t>(std::to_integer<int>(payload[(r * 16) + 14]) * static_cast<int>(k)));
        }
        offsets.push_back(elements.size());
    }
    openfits::table_info info;
    info.kind = openfits::hdu_kind::bintable;
    info.rows = rows;
    info.columns = {openfits::column_info{.name = "I"}, openfits::column_info{.name = "R"},
                    openfits::column_info{.name = "B"}, openfits::column_info{.name = "A"}};
    openfits::compression_options options;
    options.type = algorithm_of(std::to_integer<unsigned>(input[0]));
    options.tile_rows = std::to_integer<std::uint64_t>(input[1]) % 17;
    options.columns = {algorithm_of(std::to_integer<unsigned>(input[2])), openfits::compression::automatic,
                       openfits::compression::automatic, algorithm_of(std::to_integer<unsigned>(input[3]))};
    openfits::writer output;
    (void)output.add_table(info,
                           std::vector<openfits::column_values>{
                               std::span<const std::int32_t>(integers), std::span<const double>(reals),
                               std::span<const std::uint8_t>(bytes),
                               openfits::column_values::arrays(std::span<const std::int16_t>(elements), offsets)},
                           {.compression = options});
    openfits::memory_sink sink;
    try {
        output.save(sink);
    } catch (const openfits::validation_error&) {
        // An algorithm of the options that a column cannot take.
        return 0;
    }
    const openfits::reader file(std::make_unique<openfits::memory_source>(sink.release()));
    require(file.read_column<std::int32_t>(1, 0) == integers);
    require(file.read_column<double>(1, 1) == reals);
    require(file.read_column<std::uint8_t>(1, 2) == bytes);
    const openfits::column_arrays<std::int16_t> arrays = file.read_arrays<std::int16_t>(1, 3);
    require(arrays.values == elements);
    require(arrays.offsets == offsets);
    return 0;
}
