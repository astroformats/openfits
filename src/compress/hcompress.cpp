// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "compress/hcompress.h"

#include <openfits/error.h>

#include "core/endian.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace openfits::detail {

namespace {

// The stream begins with the bytes 0xDD 0x99, then the number of rows, the row length and the scale in 4 bytes each,
// the first coefficient in 8, all big-endian and signed, and the number of bit planes of the three groups of
// quadrants in a byte each.
constexpr std::size_t header_size = 25;
constexpr std::size_t planes_offset = 22;

// The decoder takes at most 63 planes, the magnitudes of 64-bit coefficients.
constexpr unsigned max_planes = 63;
constexpr std::size_t max_side = std::numeric_limits<std::int32_t>::max();
constexpr std::int64_t max_scale = std::numeric_limits<std::int32_t>::max();

// The bits of the coefficients that the encoder writes, losslessly and at a scale above 1 (hcompress_max_magnitude()).
constexpr unsigned lossless_bits = 60;
constexpr unsigned lossy_bits = 53;

[[noreturn]] void throw_corrupt(const std::string& message)
{
    throw integrity_error(errc::corrupt_compressed_data, message);
}

[[noreturn]] void throw_usage(const std::string& message)
{
    throw usage_error(errc::invalid_argument, message);
}

// The code words of the 16 codes of four cells, the bits of a code most significant first: a complete prefix code whose
// words of 3 bits are the codes of a single cell, the most frequent, and whose code 0 appears only for a plane of
// nothing.
struct code_word
{
    unsigned bits = 0;
    unsigned length = 0;
};

constexpr std::array<code_word, 16> code_words{{
    {.bits = 0x3E, .length = 6},
    {.bits = 0x00, .length = 3},
    {.bits = 0x01, .length = 3},
    {.bits = 0x08, .length = 4},
    {.bits = 0x02, .length = 3},
    {.bits = 0x09, .length = 4},
    {.bits = 0x1A, .length = 5},
    {.bits = 0x1B, .length = 5},
    {.bits = 0x03, .length = 3},
    {.bits = 0x1C, .length = 5},
    {.bits = 0x0A, .length = 4},
    {.bits = 0x1D, .length = 5},
    {.bits = 0x0B, .length = 4},
    {.bits = 0x1E, .length = 5},
    {.bits = 0x3F, .length = 6},
    {.bits = 0x0C, .length = 4},
}};

constexpr unsigned longest_code = 6;

// The code and the length of the word that each 6 bits start with: the code is complete, so that every 6 bits start
// with a word.
struct decoded_word
{
    unsigned code = 0;
    unsigned length = 0;
};

constexpr std::array<decoded_word, std::size_t{1} << longest_code> decoding_table = [] {
    std::array<decoded_word, std::size_t{1} << longest_code> table{};
    for (unsigned code = 0; code < code_words.size(); ++code) {
        const unsigned free_bits = longest_code - code_words[code].length;
        const unsigned first = code_words[code].bits << free_bits;
        for (unsigned suffix = 0; suffix < (1U << free_bits); ++suffix) {
            table[first + suffix] = {.code = code, .length = code_words[code].length};
        }
    }
    return table;
}();

// The bits of the code words of the codes, but 0 for the code 0, whose word appears only for a plane of nothing.
constexpr std::array<unsigned, 16> marked_lengths = [] {
    std::array<unsigned, 16> lengths{};
    for (std::size_t code = 1; code < lengths.size(); ++code) {
        lengths[code] = code_words[code].length;
    }
    return lengths;
}();

// Bits written most significant first, the last byte filled with zero bits, appended to output 32 at a time. The
// output grows as needed, and ends at the last byte written once flushed. The loops over many codes keep the state in
// locals, where it stays in registers, which the bytes they write cannot change.
class bit_writer
{
public:
    explicit bit_writer(std::vector<std::byte>& output) : output_(output), used_(output.size()) {}

    // The low bits of value, bits at most 32.
    void put(std::uint64_t value, unsigned bits)
    {
        buffer_ = (buffer_ << bits) | (value & ((std::uint64_t{1} << bits) - 1));
        count_ += bits;
        if (count_ >= 32) {
            count_ -= 32;
            ensure(4);
            store_big_endian(std::span(output_).subspan(used_).first<4>(),
                             static_cast<std::uint32_t>(buffer_ >> count_));
            used_ += 4;
        }
    }

    void put_code(unsigned code)
    {
        put(code_words[code].bits, code_words[code].length);
    }

    // Each code of codes in a nybble, in their order.
    void put_nybbles(std::span<const std::uint8_t> codes)
    {
        ensure((codes.size() / 2) + 4);
        std::uint64_t buffer = buffer_;
        unsigned count = count_;
        std::size_t used = used_;
        std::byte* const out = output_.data();
        for (const std::uint8_t code : codes) {
            buffer = (buffer << 4U) | (code & 0xFU);
            count += 4;
            if (count >= 32) {
                count -= 32;
                store_big_endian(std::span<std::byte, 4>(out + used, 4), static_cast<std::uint32_t>(buffer >> count));
                used += 4;
            }
        }
        buffer_ = buffer;
        count_ = count;
        used_ = used;
    }

    // The code words of the codes of codes that are not 0, from the last to the first.
    void put_marked_codes(std::span<const std::uint8_t> codes)
    {
        ensure(((codes.size() * longest_code) / 8) + 4);
        std::uint64_t buffer = buffer_;
        unsigned count = count_;
        std::size_t used = used_;
        std::byte* const out = output_.data();
        for (std::size_t k = codes.size(); k-- > 0;) {
            const unsigned code = codes[k];
            if (code == 0) {
                continue;
            }
            buffer = (buffer << code_words[code].length) | code_words[code].bits;
            count += code_words[code].length;
            if (count >= 32) {
                count -= 32;
                store_big_endian(std::span<std::byte, 4>(out + used, 4), static_cast<std::uint32_t>(buffer >> count));
                used += 4;
            }
        }
        buffer_ = buffer;
        count_ = count;
        used_ = used;
    }

