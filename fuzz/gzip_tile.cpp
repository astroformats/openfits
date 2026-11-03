// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// GZIP_1 and GZIP_2 tiles (standard §10.4.2) on untrusted bytes: the first two bytes give the size of the tile, and the
// rest is a stream, which the decoder inflates into exactly that size or refuses as corrupt data; the input, shuffled
// by values of the size that its first byte chooses, must come back from a stream of the encoder and the unshuffle. A
// failure must be an error of the library's kinds; any other exception escapes and fails the run.

#include <openfits/error.h>

#include "compress/gzip.h"
#include "compress/shuffle.h"

#include <algorithm>
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
    if (size < 2) {
        return 0;
    }
    const std::span<const std::byte> input(reinterpret_cast<const std::byte*>(data), size);
    const std::size_t tile = (std::to_integer<std::size_t>(input[0]) << 8U) | std::to_integer<std::size_t>(input[1]);
    std::vector<std::byte> output(tile);
    try {
        openfits::detail::gzip_decompress(input.subspan(2), output, 1 + (size % 97));
    } catch (const openfits::integrity_error& failure) {
        // A stream that does not inflate into exactly the size of the tile.
        require(failure.code() == openfits::errc::corrupt_compressed_data);
    }

    const std::size_t value_size = std::size_t{1} << (std::to_integer<unsigned>(input[0]) % 4U);
    std::vector<std::byte> shuffled(input.size());
    openfits::detail::shuffle_bytes(input, shuffled, value_size);
    std::vector<std::byte> stream;
    openfits::detail::gzip_encoder(1 + static_cast<int>(size % 9)).compress(shuffled, stream);
    std::vector<std::byte> inflated(input.size());
    openfits::detail::gzip_decompress(stream, inflated);
    std::vector<std::byte> restored(input.size());
    openfits::detail::unshuffle_bytes(inflated, restored, value_size);
    require(std::ranges::equal(restored, input));
    return 0;
}
