// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/io.h>

#include <benchmark/benchmark.h>

#if defined(OPENFITS_WITH_TBB)
#include <oneapi/tbb/task_arena.h>
#endif

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The benchmarks register themselves from main(), so that their names are readable (read/u16/as_float/frame) and no
// macro defines statics. The numbers are what performance decisions rest on, so the data look like an astronomical
// image: a smooth background with a little noise.

namespace openfits::bench {

/// A synthetic sky of width x height pixels, in [0, 1]: a background between about 0.05 and 0.15 with noise of about
/// 0.001. The same arguments give the same values.
[[nodiscard]] std::vector<float> sky(std::size_t width, std::size_t height);

/// The sky of sky() as unsigned 16-bit values, from 0 to 65535.
[[nodiscard]] std::vector<std::uint16_t> unsigned_sky(std::size_t width, std::size_t height);

/// The UTF-8 path of the file name in the directory of the benchmarks, which the first call makes in the temporary
/// directory of the system.
[[nodiscard]] std::string file_path(std::string_view name);

/// A sink that keeps no bytes, so that a write measures the conversion and the header, not the growth of a buffer. It
/// takes rewrites, as a file does, so that a write with checksums completes its headers in place.
class discarding_sink final : public output_sink
{
public:
    void write(std::span<const std::byte> data) override
    {
        benchmark::DoNotOptimize(data.data());
        position_ += data.size();
    }

    std::uint64_t position() const override
    {
        return position_;
    }

    bool can_rewrite() const override
    {
        return true;
    }

    void rewrite(std::uint64_t /*offset*/, std::span<const std::byte> data) override
    {
        benchmark::DoNotOptimize(data.data());
    }

private:
    std::uint64_t position_ = 0;
};

/// Counts size bytes for each iteration of state.
inline void count_bytes(benchmark::State& state, std::uint64_t size)
{
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(size));
}

/// True when the library runs parallel work on the threads of oneTBB, which with_threads() then counts.
inline constexpr bool parallel_library =
#if defined(OPENFITS_WITH_TBB)
    true;
#else
    false;
#endif

/// Calls function in a task arena of threads threads with oneTBB, so that the parallel work of the library takes that
/// many, the calling thread among them; without oneTBB, on the calling thread alone.
template <typename Function> void with_threads(unsigned threads, Function&& function)
{
#if defined(OPENFITS_WITH_TBB)
    oneapi::tbb::task_arena arena(static_cast<int>(threads));
    arena.execute(std::forward<Function>(function));
#else
    (void)threads;
    std::forward<Function>(function)();
#endif
}

/// Images read into memory of the application and written: 16-bit images of the unsigned convention read as themselves
/// and as float, floating-point images, scaled 16-bit images read as float, the region of a stacking read, and the
/// writes of each, at 1024 x 1024 pixels (small) and at the 6000 x 4000 of a camera (frame), whose files the program
/// writes once into a directory of its own in the temporary directory of the system and removes at its end; the
/// checksums of a 16-bit image, verified, verified in a read of the whole image, and written; and the walk of a file of
/// a thousand units.
void register_image_benchmarks();

/// Tables read into memory of the application and written, at 10 000 rows (small) and at a million (million): a
/// column of each type of numbers of a catalogue read as double, and as its own type, the six in one pass, its names as
/// strings, the variable-length arrays of a column of spectra, a column of an ASCII table, and the writes of the
/// catalogue and of the spectra. The files lie beside those of the images.
void register_table_benchmarks();

/// Tiled compressed images (standard §10), at the sizes of the images: 16-bit images with RICE_1 in tiles of one row
/// and of 16 rows, GZIP_2 and HCOMPRESS_1, and floating-point images quantized with RICE_1 and kept lossless with
/// GZIP_2, each read whole and written; and the image of tiles of 16 rows read row by row, without a tile cache and
/// with one.
void register_compression_benchmarks();

/// Each codec of tiled compression in tiles of a row, of 16 rows and of 256 x 256 pixels, for each type of values it
/// takes, decoded and encoded, on one thread and with oneTBB on eight, at 1024 x 1024 pixels; and the frame of 100 MB
/// of RICE_1 tiles of a row on 1, 4, 8 and 16 threads.
void register_codec_benchmarks();

/// The editor: an image reserved by the writer filled region by region, a keyword set in place, a thousand rows
/// appended to a table in place, and every value of an image of RICE_1 tiles written again, on one thread and with
/// oneTBB on eight.
void register_edit_benchmarks();

/// The kernels alone, through the internal code, each kernel with SIMD forms in each of them: the byte swap of items
/// of 2, 4 and 8 bytes, the shuffle of GZIP_2 and its reverse, the checksum, and the conversion of 16-bit values into
/// float in one pass and in two, over blocks of 32 KiB, 256 KiB and 4 MiB; and the parse of a record of each kind of
/// value.
void register_kernel_benchmarks();

/// Removes the files of the benchmarks.
void remove_benchmark_files();

} // namespace openfits::bench