    // Writes the bits held, the last byte filled with zero bits, and ends output after them.
    void flush()
    {
        if (count_ % 8 != 0) {
            put(0, 8 - (count_ % 8));
        }
        ensure(count_ / 8);
        for (; count_ > 0; count_ -= 8) {
            output_[used_] = static_cast<std::byte>((buffer_ >> (count_ - 8)) & 0xFFU);
            ++used_;
        }
        output_.resize(used_);
    }

private:
    void ensure(std::size_t bytes)
    {
        if (output_.size() - used_ < bytes) {
            output_.resize(std::max(used_ + bytes, output_.size() * 2));
        }
    }

    std::vector<std::byte>& output_;
    std::size_t used_;
    // The bits not yet written, the last count_ of them, fewer than 32 between calls.
    std::uint64_t buffer_ = 0;
    unsigned count_ = 0;
};

[[noreturn]] void throw_end()
{
    throw_corrupt("the HCOMPRESS_1 data end before the values of their tile");
}

// Bits read most significant first from a span, never beyond it. The bits held are at the top of a 64-bit word, from
// its most significant bit: the first count_ of them are those not yet taken, and the bits after them, when not 0, are
// those of the bytes that follow, which a load of a whole word brought in early; bits beyond the input are 0. Bytes are
// taken whole, a word at a time while the input has 8 bytes left. The loops over many codes or bits keep the state in
// locals, where it stays in registers, and the codes they write cannot change it.
class bit_reader
{
public:
    explicit bit_reader(std::span<const std::byte> input) noexcept : input_(input) {}

    // The next bits bits, at most 32.
    unsigned get(unsigned bits)
    {
        refill();
        if (count_ < bits) {
            throw_end();
        }
        const auto value = static_cast<unsigned>((buffer_ >> 1U) >> (63 - bits));
        buffer_ <<= bits;
        count_ -= bits;
        return value;
    }

    // The code of the next code word.
    unsigned get_code()
    {
        refill();
        // The next 6 bits, those beyond the input 0: the word that they start with must lie within it.
        const decoded_word word = decoding_table[buffer_ >> (64 - longest_code)];
        if (count_ < word.length) {
            throw_end();
        }
        buffer_ <<= word.length;
        count_ -= word.length;
        return word.code;
    }

    // Replaces each mark of marks that is not 0, from the last to the first, by the code of the next code word, as
    // get_code() reads it.
    void get_codes(std::span<std::uint8_t> marks)
    {
        std::uint64_t buffer = buffer_;
        unsigned count = count_;
        for (std::size_t k = marks.size(); k-- > 0;) {
            if (marks[k] == 0) {
                continue;
            }
            if (count < longest_code) {
                buffer_ = buffer;
                count_ = count;
                refill();
                buffer = buffer_;
                count = count_;
            }
            const decoded_word word = decoding_table[buffer >> (64 - longest_code)];
            if (count < word.length) {
                throw_end();
            }
            buffer <<= word.length;
            count -= word.length;
            marks[k] = static_cast<std::uint8_t>(word.code);
        }
        buffer_ = buffer;
        count_ = count;
    }

    // The nybbles of codes, one after the other, as get(4) reads them.
    void get_nybbles(std::span<std::uint8_t> codes)
    {
        std::uint64_t buffer = buffer_;
        unsigned count = count_;
        for (std::uint8_t& code : codes) {
            if (count < 4) {
                buffer_ = buffer;
                count_ = count;
                refill();
                buffer = buffer_;
                count = count_;
                if (count < 4) {
                    throw_end();
                }
            }
            code = static_cast<std::uint8_t>(buffer >> 60U);
            buffer <<= 4U;
            count -= 4;
        }
        buffer_ = buffer;
        count_ = count;
    }

    // Negates each value of x that is not 0 whose next bit is 1, as a loop of get(1) does.
    void negate_signed(std::span<std::int64_t> x)
    {
        std::uint64_t buffer = buffer_;
        unsigned count = count_;
        for (std::int64_t& value : x) {
            if (count == 0) {
                buffer_ = buffer;
                count_ = count;
                refill();
                buffer = buffer_;
                count = count_;
                if (count == 0 && value != 0) {
                    throw_end();
                }
            }
            // Without a branch on the bit, which half of the values would mispredict.
            const unsigned taken = value != 0 ? 1U : 0U;
            const std::uint64_t negative = (buffer >> 63U) & taken;
            buffer <<= taken;
            count -= taken;
            value = static_cast<std::int64_t>((static_cast<std::uint64_t>(value) ^ (0 - negative)) + negative);
        }
        buffer_ = buffer;
        count_ = count;
    }

    // Leaves the bits of the byte read in part.
    void align() noexcept
    {
        buffer_ <<= count_ % 8;
        count_ -= count_ % 8;
    }

private:
    // Holds at least 56 bits, or every bit left; at most 63.
    void refill() noexcept
    {
        if (input_.size() - next_ >= 8) {
            buffer_ |= load_big_endian<std::uint64_t>(input_.subspan(next_).first<8>()) >> count_;
            next_ += (63 - count_) / 8;
            count_ |= 56U;
            return;
        }
        while (count_ <= 55 && next_ < input_.size()) {
            buffer_ |= std::to_integer<std::uint64_t>(input_[next_]) << (56 - count_);
            ++next_;
            count_ += 8;
        }
    }

    std::span<const std::byte> input_;
    std::size_t next_ = 0;
    std::uint64_t buffer_ = 0;
    unsigned count_ = 0;
};

void put_big_endian(std::vector<std::byte>& output, std::uint64_t value, unsigned bytes)
{
    for (unsigned i = bytes; i-- > 0;) {
        output.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFFU));
    }
}

std::uint64_t get_big_endian(std::span<const std::byte> input, std::size_t offset, std::size_t bytes) noexcept
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < bytes; ++i) {
        value = (value << 8U) | std::to_integer<std::uint64_t>(input[offset + i]);
    }
    return value;
}

std::int64_t signed_32(std::uint64_t value) noexcept
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(value));
}

std::uint64_t magnitude_of(std::int64_t value) noexcept
{
    const auto bits = static_cast<std::uint64_t>(value);
    return value < 0 ? 0 - bits : bits;
}

// The first value of x of a magnitude beyond limit, if any: found from the extremes of x, without a branch on the
// signs, which are those of noise.
std::optional<std::int64_t> beyond(std::span<const std::int64_t> x, std::uint64_t limit) noexcept
{
    std::int64_t low = 0;
    std::int64_t high = 0;
    for (const std::int64_t value : x) {
        low = std::min(low, value);
        high = std::max(high, value);
    }
    if (std::max(magnitude_of(low), magnitude_of(high)) <= limit) {
        return std::nullopt;
    }
    for (const std::int64_t value : x) {
        if (magnitude_of(value) > limit) {
            return value;
        }
    }
    return std::nullopt;
}

