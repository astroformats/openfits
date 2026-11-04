// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// A model of a file made from untrusted bytes, written, and read back (standard §3.3.2, §4, §5): the input gives the
// type of the values, the axes, cards of the header, reserved cards and the values themselves. Written in their own
// type, the values read back bit for bit, with the cards in their order, and the same model writes the same bytes into
// any sink, its values in memory or streamed, and a copy of the unit read writes it again. Written into integer data of
// a BITPIX and a scaling that the input gives, each stored value is the value less BZERO over BSCALE, rounded half away
// from zero and clamped, and BLANK for NaN. A model that the writer refuses must be refused with an error of the
// library's kinds.

#include <openfits/error.h>
#include <openfits/header.h>
#include <openfits/image.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/types.h>
#include <openfits/writer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using openfits::errc;
using openfits::reader;
using openfits::writer;

void require(bool condition)
{
    if (!condition) {
        std::abort();
    }
}

class input_bytes
{
public:
    explicit input_bytes(std::span<const std::byte> data) : data_(data) {}

    std::uint8_t next()
    {
        return at_ < data_.size() ? std::to_integer<std::uint8_t>(data_[at_++]) : 0;
    }

    // The next byte of the input, round and round, for the values of the data.
    std::byte cycle()
    {
        if (data_.empty()) {
            return std::byte{0};
        }
        return data_[(at_++) % data_.size()];
    }

private:
    std::span<const std::byte> data_;
    std::size_t at_ = 0;
};

std::vector<std::byte> write(const writer& output, bool append_only)
{
    std::vector<std::byte> file;
    if (append_only) {
        openfits::callback_sink sink(
            [&file](std::span<const std::byte> data) { file.insert(file.end(), data.begin(), data.end()); });
        output.save(sink);
    } else {
        openfits::memory_sink sink;
        output.save(sink);
        file = sink.release();
    }
    return file;
}

