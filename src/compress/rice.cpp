// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "compress/rice.h"

#include <openfits/error.h>

#include "core/endian.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <string>
#include <type_traits>

namespace openfits::detail {

namespace {

// The constants of a coder of values of Bytes bytes: the bits of the code of a block, which gives its split level fs
// as fs + 1; the split level from which a block is stored as it is, whose code is one more; and the bits of a value.
// Those of 1, 2 and 4 bytes are CFITSIO's; 8 bytes extend the pattern of 1 and 2, the code space full.
template <std::size_t Bytes> struct rice_constants;

template <> struct rice_constants<1>
{
    static constexpr unsigned code_bits = 3;
    static constexpr unsigned max_split = 6;
};

template <> struct rice_constants<2>
{
    static constexpr unsigned code_bits = 4;
    static constexpr unsigned max_split = 14;
};

template <> struct rice_constants<4>
{
    static constexpr unsigned code_bits = 5;
    static constexpr unsigned max_split = 25;
};

template <> struct rice_constants<8>
{
    static constexpr unsigned code_bits = 6;
    static constexpr unsigned max_split = 62;
};

[[noreturn]] void throw_corrupt(const std::string& message)
{
    throw integrity_error(errc::corrupt_compressed_data, message);
}

// The errors of the decoder, whose messages are built here, out of its loops.
[[noreturn]] void throw_end()
{
    throw_corrupt("the RICE_1 data end before the values of their tile");
}

[[noreturn]] void throw_code(unsigned code)
{
    throw_corrupt("the RICE_1 data hold the code " + std::to_string(code) + " of no block");
}

[[noreturn]] void throw_wide(unsigned value_bits)
{
    throw_corrupt("the RICE_1 data hold a value beyond " + std::to_string(value_bits) + " bits");
}

void check_block_size(std::size_t block_size)
{
    if (block_size != rice_block_16 && block_size != rice_block_32) {
        throw usage_error(errc::invalid_argument,
                          "RICE_1 codes blocks of 16 or 32 pixels, not " + std::to_string(block_size));
    }
}

// value modulo 2^(8 x sizeof(U)): a cast, but for 64-bit values, where GCC calls the cast useless.
template <typename U> U truncate_to(std::uint64_t value) noexcept
{
    if constexpr (std::is_same_v<U, std::uint64_t>) {
        return value;
    } else {
        return static_cast<U>(value);
    }
}

// The difference of two values mapped to a non-negative one: 2d for d >= 0, -2d - 1 for d < 0, modulo 2^bits.
template <typename U> U map_difference(U value, U last) noexcept
{
    // 64 bits for every U, as the shifts need: auto would take U, which is narrower but for 64-bit values.
    // NOLINTNEXTLINE(modernize-use-auto)
    const std::uint64_t difference = truncate_to<U>(std::uint64_t{value} - last);
    const std::uint64_t negative = difference >> (sizeof(U) * 8 - 1);
    return truncate_to<U>((difference << 1U) ^ (0 - negative));
}

template <typename U> U unmap_difference(std::uint64_t mapped) noexcept
{
    return truncate_to<U>((mapped >> 1U) ^ (0 - (mapped & 1U)));
}

// The low bits of a 64-bit word, bits at most 63.
std::uint64_t low_bits(std::uint64_t value, unsigned bits) noexcept
{
    return value & ((std::uint64_t{1} << bits) - 1);
}

// Bits written most significant first, the last byte filled with zero bits, 32 at a time into output, which grows as
// needed and ends at the last byte written.
class bit_writer
{
public:
    // A writer that appends to output, which it first makes room bytes larger.
    bit_writer(std::vector<std::byte>& output, std::size_t room) : output_(output), used_(output.size())
    {
        output_.resize(used_ + room);
    }

    // The low bits of value, bits at most 32.
    void put(std::uint64_t value, unsigned bits)
    {
        buffer_ = (buffer_ << bits) | low_bits(value, bits);
        count_ += bits;
        if (count_ >= 32) {
            count_ -= 32;
            ensure(4);
            store_big_endian(std::span(output_).subspan(used_).first<4>(),
                             static_cast<std::uint32_t>(buffer_ >> count_));
            used_ += 4;
        }
    }