// The levels of the H-transform of a tile whose longer side has n values, and of the quadtree of a quadrant: the
// smallest L with 2^L >= n, 0 for a side of 0 or 1 value.
unsigned levels(std::size_t n) noexcept
{
    return n <= 1 ? 0 : static_cast<unsigned>(std::bit_width(n - 1));
}

// n / 2^times, rounded up: the side of the region of a level.
std::size_t halved(std::size_t n, unsigned times) noexcept
{
    return n == 0 ? 0 : ((n - 1) >> times) + 1;
}

// value plus positive or negative as its sign asks, rounded down to a multiple of 2^bits: the rounding of the
// coefficients of a level, which keeps the bits that the inverse transform restores out of them.
std::int64_t round_down(std::int64_t value, std::int64_t positive, std::int64_t negative, unsigned bits) noexcept
{
    return (value + (value >= 0 ? positive : negative)) & -(std::int64_t{1} << bits);
}

void check_tile(std::size_t nx, std::size_t ny, std::size_t count)
{
    if (nx == 0 || ny == 0 || nx > max_side || ny > max_side || nx * ny != count) {
        throw_usage("HCOMPRESS_1 codes tiles of 1 to 2^31 - 1 rows of 1 to 2^31 - 1 values, not " +
                    std::to_string(count) + " values as " + std::to_string(nx) + " rows of " + std::to_string(ny));
    }
}

// The values at the even places of a line of n values, stride apart, moved to its front in their order, and those at
// the odd places after them: (y0, y1, y2, y3, y4) becomes (y0, y2, y4, y1, y3).
void deinterleave(std::span<std::int64_t> line, std::size_t n, std::size_t stride, std::span<std::int64_t> scratch)
{
    const std::size_t evens = (n + 1) / 2;
    const std::size_t odds = n / 2;
    for (std::size_t m = 0; m < odds; ++m) {
        scratch[m] = line[((2 * m) + 1) * stride];
    }
    for (std::size_t m = 1; m < evens; ++m) {
        line[m * stride] = line[2 * m * stride];
    }
    for (std::size_t m = 0; m < odds; ++m) {
        line[(evens + m) * stride] = scratch[m];
    }
}

// The inverse of deinterleave().
void interleave(std::span<std::int64_t> line, std::size_t n, std::size_t stride, std::span<std::int64_t> scratch)
{
    const std::size_t evens = (n + 1) / 2;
    const std::size_t odds = n / 2;
    for (std::size_t m = 0; m < odds; ++m) {
        scratch[m] = line[(evens + m) * stride];
    }
    for (std::size_t m = evens; m-- > 1;) {
        line[2 * m * stride] = line[m * stride];
    }
    for (std::size_t m = 0; m < odds; ++m) {
        line[((2 * m) + 1) * stride] = scratch[m];
    }
}

// The H-transform in place: at each level k, every 2 x 2 block of the region of ceil(rows / 2^k) x ceil(cols / 2^k)
// values at the top left becomes its sum, its difference along the columns (second row minus first), along the rows
// (second column minus first) and its cross difference; then each row and each column of the region moves its sums to
// its front, so that they form the region of the next level. Level 0 keeps the sums of four values whole, the others
// halve them, and a pair at an odd edge counts twice, a lone corner four times, as a block that repeats them would.
//
// With values of magnitude P, a value of level k stays below 2^(k+2) (P + k + 1), and its sums below twice that: a tile
// of L levels needs 2^(L+1) (P + L + 1) to fit, which hcompress_max_magnitude() keeps below 2^60.
void forward_transform(std::span<std::int64_t> x, std::size_t rows, std::size_t cols)
{
    const unsigned level_count = levels(std::max(rows, cols));
    std::vector<std::int64_t> scratch((std::max(rows, cols) / 2) + 1);
    for (unsigned k = 0; k < level_count; ++k) {
        const std::size_t active_rows = halved(rows, k);
        const std::size_t active_cols = halved(cols, k);
        const unsigned shift = k == 0 ? 0 : 1;
        const std::int64_t pair_factor = k == 0 ? 2 : 1;
        const std::int64_t corner_factor = k == 0 ? 4 : 2;
        // Differences round to a multiple of 2^(k+1), adding 2^k to those not negative; sums to a multiple of
        // 2^(k+2), adding 2^(k+1), or 2^(k+1) - 1 to negative ones. Cross differences keep every bit.
        const std::int64_t difference_bias = std::int64_t{1} << k;
        const std::int64_t sum_bias = std::int64_t{1} << (k + 1);
        const auto difference = [&](std::int64_t value) { return round_down(value, difference_bias, 0, k + 1); };
        const auto sum = [&](std::int64_t value) { return round_down(value, sum_bias, sum_bias - 1, k + 2); };

        std::size_t i = 0;
        for (; i + 1 < active_rows; i += 2) {
            const std::size_t top = i * cols;
            const std::size_t bottom = top + cols;
            std::size_t j = 0;
            for (; j + 1 < active_cols; j += 2) {
                const std::int64_t p = x[top + j];
                const std::int64_t q = x[top + j + 1];
                const std::int64_t r = x[bottom + j];
                const std::int64_t t = x[bottom + j + 1];
                x[top + j] = sum((p + q + r + t) >> shift);
                x[bottom + j] = difference((r + t - p - q) >> shift);
                x[top + j + 1] = difference((q + t - p - r) >> shift);
                x[bottom + j + 1] = (p + t - q - r) >> shift;
            }
            if (j < active_cols) {
                const std::int64_t p = x[top + j];
                const std::int64_t r = x[bottom + j];
                x[top + j] = sum((p + r) * pair_factor);
                x[bottom + j] = difference((r - p) * pair_factor);
            }
        }
        if (i < active_rows) {
            const std::size_t top = i * cols;
            std::size_t j = 0;
            for (; j + 1 < active_cols; j += 2) {
                const std::int64_t p = x[top + j];
                const std::int64_t q = x[top + j + 1];
                x[top + j] = sum((p + q) * pair_factor);
                x[top + j + 1] = difference((q - p) * pair_factor);
            }
            if (j < active_cols) {
                x[top + j] = sum(x[top + j] * corner_factor);
            }
        }

        for (std::size_t row = 0; row < active_rows; ++row) {
            deinterleave(x.subspan(row * cols), active_cols, 1, scratch);
        }
        for (std::size_t col = 0; col < active_cols; ++col) {
            deinterleave(x.subspan(col), active_rows, cols, scratch);
        }
    }
}