// The cards of the input: names of the characters of standard §4.1.2.1, values of each kind.
openfits::header cards_of(input_bytes& in)
{
    openfits::header cards;
    const std::size_t count = in.next() % 4;
    constexpr std::array<char, 38> letters{'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
                                           'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
                                           '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '-', '_'};
    for (std::size_t i = 0; i < count; ++i) {
        std::string name;
        const std::size_t length = 1 + (in.next() % 8);
        for (std::size_t c = 0; c < length; ++c) {
            name += letters[in.next() % letters.size()];
        }
        const std::uint8_t kind = in.next() % 5;
        const auto number = static_cast<std::int64_t>(static_cast<std::int8_t>(in.next())) * 1000;
        switch (kind) {
        case 0:
            cards.append(openfits::card(name, number % 2 == 0));
            break;
        case 1:
            cards.append(openfits::card(name, number, "an integer"));
            break;
        case 2:
            cards.append(openfits::card(name, static_cast<double>(number) / 7.0));
            break;
        case 3:
            // The text of the value, which the record holds as it is.
            cards.append(openfits::card::with_value_text(name, std::to_string(number), "a value text"));
            break;
        default:
            cards.append(openfits::card(name, std::string(length, 'x'), "a string"));
            break;
        }
    }
    return cards;
}

// The errors of a model that the writer refuses: a card of a structural keyword, or one that no record holds.
bool refused_model(errc code)
{
    return code == errc::structural_keyword || code == errc::invalid_keyword || code == errc::value_too_long ||
           code == errc::comment_too_long || code == errc::invalid_value_indicator;
}

template <typename T>
void round_trip(input_bytes& in, const std::vector<std::uint64_t>& axes, const openfits::header& cards)
{
    std::uint64_t count = axes.empty() ? 0 : 1;
    for (const std::uint64_t length : axes) {
        count *= length;
    }
    std::vector<T> values(count);
    for (T& value : values) {
        std::array<std::byte, sizeof(T)> bits{};
        for (std::byte& b : bits) {
            b = in.cycle();
        }
        std::memcpy(&value, bits.data(), sizeof(T));
    }
    openfits::image_info info;
    info.geometry.axes = axes;
    info.header = cards;
    writer output;
    const std::size_t reserved = in.next() % 40;
    (void)output.add_image(info, std::span<const T>(values), {.reserved_cards = reserved});

    std::vector<std::byte> file;
    try {
        file = write(output, false);
    } catch (const openfits::validation_error& failure) {
        require(refused_model(failure.code()));
        return;
    }
    require(file.size() % 2880 == 0);
    require(write(output, true) == file);
    // The values given chunk after chunk write the same file.
    writer streamed;
    (void)streamed.add_image(info, openfits::numeric_type_of<T>(),
                             [&values](std::uint64_t first, std::span<std::byte> chunk) {
                                 std::memcpy(chunk.data(), values.data() + first, chunk.size());
                             },
                             {.reserved_cards = reserved});
    require(write(streamed, false) == file);

    const reader input(std::make_unique<openfits::memory_source>(file));
    require(input.hdu_count() == 1);
    require(input.diagnostics().empty());
    const openfits::image_info image = input.image(0);
    require(image.geometry.axes == axes);
    require(image.value_type() == openfits::numeric_type_of<T>());
    require(input.hdu(0).header().free_cards() == reserved);
    // The cards given, after the structural ones, in their order.
    const openfits::header& read_cards = input.hdu(0).header();
    const openfits::header given = openfits::without_structural(read_cards);
    require(given.size() - given.free_cards() == cards.size());
    for (std::size_t i = 0; i < cards.size(); ++i) {
        require(given[i].keyword() == cards[i].keyword());
        require(given[i].value() == cards[i].value());
    }
    const std::vector<T> back = input.read_pixels<T>(0);
    require(back.size() == values.size());
    require(values.empty() || std::memcmp(back.data(), values.data(), values.size() * sizeof(T)) == 0);
    // The unit copied as it is.
    writer copier;
    (void)copier.add_hdu(input, 0);
    require(write(copier, true) == file);
}

// value rounded half away from zero, from its truncation and the fraction, which the subtraction gives exactly: the
// std::round() of some C runtimes adds 0.5 first, which takes the largest double below 0.5 to 1.
double half_away(double value)
{
    const double whole = std::trunc(value);
    const double fraction = value - whole;
    if (fraction >= 0.5) {
        return whole + 1.0;
    }
    if (fraction <= -0.5) {
        return whole - 1.0;
    }
    return whole;
}

// Values of double written into integer data with a scaling: each stored value as a plain reference computes it.
void scaled_write(input_bytes& in, const std::vector<std::uint64_t>& axes)
{
    constexpr std::array<openfits::bitpix, 4> integers{openfits::bitpix::uint8, openfits::bitpix::int16,
                                                       openfits::bitpix::int32, openfits::bitpix::int64};
    const openfits::bitpix stored = integers[in.next() % integers.size()];
    const double scale = std::ldexp(1.0 + (in.next() % 8), (in.next() % 9) - 4) * ((in.next() % 2) == 0 ? 1 : -1);
    const double zero = static_cast<double>(static_cast<std::int8_t>(in.next())) * 64.0;
    std::uint64_t count = axes.empty() ? 0 : 1;
    for (const std::uint64_t length : axes) {
        count *= length;
    }
    std::vector<double> values(count);
    for (double& value : values) {
        // Whole and half values about the range of every type, and NaN.
        const auto byte = static_cast<std::int8_t>(in.next());
        value = byte == -128 ? std::numeric_limits<double>::quiet_NaN()
                             : static_cast<double>(byte) * std::ldexp(1.0, in.next() % 64) / 2.0;
    }
    openfits::image_info info;
    info.geometry.axes = axes;
    info.bitpix = stored;
    info.scaling = {.scale = scale, .zero = zero};
    info.blank = 0;
    writer output;
    (void)output.add_image(info, std::span<const double>(values), {.overflow = openfits::overflow_policy::clamp});
    const reader input(std::make_unique<openfits::memory_source>(write(output, false)));
    // The stored values, BLANK among them: an integer type gets BLANK as any stored value.
    const std::vector<std::int64_t> raw =
        input.read_pixels<std::int64_t>(0, {.scaling = openfits::scaling_policy::raw});
    require(raw.size() == values.size());
    // The range of the stored type, its upper bound excluded and exact in double.
    std::int64_t low = 0;
    std::int64_t high = 255;
    if (stored == openfits::bitpix::int16) {
        low = std::numeric_limits<std::int16_t>::min();
        high = std::numeric_limits<std::int16_t>::max();
    } else if (stored == openfits::bitpix::int32) {
        low = std::numeric_limits<std::int32_t>::min();
        high = std::numeric_limits<std::int32_t>::max();
    } else if (stored == openfits::bitpix::int64) {
        low = std::numeric_limits<std::int64_t>::min();
        high = std::numeric_limits<std::int64_t>::max();
    }
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (std::isnan(values[i])) {
            require(raw[i] == 0);
            continue;
        }
        // The bounds as doubles are exact, but that of int64, 2^63, which no value below it reaches.
        const double rounded = half_away((values[i] - zero) / scale);
        std::int64_t expected = 0;
        if (rounded <= static_cast<double>(low)) {
            expected = low;
        } else if (rounded >= static_cast<double>(high)) {
            expected = high;
        } else {
            expected = static_cast<std::int64_t>(rounded);
        }
        require(raw[i] == expected);
    }
}

} // namespace

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    input_bytes in({reinterpret_cast<const std::byte*>(data), size});
    const std::uint8_t type = in.next() % 10;
    const std::size_t naxis = in.next() % 4;
    std::vector<std::uint64_t> axes;
    axes.reserve(naxis);
    for (std::size_t a = 0; a < naxis; ++a) {
        axes.push_back(in.next() % 9);
    }
    openfits::header cards;
    try {
        cards = cards_of(in);
    } catch (const openfits::usage_error& failure) {
        // A name that set() refuses, such as END.
        require(failure.code() == errc::invalid_keyword);
        return 0;
    }
    switch (type) {
    case 0:
        round_trip<std::int8_t>(in, axes, cards);
        break;
    case 1:
        round_trip<std::uint8_t>(in, axes, cards);
        break;
    case 2:
        round_trip<std::int16_t>(in, axes, cards);
        break;
    case 3:
        round_trip<std::uint16_t>(in, axes, cards);
        break;
    case 4:
        round_trip<std::int32_t>(in, axes, cards);
        break;
    case 5:
        round_trip<std::uint32_t>(in, axes, cards);
        break;
    case 6:
        round_trip<std::int64_t>(in, axes, cards);
        break;
    case 7:
        round_trip<std::uint64_t>(in, axes, cards);
        break;
    case 8:
        round_trip<float>(in, axes, cards);
        break;
    default:
        round_trip<double>(in, axes, cards);
        break;
    }
    scaled_write(in, axes);
    return 0;
}