    // The low bits of value, up to 64.
    void put_wide(std::uint64_t value, unsigned bits)
    {
        if (bits > 32) {
            put(value >> 32U, bits - 32);
            put(value & 0xFFFF'FFFFU, 32);
        } else {
            put(value, bits);
        }
    }

    // count zero bits, then a one.
    void put_unary(std::uint64_t count)
    {
        while (count >= 32) {
            put(0, 32);
            count -= 32;
        }
        put(1, static_cast<unsigned>(count) + 1);
    }

    // The mapped values of a block split at split, below 63: for each, value >> split zero bits, a one, then its split
    // low bits. The state of the writer is in locals for the loop, where it stays in registers.
    template <typename U> void put_block(std::span<const U> mapped, unsigned split)
    {
        std::uint64_t buffer = buffer_;
        unsigned count = count_;
        std::size_t used = used_;
        std::byte* out = output_.data();
        std::size_t size = output_.size();
        const std::uint64_t one = std::uint64_t{1} << split;
        for (const U value : mapped) {
            const std::uint64_t top = std::uint64_t{value} >> split;
            if (top >= 32 || top + 1 + split > 32 || size - used < 4) {
                // A long high part, or the output full: the writer's own calls.
                buffer_ = buffer;
                count_ = count;
                used_ = used;
                put_unary(top);
                if (split > 0) {
                    put_wide(value, split);
                }
                buffer = buffer_;
                count = count_;
                used = used_;
                out = output_.data();
                size = output_.size();
                continue;
            }
            // One write: the one bit above the low bits, and the zero bits above it.
            const auto bits = static_cast<unsigned>(top) + 1 + split;
            buffer = (buffer << bits) | one | low_bits(value, split);
            count += bits;
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

// Bits read most significant first from a span, never beyond it. The bits held are at the top of a 64-bit word, from
// its most significant bit: the first count_ of them are those not yet taken, and the bits after them, when not 0, are
// those of the bytes that follow, which a load of a whole word brought in early. Bytes are taken whole, a word at a
// time while the input has 8 bytes left.
class bit_reader
{
public:
    explicit bit_reader(std::span<const std::byte> input) noexcept : input_(input) {}

    // The next bits bits, at most 32.
    std::uint64_t get(unsigned bits)
    {
        refill();
        if (count_ < bits) {
            throw_end();
        }
        const std::uint64_t value = (buffer_ >> 1U) >> (63 - bits);
        buffer_ <<= bits;
        count_ -= bits;
        return value;
    }

    std::uint64_t get_wide(unsigned bits)
    {
        if (bits > 32) {
            const std::uint64_t high = get(bits - 32);
            return (high << 32U) | get(32);
        }
        return get(bits);
    }

    // The number of zero bits before the next one bit, which it takes.
    std::uint64_t get_unary()
    {
        std::uint64_t zeros = 0;
        for (;;) {
            refill();
            // The bits not yet taken, those after them cleared.
            const std::uint64_t held = buffer_ & ~(~std::uint64_t{0} >> count_);
            if (held != 0) {
                const auto skipped = static_cast<unsigned>(std::countl_zero(held));
                zeros += skipped;
                buffer_ <<= skipped + 1;
                count_ -= skipped + 1;
                return zeros;
            }
            if (count_ == 0) {
                throw_end();
            }
            zeros += count_;
            buffer_ <<= count_;
            count_ = 0;
        }
    }

    // The mapped value of a block split at split bits, which has value_bits bits at most: the zero bits of its high
    // part, a one, then its split low bits.
    std::uint64_t get_split(unsigned split, unsigned value_bits)
    {
        const std::uint64_t top = get_unary();
        if (split + static_cast<unsigned>(std::bit_width(top)) > value_bits) {
            throw_wide(value_bits);
        }
        std::uint64_t mapped = top << split;
        if (split > 0) {
            mapped |= get_wide(split);
        }
        return mapped;
    }

    // The values of a block split at split bits, of value_bits bits, into values, each the one before it plus its
    // difference, from last; returns the last of them. The state of the reader is in locals for the loop, where it
    // stays in registers: a value whose bits the word held gives is taken there, any other by get_split().
    template <typename U> U get_block(unsigned split, unsigned value_bits, U last, std::span<U> values)
    {
        // The largest high part of a value of value_bits bits, of which split are below it.
        const unsigned high_bits = value_bits - split;
        const std::uint64_t most = high_bits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << high_bits) - 1;
        const std::byte* const data = input_.data();
        const std::size_t size = input_.size();
        std::uint64_t buffer = buffer_;
        unsigned count = count_;
        std::size_t next = next_;
        for (U& value : values) {
            std::uint64_t mapped = 0;
            bool taken = false;
            if (size - next >= 8) {
                // refill(), and the value if the bits held have all of it.
                buffer |= load_big_endian<std::uint64_t>(std::span<const std::byte, 8>(data + next, 8)) >> count;
                next += (63 - count) / 8;
                count |= 56U;
                const auto zeros = static_cast<unsigned>(std::countl_zero(buffer));
                const unsigned used = zeros + 1 + split;
                if (used <= count) {
                    if (zeros > most) {
                        throw_wide(value_bits);
                    }
                    mapped = (std::uint64_t{zeros} << split) | ((((buffer << zeros) << 1U) >> 1U) >> (63 - split));
                    buffer <<= used;
                    count -= used;
                    taken = true;
                }
            }
            if (!taken) {
                buffer_ = buffer;
                count_ = count;
                next_ = next;
                mapped = get_split(split, value_bits);
                buffer = buffer_;
                count = count_;
                next = next_;
            }
            last = truncate_to<U>(std::uint64_t{last} + unmap_difference<U>(mapped));
            value = last;
        }
        buffer_ = buffer;
        count_ = count;
        next_ = next;
        return last;
    }

private:
    // Holds at least 56 bits, or every bit left; at most 63. With 8 bytes left, a word: those of its bytes that fit
    // whole are taken, and the bits of the next one follow them.
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

// The split level of a block of count mapped values whose sum is sum, in two 64-bit words: the bits of
// floor(floor((sum - floor(count / 2) - 1) / count) / 2), 0 for a sum that leaves nothing, as CFITSIO computes it.
unsigned split_level(std::uint64_t high, std::uint64_t low, std::size_t count) noexcept
{
    const std::uint64_t bias = (count / 2) + 1;
    if (high == 0) {
        // A sum of 64 bits, as those of values of up to 32 bits always are: one division.
        return low < bias ? 0 : static_cast<unsigned>(std::bit_width(((low - bias) / count) >> 1U));
    }
    if (low < bias) {
        --high;
    }
    low -= bias;
    // The quotient by count, in digits of 32 bits, the remainder below count.
    const std::array<std::uint64_t, 4> digits{high >> 32U, high & 0xFFFF'FFFFU, low >> 32U, low & 0xFFFF'FFFFU};
    std::array<std::uint64_t, 4> quotient{};
    std::uint64_t remainder = 0;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        const std::uint64_t part = (remainder << 32U) | digits[i];
        quotient[i] = part / count;
        remainder = part % count;
    }
    const std::uint64_t quotient_high = (quotient[0] << 32U) | quotient[1];
    const std::uint64_t quotient_low = (quotient[2] << 32U) | quotient[3];
    // The bits of the quotient halved.
    if (quotient_high != 0) {
        return static_cast<unsigned>(std::bit_width(quotient_high)) + 63;
    }
    return static_cast<unsigned>(std::bit_width(quotient_low >> 1U));
}

template <typename U> void encode(std::span<const U> values, std::size_t block_size, std::vector<std::byte>& output)
{
    check_block_size(block_size);
    using constants = rice_constants<sizeof(U)>;
    constexpr unsigned value_bits = sizeof(U) * 8;
    if (values.empty()) {
        return;
    }
    // Room for the values as they are, which most streams need less than, and the codes of the blocks.
    bit_writer bits(output, (values.size() * sizeof(U)) + (values.size() / 4) + 16);
    bits.put_wide(values[0], value_bits);
    std::array<U, rice_block_32> mapped{};
    U last = values[0];
    for (std::size_t start = 0; start < values.size(); start += block_size) {
        const std::size_t count = std::min(block_size, values.size() - start);
        std::uint64_t high = 0;
        std::uint64_t low = 0;
        for (std::size_t j = 0; j < count; ++j) {
            mapped[j] = map_difference(values[start + j], last);
            last = values[start + j];
            const std::uint64_t value = mapped[j];
            low += value;
            // The sum of a block of values of up to 32 bits holds in 64.
            if constexpr (sizeof(U) == 8) {
                if (low < value) {
                    ++high;
                }
            }
        }
        const unsigned split = split_level(high, low, count);
        if (split >= constants::max_split) {
            // Too much information: the mapped values as they are.
            bits.put(constants::max_split + 1, constants::code_bits);
            for (std::size_t j = 0; j < count; ++j) {
                bits.put_wide(mapped[j], value_bits);
            }
        } else if (high == 0 && low == 0) {
            // No information: every value equal to the last.
            bits.put(0, constants::code_bits);
        } else {
            bits.put(split + 1, constants::code_bits);
            bits.put_block(std::span<const U>(mapped).first(count), split);
        }
    }
    bits.flush();
}

template <typename U> void decode(std::span<const std::byte> input, std::size_t block_size, std::span<U> output)
{
    check_block_size(block_size);
    using constants = rice_constants<sizeof(U)>;
    constexpr unsigned value_bits = sizeof(U) * 8;
    if (output.empty()) {
        return;
    }
    if (input.size() < rice_minimum_size(output.size(), sizeof(U), block_size)) {
        throw_corrupt("the " + std::to_string(input.size()) + " bytes of RICE_1 data are too few for the " +
                      std::to_string(output.size()) + " values of their tile");
    }
    bit_reader bits(input);
    U last = truncate_to<U>(bits.get_wide(value_bits));
    for (std::size_t start = 0; start < output.size(); start += block_size) {
        const std::span<U> block = output.subspan(start, std::min(block_size, output.size() - start));
        const auto code = static_cast<unsigned>(bits.get(constants::code_bits));
        if (code == 0) {
            std::ranges::fill(block, last);
            continue;
        }
        if (code == constants::max_split + 1) {
            for (U& value : block) {
                last = truncate_to<U>(std::uint64_t{last} + unmap_difference<U>(bits.get_wide(value_bits)));
                value = last;
            }
            continue;
        }
        if (code > constants::max_split + 1) {
            throw_code(code);
        }
        last = bits.get_block(code - 1, value_bits, last, block);
    }
}

} // namespace

std::uint64_t rice_minimum_size(std::uint64_t count, std::size_t bytes, std::size_t block_size) noexcept
{
    if (count == 0 || block_size == 0) {
        return 0;
    }
    const std::uint64_t code_bits = bytes == 1 ? 3 : bytes == 2 ? 4 : bytes == 4 ? 5 : 6;
    const std::uint64_t blocks = ((count - 1) / block_size) + 1;
    const std::uint64_t bits = (bytes * 8) + (blocks * code_bits);
    return (bits + 7) / 8;
}

void rice_encode(std::span<const std::uint8_t> values, std::size_t block_size, std::vector<std::byte>& output)
{
    encode(values, block_size, output);
}

void rice_encode(std::span<const std::uint16_t> values, std::size_t block_size, std::vector<std::byte>& output)
{
    encode(values, block_size, output);
}

void rice_encode(std::span<const std::uint32_t> values, std::size_t block_size, std::vector<std::byte>& output)
{
    encode(values, block_size, output);
}

void rice_encode(std::span<const std::uint64_t> values, std::size_t block_size, std::vector<std::byte>& output)
{
    encode(values, block_size, output);
}

void rice_decode(std::span<const std::byte> input, std::size_t block_size, std::span<std::uint8_t> output)
{
    decode(input, block_size, output);
}

void rice_decode(std::span<const std::byte> input, std::size_t block_size, std::span<std::uint16_t> output)
{
    decode(input, block_size, output);
}

void rice_decode(std::span<const std::byte> input, std::size_t block_size, std::span<std::uint32_t> output)
{
    decode(input, block_size, output);
}

void rice_decode(std::span<const std::byte> input, std::size_t block_size, std::span<std::uint64_t> output)
{
    decode(input, block_size, output);
}

} // namespace openfits::detail