// Each coefficient divided by the scale and rounded to the nearest integer, halves toward 0: the loss of HCOMPRESS_1.
// The first coefficient too, though the stream stores it whole.
void divide_by_scale(std::span<std::int64_t> x, std::int64_t scale)
{
    if (scale <= 1) {
        return;
    }
    const std::int64_t bias = ((scale + 1) / 2) - 1;
    for (std::int64_t& value : x) {
        value = value > 0 ? (value + bias) / scale : (value - bias) / scale;
    }
}

// The change of the difference of a block toward the slope between the sums of the blocks on either side of it,
// before and after along the same axis, bounded so that the three stay monotonic and by step either way; a slope that
// monotonicity would flatten leaves the difference as it is. The difference holds 8 times the slope.
std::int64_t slope_change(std::int64_t before, std::int64_t sum, std::int64_t after, std::int64_t difference,
                          std::int64_t step)
{
    const std::int64_t rise = after - sum;
    const std::int64_t previous_rise = sum - before;
    const std::int64_t upper = 4 * std::max(std::min(rise, previous_rise), std::int64_t{0});
    const std::int64_t lower = 4 * std::min(std::max(rise, previous_rise), std::int64_t{0});
    if (lower >= upper) {
        return 0;
    }
    const std::int64_t target = std::clamp(after - before, lower, upper);
    return std::clamp((target - (8 * difference)) / 8, -step, step);
}

// The change of the cross difference of the block whose sum is at index at, from the sums of the four blocks at its
// corners and its two differences as the smoothing left them; the cross difference holds 64 times the curvature.
std::int64_t curvature_change(std::span<const std::int64_t> x, std::size_t at, std::size_t stride, std::int64_t step)
{
    const std::size_t two_rows = 2 * stride;
    const std::int64_t sum = x[at];
    const std::int64_t up_left = x[at - two_rows - 2];
    const std::int64_t up_right = x[at - two_rows + 2];
    const std::int64_t down_left = x[at + two_rows - 2];
    const std::int64_t down_right = x[at + two_rows + 2];
    const std::int64_t hx2 = 2 * x[at + stride];
    const std::int64_t hy2 = 2 * x[at + 1];
    constexpr std::int64_t zero = 0;
    const std::int64_t upper =
        16 * std::min({std::max(down_right - sum, zero) - hx2 - hy2, std::max(sum - down_left, zero) + hx2 - hy2,
                       std::max(sum - up_right, zero) - hx2 + hy2, std::max(up_left - sum, zero) + hx2 + hy2});
    const std::int64_t lower =
        16 * std::max({std::min(down_right - sum, zero) - hx2 - hy2, std::min(sum - down_left, zero) + hx2 - hy2,
                       std::min(sum - up_right, zero) - hx2 + hy2, std::min(up_left - sum, zero) + hx2 + hy2});
    if (lower >= upper) {
        return 0;
    }
    const std::int64_t target = std::clamp(down_right + up_left - up_right - down_left, lower, upper);
    return std::clamp((target - (64 * x[at + stride + 1])) / 64, -step, step);
}

// The smoothing of a lossy tile at a level, before its inverse, over its region of rows x cols: the differences along
// the columns, then along the rows, then the cross differences move toward what the sums of the neighbouring blocks
// suggest, each by at most step, half the scale, the most that the quantization moved it. The blocks at the edges keep
// their differences.
void smooth_level(std::span<std::int64_t> x, std::size_t stride, std::size_t rows, std::size_t cols, std::int64_t step)
{
    const std::size_t two_rows = 2 * stride;
    for (std::size_t i = 2; i + 2 < rows; i += 2) {
        for (std::size_t j = 0; j < cols; j += 2) {
            const std::size_t at = (i * stride) + j;
            x[at + stride] += slope_change(x[at - two_rows], x[at], x[at + two_rows], x[at + stride], step);
        }
    }
    for (std::size_t i = 0; i < rows; i += 2) {
        for (std::size_t j = 2; j + 2 < cols; j += 2) {
            const std::size_t at = (i * stride) + j;
            x[at + 1] += slope_change(x[at - 2], x[at], x[at + 2], x[at + 1], step);
        }
    }
    for (std::size_t i = 2; i + 2 < rows; i += 2) {
        for (std::size_t j = 2; j + 2 < cols; j += 2) {
            const std::size_t at = (i * stride) + j;
            x[at + stride + 1] += curvature_change(x, at, stride, step);
        }
    }
}

// The largest magnitude of a coefficient that the inverse of a tile of level_count levels takes, smoothed by step or
// not (0). Let M be that magnitude, D = M + step the most that a difference reaches once smoothed, and
// Y = 3D + 2^(L+5). The first sum, rounded, stays below M + 2^L; the four terms of a block, rounded, below the sum and
// 3D + 2^(k+3) at level k, so that if every sum stays below Y, the values of the level, halves of their sums, do too.
// The sums of a block then stay below 2Y, and the terms of the smoothing below 2^7 Y: Y below 2^62, or 2^56 when
// smoothed, keeps every step within 64 bits, with room for the coefficients that CFITSIO writes for 32-bit values,
// below 2^60, and those that hcompress_encode() writes at a scale above 1, below 2^53.
std::uint64_t max_coefficient(unsigned level_count, std::int64_t step) noexcept
{
    const std::uint64_t ceiling = std::uint64_t{1} << (step > 0 ? 56U : 62U);
    const std::uint64_t reserve = (std::uint64_t{1} << (level_count + 5)) + (3 * static_cast<std::uint64_t>(step));
    return (ceiling - reserve - 1) / 3;
}

