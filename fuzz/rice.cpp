// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// RICE_1 (standard §10.4.1) on untrusted bytes: the first byte chooses the bytes of a value (1, 2, 4 or 8) and the
// block size (16 or 32), the next two the number of values, and the rest is a stream, which the decoder decodes or
// refuses as corrupt data; the values it decodes must encode into a stream that decodes to them again. The input read
// as values must also round-trip. A failure must be an error of the library's kinds; any other exception escapes and
// fails the run.

#include "compress/rice.h"

#include <openfits/error.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

namespace {

void require(bool condition)
{
    if (!condition) {
        std::abort();
    }
}

template <typename U> void exercise(std::span<const std::byte> stream, std::size_t count, std::size_t block)
{
    std::vector<U> values(count);
    try {
        openfits::detail::rice_decode(stream, block, std::span<U>(values));
    } catch (const openfits::integrity_error&) {
        values.clear();
    }
    std::vector<std::byte> encoded;
    openfits::detail::rice_encode(std::span<const U>(values), block, encoded);
    std::vector<U> again(values.size());
    openfits::detail::rice_decode(encoded, block, std::span<U>(again));
    require(again == values);

    // The bytes of the stream as values, which must round-trip too.
    std::vector<U> raw(stream.size() / sizeof(U));
    if (!raw.empty()) {
        std::memcpy(raw.data(), stream.data(), raw.size() * sizeof(U));
    }
    encoded.clear();
    openfits::detail::rice_encode(std::span<const U>(raw), block, encoded);
    std::vector<U> back(raw.size());
    openfits::detail::rice_decode(encoded, block, std::span<U>(back));
    require(back == raw);
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
    const auto selector = std::to_integer<unsigned>(input[0]);
    const std::size_t count = (std::to_integer<std::size_t>(input[1]) << 8U) | std::to_integer<std::size_t>(input[2]);
    const std::size_t block = (selector & 4U) != 0 ? 16 : 32;
    const std::span<const std::byte> stream = input.subspan(3);
    switch (selector & 3U) {
    case 0:
        exercise<std::uint8_t>(stream, count, block);
        break;
    case 1:
        exercise<std::uint16_t>(stream, count, block);
        break;
    case 2:
        exercise<std::uint32_t>(stream, count, block);
        break;
    default:
        exercise<std::uint64_t>(stream, count, block);
        break;
    }
    return 0;
}
