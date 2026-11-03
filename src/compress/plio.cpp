// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "compress/plio.h"

#include <openfits/error.h>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

namespace openfits::detail {

namespace {

// The header of a list: words 0, 5 and 6 are 0, word 1 the place of the first instruction, word 2 the marker of the
// header of seven words, and words 3 and 4 the length of the list, header included, in two parts of 15 bits.
constexpr std::size_t header_words = 7;
constexpr std::int16_t new_header = -100;
constexpr std::int64_t length_unit = 32768;

// The instructions of standard Table 38, in the 3 bits above the 12 bits of their data.
constexpr int zero_run = 0;           // ZN: n zeros.
constexpr int set_high = 1;           // SH: the high value, its 12 low bits here and the others in the next word.
constexpr int increment_high = 2;     // IH: the high value plus n.
constexpr int decrement_high = 3;     // DH: the high value minus n.
constexpr int high_run = 4;           // HN: n pixels of the high value.
constexpr int zeros_then_high = 5;    // PN: n - 1 zeros, then a pixel of the high value.
constexpr int increment_and_step = 6; // IS: the high value plus n, then a pixel of it.
constexpr int decrement_and_step = 7; // DS: the high value minus n, then a pixel of it.
constexpr std::int32_t max_data = 4095;

[[noreturn]] void throw_corrupt(const std::string& message)
{
    throw integrity_error(errc::corrupt_compressed_data, message);
}

// The high value of a decoder, which pixels take: CFITSIO's arithmetic on it is undefined beyond 32 bits.
void check_high(std::int64_t high)
{
    if (high < std::numeric_limits<std::int32_t>::min() || high > std::numeric_limits<std::int32_t>::max()) {
        throw_corrupt("the PLIO_1 list sets a value of " + std::to_string(high) + ", beyond 32-bit integers");
    }
}

std::int16_t word(int opcode, std::int32_t data) noexcept
{
    return static_cast<std::int16_t>((opcode << 12) + data);
}

// The words of a run of count pixels of an instruction: 4095 in each, the rest in the last.
void push_run(int opcode, std::uint64_t count, std::vector<std::int16_t>& output)
{
    while (count > 0) {
        const auto chunk = static_cast<std::int32_t>(std::min<std::uint64_t>(count, max_data));
        output.push_back(word(opcode, chunk));
        count -= static_cast<std::uint64_t>(chunk);
    }
}

// The words of a run of length pixels of the value value after zeros zeros, the high value high before it.
void push_group(std::int32_t value, std::uint64_t length, std::uint64_t zeros, std::int32_t& high,
                std::vector<std::int16_t>& output)
{
    const std::int32_t change = value - high;
    if (change != 0) {
        high = value;
        if (std::abs(change) > max_data) {
            // The value itself, never followed by IS or DS.
            output.push_back(word(set_high, value & max_data));
            output.push_back(static_cast<std::int16_t>(value >> 12));
        } else if (length == 1 && zeros == 0) {
            output.push_back(change > 0 ? word(increment_and_step, change) : word(decrement_and_step, -change));
            return;
        } else {
            output.push_back(change > 0 ? word(increment_high, change) : word(decrement_high, -change));
        }
    }
    if (zeros > 0) {
        push_run(zero_run, zeros, output);
        if (length == 1) {
            // The last run of zeros and the pixel in one PN word, which needs room for one more pixel: CFITSIO adds
            // the pixel to a run of 4095 zeros too, which overflows the data into the instruction above it.
            if (output.back() == max_data) {
                output.push_back(word(zeros_then_high, 1));
            } else {
                output.back() = static_cast<std::int16_t>(output.back() + word(zeros_then_high, 1));
            }
            return;
        }
    }
    push_run(high_run, length, output);
}

} // namespace

std::uint64_t plio_max_words(std::uint64_t count) noexcept
{
    return (3 * count) + header_words;
}

void plio_encode(std::span<const std::int32_t> values, std::vector<std::int16_t>& output)
{
    if (values.size() > plio_max_pixels) {
        throw usage_error(errc::invalid_argument, "PLIO_1 compresses at most " + std::to_string(plio_max_pixels) +
                                                      " pixels in a tile, not " + std::to_string(values.size()));
    }
    for (const std::int32_t value : values) {
        if (value < 0 || value > plio_max_value) {
            throw usage_error(errc::invalid_argument,
                              "PLIO_1 compresses the values from 0 to 2^24 - 1, and not " + std::to_string(value));
        }
    }
    const std::size_t start = output.size();
    output.insert(output.end(), {0, static_cast<std::int16_t>(header_words), new_header, 0, 0, 0, 0});
    std::int32_t high = 1;
    std::size_t i = 0;
    while (i < values.size()) {
        const std::size_t zeros_start = i;
        while (i < values.size() && values[i] == 0) {
            ++i;
        }
        const std::uint64_t zeros = i - zeros_start;
        if (i == values.size()) {
            push_run(zero_run, zeros, output);
            break;
        }
        const std::int32_t value = values[i];
        const std::size_t run_start = i;
        while (i < values.size() && values[i] == value) {
            ++i;
        }
        push_group(value, i - run_start, zeros, high, output);
    }
    const auto length = static_cast<std::int64_t>(output.size() - start);
    output[start + 3] = static_cast<std::int16_t>(length % length_unit);
    output[start + 4] = static_cast<std::int16_t>(length / length_unit);
}

void plio_decode(std::span<const std::int16_t> input, std::span<std::int32_t> output)
{
    if (input.size() < 3) {
        throw_corrupt("the PLIO_1 list of " + std::to_string(input.size()) + " words is shorter than its header");
    }
    // The header of three words of old lists, which gives the length in word 2, or that of seven.
    std::int64_t length = input[2];
    std::int64_t first = 3;
    if (length <= 0) {
        if (input.size() < 5) {
            throw_corrupt("the PLIO_1 list of " + std::to_string(input.size()) + " words is shorter than its header");
        }
        length = (std::int64_t{input[4]} * length_unit) + input[3];
        first = input[1];
    }
    if (length <= 0 || first < 0) {
        throw_corrupt("the header of the PLIO_1 list gives a length of " + std::to_string(length) +
                      " words, its instructions from word " + std::to_string(first));
    }

    const std::size_t count = output.size();
    std::size_t x = 0;
    std::int64_t high = 1;
    for (auto i = static_cast<std::size_t>(first); std::cmp_less(i, length) && x < count; ++i) {
        if (i >= input.size()) {
            throw_corrupt("the PLIO_1 list ends at word " + std::to_string(input.size()) + ", before the " +
                          std::to_string(length) + " words of its header");
        }
        if (input[i] < 0) {
            throw_corrupt("word " + std::to_string(i) + " of the PLIO_1 list is no instruction of standard Table 38");
        }
        const int opcode = input[i] >> 12;
        const std::int32_t data = input[i] & max_data;
        switch (opcode) {
        case zero_run:
        case high_run:
        case zeros_then_high: {
            // The run is cut where the tile ends; the last pixel of PN is the high value when the tile holds it.
            const std::size_t end = std::min(x + static_cast<std::size_t>(data), count);
            const std::int32_t fill = opcode == high_run ? static_cast<std::int32_t>(high) : 0;
            std::fill(output.begin() + static_cast<std::ptrdiff_t>(x),
                      output.begin() + static_cast<std::ptrdiff_t>(end), fill);
            if (opcode == zeros_then_high && data > 0 && x + static_cast<std::size_t>(data) <= count) {
                output[x + static_cast<std::size_t>(data) - 1] = static_cast<std::int32_t>(high);
            }
            x += static_cast<std::size_t>(data);
            break;
        }
        case set_high:
            if (i + 1 >= input.size()) {
                throw_corrupt("the PLIO_1 list ends within the instruction SH of word " + std::to_string(i));
            }
            high = (std::int64_t{input[i + 1]} * (max_data + 1)) + data;
            ++i;
            break;
        case increment_high:
            high += data;
            break;
        case decrement_high:
            high -= data;
            break;
        case increment_and_step:
        case decrement_and_step:
            high += opcode == increment_and_step ? data : -data;
            check_high(high);
            output[x] = static_cast<std::int32_t>(high);
            ++x;
            break;
        default:
            break;
        }
        check_high(high);
    }
    if (x < count) {
        std::fill(output.begin() + static_cast<std::ptrdiff_t>(x), output.end(), 0);
    }
}

} // namespace openfits::detail