// The inverse of forward_transform(), from the last level to the first, smoothed by step when it is above 0. Each
// difference restores the low bits that the rounding of the forward transform dropped: the cross difference gives
// the bit 2^k of the two differences, and the three differences give the bits 2^k and 2^(k+1) of the sum.
void inverse_transform(std::span<std::int64_t> x, std::size_t rows, std::size_t cols, std::int64_t step)
{
    const unsigned level_count = levels(std::max(rows, cols));
    if (level_count == 0) {
        return;
    }
    // The first sum is a multiple of 2^(L+1) as the encoder wrote it, but for the scaling.
    const std::int64_t top_bias = std::int64_t{1} << level_count;
    x[0] = round_down(x[0], top_bias, top_bias - 1, level_count + 1);
    std::vector<std::int64_t> scratch((std::max(rows, cols) / 2) + 1);
    for (unsigned k = level_count; k-- > 0;) {
        const std::size_t active_rows = halved(rows, k);
        const std::size_t active_cols = halved(cols, k);
        for (std::size_t row = 0; row < active_rows; ++row) {
            interleave(x.subspan(row * cols), active_cols, 1, scratch);
        }
        for (std::size_t col = 0; col < active_cols; ++col) {
            interleave(x.subspan(col), active_rows, cols, scratch);
        }
        if (step > 0) {
            smooth_level(x, cols, active_rows, active_cols, step);
        }

        // Level 0 divides the sums of a block by 4, the others by 2. Differences round to a multiple of 2^(k+1) and
        // cross differences to one of 2^k, at level 0 not at all.
        const unsigned shift = k == 0 ? 2 : 1;
        const std::int64_t low = std::int64_t{1} << k;
        const std::int64_t high = low << 1;
        const auto difference = [&](std::int64_t value) { return round_down(value, low, low - 1, k + 1); };
        const auto cross = [&](std::int64_t value) {
            return k == 0 ? value : round_down(value, low / 2, (low / 2) - 1, k);
        };

        std::size_t i = 0;
        for (; i + 1 < active_rows; i += 2) {
            const std::size_t top = i * cols;
            const std::size_t bottom = top + cols;
            std::size_t j = 0;
            for (; j + 1 < active_cols; j += 2) {
                std::int64_t h0 = x[top + j];
                std::int64_t hx = difference(x[bottom + j]);
                std::int64_t hy = difference(x[top + j + 1]);
                const std::int64_t hc = cross(x[bottom + j + 1]);
                const std::int64_t low_bit = hc & low;
                hx = hx >= 0 ? hx - low_bit : hx + low_bit;
                hy = hy >= 0 ? hy - low_bit : hy + low_bit;
                const std::int64_t high_bit = (hc ^ hx ^ hy) & high;
                if (h0 >= 0 || low_bit != 0) {
                    h0 += low_bit - high_bit;
                } else {
                    h0 += high_bit;
                }
                x[bottom + j + 1] = (h0 + hx + hy + hc) >> shift;
                x[bottom + j] = (h0 + hx - hy - hc) >> shift;
                x[top + j + 1] = (h0 - hx + hy - hc) >> shift;
                x[top + j] = (h0 - hx - hy + hc) >> shift;
            }
            if (j < active_cols) {
                std::int64_t h0 = x[top + j];
                const std::int64_t hx = difference(x[bottom + j]);
                const std::int64_t high_bit = hx & high;
                h0 = h0 >= 0 ? h0 - high_bit : h0 + high_bit;
                x[bottom + j] = (h0 + hx) >> shift;
                x[top + j] = (h0 - hx) >> shift;
            }
        }
        if (i < active_rows) {
            const std::size_t top = i * cols;
            std::size_t j = 0;
            for (; j + 1 < active_cols; j += 2) {
                std::int64_t h0 = x[top + j];
                const std::int64_t hy = difference(x[top + j + 1]);
                const std::int64_t high_bit = hy & high;
                h0 = h0 >= 0 ? h0 - high_bit : h0 + high_bit;
                x[top + j + 1] = (h0 + hy) >> shift;
                x[top + j] = (h0 - hy) >> shift;
            }
            if (j < active_cols) {
                x[top + j] >>= shift;
            }
        }
    }
}

// A quadrant of the coefficients: its first row and column, and its size.
struct quadrant
{
    std::size_t row = 0;
    std::size_t col = 0;
    std::size_t rows = 0;
    std::size_t cols = 0;
};

// The quadrants in the order of the stream: the top left, which holds the coefficients of every level but the first;
// the differences of the first level along the rows and along the columns, which share their number of planes; and its
// cross differences. A tile of one row or one column leaves two of them without cells.
std::array<quadrant, 4> quadrants_of(std::size_t rows, std::size_t cols) noexcept
{
    const std::size_t top_rows = (rows + 1) / 2;
    const std::size_t left_cols = (cols + 1) / 2;
    return {{
        {.row = 0, .col = 0, .rows = top_rows, .cols = left_cols},
        {.row = 0, .col = left_cols, .rows = top_rows, .cols = cols / 2},
        {.row = top_rows, .col = 0, .rows = rows / 2, .cols = left_cols},
        {.row = top_rows, .col = left_cols, .rows = rows / 2, .cols = cols / 2},
    }};
}

// A level of the quadtree of a quadrant, stored after the levels below it: level 1 has a code of 4 bits for each 2 x 2
// block of cells, and each level above it a code for each 2 x 2 block of the codes below, up to a level of one code.
// A code holds 8 for its top left, 4 for its top right, 2 for its bottom left and 1 for its bottom right.
struct code_level
{
    std::size_t offset = 0;
    std::size_t rows = 0;
    std::size_t cols = 0;
};

std::vector<code_level> code_levels(const quadrant& part)
{
    const unsigned count = std::max(levels(std::max(part.rows, part.cols)), 1U);
    std::vector<code_level> result;
    std::size_t offset = 0;
    for (unsigned m = 1; m <= count; ++m) {
        const code_level level{.offset = offset, .rows = halved(part.rows, m), .cols = halved(part.cols, m)};
        result.push_back(level);
        offset += level.rows * level.cols;
    }
    return result;
}

std::size_t codes_size(const std::vector<code_level>& tree) noexcept
{
    return tree.back().offset + (tree.back().rows * tree.back().cols);
}

unsigned bit_of(std::int64_t value, unsigned plane) noexcept
{
    return static_cast<unsigned>((value >> plane) & 1);
}

