// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// HCOMPRESS_1 (standard §10.4.4) on untrusted bytes: the first two bytes give the rows and the length of the rows of a
// tile, and the rest is a stream, which the decoders of 32 and 64 bits decode into exactly the tile or refuse as
// corrupt data, with and without smoothing; values taken from the input must come back exactly at scale 0, and within
// the scale above it. A failure must be an error of the library's kinds; any other exception escapes and fails the
// run.

#include "compress/hcompress.h"

#include <openfits/error.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

namespace {

void require(bool condition)
{
    if (!condition) {
        std::abort();
    }
}

} // namespace

// The entry point that libFuzzer, and the replay driver, call for every input. Its name is fixed by libFuzzer.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 3) {
        return 0;
    }
    const std::span<const std::byte> input(reinterpret_cast<const std::byte*>(data), size);
    const std::size_t nx = 1 + (std::to_integer<std::size_t>(input[0]) % 64);
    const std::size_t ny = 1 + (std::to_integer<std::size_t>(input[1]) % 64);
    const bool smooth = (std::to_integer<unsigned>(input[2]) & 1U) != 0;
    const std::span<const std::byte> stream = input.subspan(3);
    std::vector<std::int32_t> narrow(nx * ny);
    std::vector<std::int64_t> wide(nx * ny);
    try {
        openfits::detail::hcompress_decode(stream, nx, ny, smooth, std::span<std::int32_t>(narrow));
    } catch (const openfits::integrity_error& failure) {
        // A stream that does not decode into exactly the tile.
        require(failure.code() == openfits::errc::corrupt_compressed_data);
    }
    try {
        openfits::detail::hcompress_decode(stream, nx, ny, smooth, std::span<std::int64_t>(wide));
    } catch (const openfits::integrity_error& failure) {
        require(failure.code() == openfits::errc::corrupt_compressed_data);
    }

    // The bytes of the input, the three of the parameters included, so that a stream of none gives values too, as
    // signed values of a tile, at scale 0 and at a scale of the third byte.
    std::vector<std::int32_t> values(nx * ny);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = static_cast<std::int32_t>(std::to_integer<std::int8_t>(input[i % size])) * 37;
    }
    std::vector<std::byte> encoded;
    openfits::detail::hcompress_encode(values, nx, ny, 0, encoded);
    std::vector<std::int32_t> back(values.size());
    openfits::detail::hcompress_decode(encoded, nx, ny, false, std::span<std::int32_t>(back));
    require(back == values);
    const std::int64_t scale = 2 + (std::to_integer<std::int64_t>(input[2]) % 30);
    encoded.clear();
    openfits::detail::hcompress_encode(values, nx, ny, scale, encoded);
    openfits::detail::hcompress_decode(encoded, nx, ny, smooth, std::span<std::int32_t>(back));
    return 0;
}
