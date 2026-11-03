// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// PLIO_1 (standard §10.4.3) on untrusted bytes: the first two bytes give the pixels of a tile, and the rest are the
// big-endian words of a line list, which the decoder decodes into exactly the tile or refuses as corrupt data; values
// within 0 to 2^24 - 1 that it decodes must encode into a list of no more words than the worst case that decodes to
// them again, and so must values taken from the input. A failure must be an error of the library's kinds; any other
// exception escapes and fails the run.

#include "compress/plio.h"

#include <openfits/error.h>

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

void round_trip(const std::vector<std::int32_t>& values)
{
    std::vector<std::int16_t> words;
    openfits::detail::plio_encode(values, words);
    require(words.size() <= openfits::detail::plio_max_words(values.size()));
    std::vector<std::int32_t> back(values.size(), -1);
    openfits::detail::plio_decode(words, back);
    require(back == values);
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
    const std::size_t pixels = (std::to_integer<std::size_t>(input[0]) << 8U) | std::to_integer<std::size_t>(input[1]);
    std::vector<std::int16_t> words((size - 2) / 2);
    for (std::size_t i = 0; i < words.size(); ++i) {
        const auto high = std::to_integer<std::uint16_t>(input[2 + (2 * i)]);
        const auto low = std::to_integer<std::uint16_t>(input[3 + (2 * i)]);
        words[i] = static_cast<std::int16_t>(static_cast<std::uint16_t>((high << 8U) | low));
    }
    std::vector<std::int32_t> decoded(pixels);
    bool valid = true;
    try {
        openfits::detail::plio_decode(words, decoded);
    } catch (const openfits::integrity_error&) {
        valid = false;
    }
    if (valid &&
        std::ranges::all_of(decoded, [](std::int32_t v) { return v >= 0 && v <= openfits::detail::plio_max_value; })) {
        round_trip(decoded);
    }

    // The input as values of up to 24 bits, with long runs where its bytes repeat.
    std::vector<std::int32_t> values;
    for (std::size_t i = 2; i + 3 <= size && values.size() < 100'000; i += 3) {
        const std::int32_t value = (std::to_integer<std::int32_t>(input[i]) << 16) |
                                   (std::to_integer<std::int32_t>(input[i + 1]) << 8) |
                                   std::to_integer<std::int32_t>(input[i + 2]);
        const std::size_t repeat = (value & 0x1F) == 0 ? 4096 + static_cast<std::size_t>(value & 0x0FFF) : 1;
        values.insert(values.end(), repeat, (value & 0x30) == 0 ? 0 : value);
    }
    round_trip(values);
    return 0;
}