// The level-1 codes of a plane of a quadrant of magnitudes; the cells beyond an odd edge are 0.
void level_one_codes(std::span<const std::int64_t> x, std::size_t stride, const quadrant& part, unsigned plane,
                     std::span<std::uint8_t> codes)
{
    std::size_t k = 0;
    for (std::size_t i = 0; i < part.rows; i += 2) {
        const std::size_t top = ((part.row + i) * stride) + part.col;
        const std::size_t bottom = top + stride;
        const bool has_bottom = i + 1 < part.rows;
        std::size_t j = 0;
        if (has_bottom) {
            for (; j + 1 < part.cols; j += 2) {
                codes[k] = static_cast<std::uint8_t>(
                    (bit_of(x[top + j], plane) << 3U) | (bit_of(x[top + j + 1], plane) << 2U) |
                    (bit_of(x[bottom + j], plane) << 1U) | bit_of(x[bottom + j + 1], plane));
                ++k;
            }
        }
        for (; j < part.cols; j += 2) {
            unsigned code = bit_of(x[top + j], plane) << 3U;
            if (j + 1 < part.cols) {
                code |= bit_of(x[top + j + 1], plane) << 2U;
            }
            if (has_bottom) {
                code |= bit_of(x[bottom + j], plane) << 1U;
            }
            codes[k] = static_cast<std::uint8_t>(code);
            ++k;
        }
    }
}

// The codes of a level from those of the level below it: 8, 4, 2 and 1 for the codes of each 2 x 2 block that are not
// 0. Returns the bits of the code words of those of them that are not 0.
std::uint64_t reduce(std::span<std::uint8_t> codes, const code_level& from, const code_level& to)
{
    const auto marked = [&codes](std::size_t at) { return codes[at] != 0 ? 1U : 0U; };
    std::uint64_t bits = 0;
    std::size_t k = to.offset;
    for (std::size_t i = 0; i < from.rows; i += 2) {
        const std::size_t top = from.offset + (i * from.cols);
        const std::size_t bottom = top + from.cols;
        const bool has_bottom = i + 1 < from.rows;
        for (std::size_t j = 0; j < from.cols; j += 2) {
            const bool has_right = j + 1 < from.cols;
            unsigned code = marked(top + j) << 3U;
            if (has_right) {
                code |= marked(top + j + 1) << 2U;
            }
            if (has_bottom) {
                code |= marked(bottom + j) << 1U;
                if (has_right) {
                    code |= marked(bottom + j + 1);
                }
            }
            codes[k] = static_cast<std::uint8_t>(code);
            bits += marked_lengths[code];
            ++k;
        }
    }
    return bits;
}

// The planes of a quadrant of magnitudes, from the most significant. A plane is a quadtree, the nybble 15 then the
// code words of the codes that are not 0, from the code of the top level down to level 1, each level from its last code
// to its first; or the code word of 0 alone for a plane of nothing. A quadtree whose words fill as many bytes as half
// the blocks of level 1, rounded up, gives way to direct coding, the nybble 0 then every code of level 1 in a nybble,
// in their order, which gives the streams of CFITSIO; so does the plane of a quadrant without cells, where CFITSIO
// would write a quadtree of nothing.
void encode_quadrant(std::span<const std::int64_t> x, std::size_t stride, const quadrant& part, unsigned planes,
                     bit_writer& bits)
{
    const std::vector<code_level> tree = code_levels(part);
    const std::size_t blocks = tree.front().rows * tree.front().cols;
    const std::size_t limit = (blocks + 1) / 2;
    std::vector<std::uint8_t> codes(codes_size(tree));
    for (unsigned plane = planes; plane-- > 0;) {
        level_one_codes(x, stride, part, plane, codes);
        std::uint64_t code_bits = 0;
        for (std::size_t k = 0; k < blocks; ++k) {
            code_bits += marked_lengths[codes[k]];
        }
        bool direct = code_bits / 8 >= limit;
        for (std::size_t m = 1; m < tree.size() && !direct; ++m) {
            code_bits += reduce(codes, tree[m - 1], tree[m]);
            direct = code_bits / 8 >= limit;
        }
        if (direct) {
            bits.put(0, 4);
            bits.put_nybbles(std::span(codes).first(blocks));
        } else if (code_bits == 0) {
            bits.put(15, 4);
            bits.put_code(0);
        } else {
            bits.put(15, 4);
            bits.put_marked_codes(codes);
        }
    }
}

std::uint64_t largest_in(std::span<const std::int64_t> x, std::size_t stride, const quadrant& part) noexcept
{
    std::int64_t largest = 0;
    for (std::size_t i = 0; i < part.rows; ++i) {
        const std::size_t start = ((part.row + i) * stride) + part.col;
        for (std::size_t j = 0; j < part.cols; ++j) {
            largest = std::max(largest, x[start + j]);
        }
    }
    return static_cast<std::uint64_t>(largest);
}

unsigned planes_of(std::uint64_t largest) noexcept
{
    return static_cast<unsigned>(std::bit_width(largest));
}

// The sign bits of the values of x that are not 0, 1 for negative, most significant first, the last byte filled with
// zero bits; each value made its magnitude. Without a branch on the signs, which are those of noise.
std::vector<std::byte> take_signs(std::span<std::int64_t> x)
{
    // A bit for each value at most, written 64 at a time.
    std::vector<std::byte> signs((x.size() + 7) / 8);
    std::uint64_t buffer = 0;
    unsigned count = 0;
    std::size_t used = 0;
    for (std::int64_t& value : x) {
        const auto bits = static_cast<std::uint64_t>(value);
        const std::uint64_t negative = bits >> 63U;
        const unsigned taken = value != 0 ? 1U : 0U;
        buffer = (buffer << taken) | negative;
        count += taken;
        value = static_cast<std::int64_t>((bits ^ (0 - negative)) + negative);
        if (count == 64) {
            store_big_endian(std::span(signs).subspan(used).first<8>(), buffer);
            used += 8;
            count = 0;
        }
    }
    for (; count >= 8; count -= 8) {
        signs[used] = static_cast<std::byte>((buffer >> (count - 8)) & 0xFFU);
        ++used;
    }
    if (count > 0) {
        signs[used] = static_cast<std::byte>((buffer << (8 - count)) & 0xFFU);
        ++used;
    }
    signs.resize(used);
    return signs;
}

// The stream of the coefficients of a tile, checked against hcompress_max_magnitude().
void encode_tile(std::span<std::int64_t> x, std::size_t rows, std::size_t cols, std::int64_t scale,
                 std::vector<std::byte>& output)
{
    check_tile(rows, cols, x.size());
    if (scale < 0 || scale > max_scale) {
        throw_usage("HCOMPRESS_1 takes a scale from 0 to 2^31 - 1, not " + std::to_string(scale));
    }
    const std::uint64_t limit = hcompress_max_magnitude(rows, cols, scale);
    if (const std::optional<std::int64_t> value = beyond(x, limit)) {
        throw_usage("HCOMPRESS_1 compresses values of magnitude up to " + std::to_string(limit) + " in a tile of " +
                    std::to_string(rows) + " x " + std::to_string(cols) + " at the scale " + std::to_string(scale) +
                    ", not " + std::to_string(*value));
    }

    forward_transform(x, rows, cols);
    divide_by_scale(x, scale);
    output.push_back(std::byte{0xDD});
    output.push_back(std::byte{0x99});
    put_big_endian(output, rows, 4);
    put_big_endian(output, cols, 4);
    put_big_endian(output, static_cast<std::uint64_t>(scale), 4);
    put_big_endian(output, static_cast<std::uint64_t>(x[0]), 8);

    // The first coefficient is in the header; every other one that is not 0 gives its sign to a bit, 1 for negative,
    // and the planes code the magnitudes.
    x[0] = 0;
    const std::vector<std::byte> signs = take_signs(x);

    const std::array<quadrant, 4> parts = quadrants_of(rows, cols);
    const unsigned top_planes = planes_of(largest_in(x, cols, parts[0]));
    const unsigned difference_planes =
        planes_of(std::max(largest_in(x, cols, parts[1]), largest_in(x, cols, parts[2])));
    const unsigned cross_planes = planes_of(largest_in(x, cols, parts[3]));
    output.push_back(static_cast<std::byte>(top_planes));
    output.push_back(static_cast<std::byte>(difference_planes));
    output.push_back(static_cast<std::byte>(cross_planes));
    const std::array<unsigned, 4> planes{top_planes, difference_planes, difference_planes, cross_planes};
    bit_writer bits(output);
    for (std::size_t q = 0; q < parts.size(); ++q) {
        encode_quadrant(x, cols, parts[q], planes[q], bits);
    }
    // The end of the planes, then the signs from the next byte.
    bits.put(0, 4);
    bits.flush();
    output.insert(output.end(), signs.begin(), signs.end());
}

// The level-1 codes of a plane into the quadrant, as the bit 2^plane of the cells that they mark; a mark beyond an odd
// edge, which no encoder writes, is ignored, as CFITSIO does.
void insert_plane(std::span<const std::uint8_t> codes, std::span<std::int64_t> x, std::size_t stride,
                  const quadrant& part, unsigned plane)
{
    const std::int64_t value = std::int64_t{1} << plane;
    // The bit of a cell from the code of its block, 0 or the bit of the plane.
    const auto bit = [plane](unsigned code, unsigned shift) {
        return static_cast<std::int64_t>((code >> shift) & 1U) << plane;
    };
    std::size_t k = 0;
    for (std::size_t i = 0; i < part.rows; i += 2) {
        const std::size_t top = ((part.row + i) * stride) + part.col;
        const std::size_t bottom = top + stride;
        const bool has_bottom = i + 1 < part.rows;
        std::size_t j = 0;
        if (has_bottom) {
            // The blocks of four cells, without a branch on their bits, which those of the low planes, noise, would
            // mispredict.
            for (; j + 1 < part.cols; j += 2) {
                const unsigned code = codes[k];
                ++k;
                x[top + j] |= bit(code, 3);
                x[top + j + 1] |= bit(code, 2);
                x[bottom + j] |= bit(code, 1);
                x[bottom + j + 1] |= bit(code, 0);
            }
        }
        for (; j < part.cols; j += 2) {
            const unsigned code = codes[k];
            ++k;
            if (code == 0) {
                continue;
            }
            const bool has_right = j + 1 < part.cols;
            if ((code & 8U) != 0) {
                x[top + j] |= value;
            }
            if ((code & 4U) != 0 && has_right) {
                x[top + j + 1] |= value;
            }
            if ((code & 2U) != 0 && has_bottom) {
                x[bottom + j] |= value;
            }
            if ((code & 1U) != 0 && has_bottom && has_right) {
                x[bottom + j + 1] |= value;
            }
        }
    }
}

// The codes of a level from the level above it: each cell that its parent marks takes the next code word, from the
// last cell of the level to the first; the others are 0.
void expand(bit_reader& bits, std::span<std::uint8_t> codes, const code_level& from, const code_level& to)
{
    // The marks of the cells of each parent, from its code: 8 for the top left, 4 for the top right, 2 for the bottom
    // left and 1 for the bottom right.
    const std::span<std::uint8_t> marks = codes.subspan(to.offset, to.rows * to.cols);
    const auto mark = [](unsigned parent, unsigned shift) { return static_cast<std::uint8_t>((parent >> shift) & 1U); };
    for (std::size_t i = 0; i < to.rows; i += 2) {
        const std::size_t parents = from.offset + ((i / 2) * from.cols);
        const std::size_t top = i * to.cols;
        const std::size_t bottom = top + to.cols;
        const bool has_bottom = i + 1 < to.rows;
        for (std::size_t j = 0; j < to.cols; j += 2) {
            const unsigned parent = codes[parents + (j / 2)];
            const bool has_right = j + 1 < to.cols;
            marks[top + j] = mark(parent, 3);
            if (has_right) {
                marks[top + j + 1] = mark(parent, 2);
            }
            if (has_bottom) {
                marks[bottom + j] = mark(parent, 1);
                if (has_right) {
                    marks[bottom + j + 1] = mark(parent, 0);
                }
            }
        }
    }
    bits.get_codes(marks);
}

// The planes of a quadrant, as encode_quadrant() writes them, added to the magnitudes of x.
void decode_quadrant(bit_reader& bits, std::span<std::int64_t> x, std::size_t stride, const quadrant& part,
                     unsigned planes)
{
    const std::vector<code_level> tree = code_levels(part);
    const std::size_t blocks = tree.front().rows * tree.front().cols;
    std::vector<std::uint8_t> codes(codes_size(tree));
    for (unsigned plane = planes; plane-- > 0;) {
        const unsigned format = bits.get(4);
        if (format == 0) {
            bits.get_nybbles(std::span(codes).first(blocks));
        } else if (format == 15) {
            const unsigned top = bits.get_code();
            if (top == 0) {
                continue;
            }
            if (blocks == 0) {
                // CFITSIO reads the codes of a quadrant without cells into levels of one row or column.
                throw_corrupt("the HCOMPRESS_1 data mark cells in a quadrant that has none");
            }
            codes[tree.back().offset] = static_cast<std::uint8_t>(top);
            for (std::size_t m = tree.size() - 1; m-- > 0;) {
                expand(bits, codes, tree[m + 1], tree[m]);
            }
        } else {
            throw_corrupt("the HCOMPRESS_1 data code a bit plane in the format " + std::to_string(format) +
                          ", neither 0 nor 15");
        }
        insert_plane(codes, x, stride, part, plane);
    }
}

[[noreturn]] void throw_coefficient(std::int64_t value, std::int64_t scale)
{
    throw_corrupt("the HCOMPRESS_1 data hold a coefficient of " + std::to_string(value) + " at the scale " +
                  std::to_string(scale) + ", beyond the range of the inverse transform");
}

// The coefficients of a lossy stream multiplied by its scale, each checked against max_coefficient(): the error names
// the first coefficient beyond it.
void multiply_by_scale(std::span<std::int64_t> x, std::int64_t scale, unsigned level_count, std::int64_t step)
{
    const std::uint64_t factor = scale > 1 ? static_cast<std::uint64_t>(scale) : 1;
    if (const std::optional<std::int64_t> value = beyond(x, max_coefficient(level_count, step) / factor)) {
        throw_coefficient(*value, scale);
    }
    if (scale > 1) {
        for (std::int64_t& value : x) {
            value *= scale;
        }
    }
}

void decode_tile(std::span<const std::byte> input, std::size_t rows, std::size_t cols, bool smooth,
                 std::span<std::int64_t> x)
{
    if (input.size() < header_size) {
        throw_corrupt("the " + std::to_string(input.size()) + " bytes of HCOMPRESS_1 data are fewer than the " +
                      std::to_string(header_size) + " of its header");
    }
    if (input[0] != std::byte{0xDD} || input[1] != std::byte{0x99}) {
        throw_corrupt("the HCOMPRESS_1 data do not start with the bytes 0xDD 0x99");
    }
    const std::int64_t stream_rows = signed_32(get_big_endian(input, 2, 4));
    const std::int64_t stream_cols = signed_32(get_big_endian(input, 6, 4));
    if (std::cmp_not_equal(stream_rows, rows) || std::cmp_not_equal(stream_cols, cols)) {
        throw_corrupt("the HCOMPRESS_1 data describe a tile of " + std::to_string(stream_rows) + " rows of " +
                      std::to_string(stream_cols) + " values, not one of " + std::to_string(rows) + " rows of " +
                      std::to_string(cols));
    }
    const std::int64_t scale = signed_32(get_big_endian(input, 10, 4));
    const auto sum = static_cast<std::int64_t>(get_big_endian(input, 14, 8));
    std::array<unsigned, 3> groups{};
    for (std::size_t g = 0; g < groups.size(); ++g) {
        groups[g] = std::to_integer<unsigned>(input[planes_offset + g]);
        if (groups[g] > max_planes) {
            throw_corrupt("the HCOMPRESS_1 data give " + std::to_string(groups[g]) + " bit planes to a quadrant, " +
                          "more than the " + std::to_string(max_planes) + " of 64-bit coefficients");
        }
    }

    std::ranges::fill(x, 0);
    bit_reader bits(input.subspan(header_size));
    const std::array<quadrant, 4> parts = quadrants_of(rows, cols);
    const std::array<unsigned, 4> planes{groups[0], groups[1], groups[1], groups[2]};
    for (std::size_t q = 0; q < parts.size(); ++q) {
        decode_quadrant(bits, x, cols, parts[q], planes[q]);
    }
    if (bits.get(4) != 0) {
        throw_corrupt("the bit planes of the HCOMPRESS_1 data do not end with the nybble 0");
    }
    // A sign bit for each coefficient that is not 0, from the next byte; the first takes one too if the planes made it
    // other than 0, as CFITSIO reads it, before the header replaces it.
    bits.align();
    bits.negate_signed(x);
    x[0] = sum;

    const unsigned level_count = levels(std::max(rows, cols));
    const std::int64_t step = smooth && scale > 1 ? scale / 2 : 0;
    multiply_by_scale(x, scale, level_count, step);
    inverse_transform(x, rows, cols, step);
}

} // namespace

std::uint64_t hcompress_max_magnitude(std::size_t nx, std::size_t ny, std::int64_t scale) noexcept
{
    // With values of magnitude P, the coefficients of a tile of L levels stay below 2^(L+1) (P + L + 1).
    const unsigned level_count = levels(std::max(nx, ny));
    const unsigned bits = scale > 1 ? lossy_bits : lossless_bits;
    if (level_count + 8 > bits) {
        return 0;
    }
    return (std::uint64_t{1} << (bits - level_count - 1)) - level_count - 1;
}

void hcompress_encode(std::span<const std::int32_t> values, std::size_t nx, std::size_t ny, std::int64_t scale,
                      std::vector<std::byte>& output)
{
    std::vector<std::int64_t> x(values.begin(), values.end());
    encode_tile(x, nx, ny, scale, output);
}

void hcompress_encode(std::span<const std::int64_t> values, std::size_t nx, std::size_t ny, std::int64_t scale,
                      std::vector<std::byte>& output)
{
    std::vector<std::int64_t> x(values.begin(), values.end());
    encode_tile(x, nx, ny, scale, output);
}

void hcompress_decode(std::span<const std::byte> input, std::size_t nx, std::size_t ny, bool smooth,
                      std::span<std::int32_t> output)
{
    check_tile(nx, ny, output.size());
    std::vector<std::int64_t> x(output.size());
    decode_tile(input, nx, ny, smooth, x);
    for (const std::int64_t value : x) {
        if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max()) {
            throw_corrupt("the HCOMPRESS_1 data decode to " + std::to_string(value) + ", beyond 32-bit integers");
        }
    }
    std::ranges::transform(x, output.begin(), [](std::int64_t value) { return static_cast<std::int32_t>(value); });
}

void hcompress_decode(std::span<const std::byte> input, std::size_t nx, std::size_t ny, bool smooth,
                      std::span<std::int64_t> output)
{
    check_tile(nx, ny, output.size());
    decode_tile(input, nx, ny, smooth, output);
}

} // namespace openfits::detail
