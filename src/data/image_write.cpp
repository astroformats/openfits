// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "data/image_write.h"

#include <openfits/error.h>

#include "core/checked_math.h"
#include "core/endian.h"
#include "core/scratch_buffer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace openfits::detail {

namespace {

constexpr std::uint64_t block_size = 2880;
constexpr double two_to_63 = 9223372036854775808.0;
constexpr double two_to_64 = 18446744073709551616.0;

// The BITPIX of a type of values, and the BZERO of the convention that stores it with an offset (standard §5.2.5).
struct storage
{
    openfits::bitpix bitpix = openfits::bitpix::uint8;
    double zero = 0.0;
};

storage storage_of(numeric_type type) noexcept
{
    switch (type) {
    case numeric_type::int8:
        return {.bitpix = bitpix::uint8, .zero = -128.0};
    case numeric_type::uint8:
        return {.bitpix = bitpix::uint8};
    case numeric_type::int16:
        return {.bitpix = bitpix::int16};
    case numeric_type::uint16:
        return {.bitpix = bitpix::int16, .zero = 32768.0};
    case numeric_type::int32:
        return {.bitpix = bitpix::int32};
    case numeric_type::uint32:
        return {.bitpix = bitpix::int32, .zero = 2147483648.0};
    case numeric_type::int64:
        return {.bitpix = bitpix::int64};
    case numeric_type::uint64:
        return {.bitpix = bitpix::int64, .zero = two_to_63};
    case numeric_type::float32:
        return {.bitpix = bitpix::float32};
    case numeric_type::float64:
        break;
    }
    return {.bitpix = bitpix::float64};
}

// A real value as the card of a keyword: an integer when it is one, so that BZERO = 32768 reads as the standard writes
// it, and a real otherwise.
card_value number(double value)
{
    if (value == std::trunc(value) && value >= -two_to_63 && value < two_to_63) {
        return static_cast<std::int64_t>(value);
    }
    if (value == std::trunc(value) && value >= two_to_63 && value < two_to_64) {
        return static_cast<std::uint64_t>(value);
    }
    return value;
}

std::string bitpix_comment(bitpix stored)
{
    switch (stored) {
    case bitpix::uint8:
        return "8-bit unsigned integers";
    case bitpix::int16:
        return "16-bit signed integers";
    case bitpix::int32:
        return "32-bit signed integers";
    case bitpix::int64:
        return "64-bit signed integers";
    case bitpix::float32:
        return "32-bit IEEE floating point";
    case bitpix::float64:
        break;
    }
    return "64-bit IEEE floating point";
}

// The types that the conventions of standard §5.2.5 store with an offset, and their names in the comment of BZERO.
struct offset_convention
{
    numeric_type type = numeric_type::int8;
    std::string_view name{};
};

constexpr std::array<offset_convention, 4> offset_conventions{{
    {.type = numeric_type::int8, .name = "signed bytes"},
    {.type = numeric_type::uint16, .name = "unsigned 16-bit integers"},
    {.type = numeric_type::uint32, .name = "unsigned 32-bit integers"},
    {.type = numeric_type::uint64, .name = "unsigned 64-bit integers"},
}};

// The comment of BZERO for the BITPIX given as data: the type that an offset of standard §5.2.5 stores, or what any
// other BZERO is.
std::string zero_comment(const image_info& image, bitpix data)
{
    if (image.scaling.scale == 1.0) {
        for (const offset_convention& convention : offset_conventions) {
            const storage stored = storage_of(convention.type);
            if (stored.bitpix == data && stored.zero == image.scaling.zero) {
                return std::string(convention.name);
            }
        }
    }
    return "physical value of a stored 0";
}

} // namespace

bitpix model_bitpix(const image_info& image)
{
    if (!image.bitpix) {
        throw usage_error(errc::invalid_argument, "the model of an image has no BITPIX");
    }
    return *image.bitpix;
}

image_info resolve_image(image_info info, std::optional<numeric_type> type, std::optional<std::uint64_t> values,
                         std::uint64_t hdu)
{
    const error_context where{.hdu = hdu};
    const std::vector<std::uint64_t>& axes = info.geometry.axes;
    if (axes.size() > 999) {
        throw validation_error(errc::invalid_structure,
                               "an image has at most 999 axes (standard §4.4.1.1), and that of unit " +
                                   std::to_string(hdu) + " has " + std::to_string(axes.size()),
                               {.hdu = hdu, .keyword = "NAXIS"});
    }
    // The product of the axes, which no data unit of more than 2^64 bytes can hold (standard §3.3.2, Eq. 1).
    std::uint64_t count = axes.empty() ? 0 : 1;
    for (const std::uint64_t length : axes) {
        if (length != 0 && count > std::numeric_limits<std::uint64_t>::max() / length) {
            throw validation_error(errc::invalid_structure,
                                   "the axes of the image of unit " + std::to_string(hdu) +
                                       " hold more than 2^64 values",
                                   {.hdu = hdu, .keyword = "NAXIS"});
        }
        count *= length;
    }
    if (values && *values != count) {
        throw usage_error(errc::invalid_argument,
                          "the data hold " + std::to_string(*values) + " values, and the axes of the image of unit " +
                              std::to_string(hdu) + " have " + std::to_string(count),
                          where);
    }
    if (!info.bitpix) {
        if (!type) {
            throw usage_error(
                errc::invalid_argument,
                "the reserved image of unit " + std::to_string(hdu) + " has no BITPIX, which no values give it", where);
        }
        if (info.scaling != scaling{}) {
            throw usage_error(errc::invalid_argument,
                              "the image of unit " + std::to_string(hdu) +
                                  " has a scaling and no BITPIX, which the scaling converts its values into",
                              where);
        }
        const storage stored = storage_of(*type);
        info.bitpix = stored.bitpix;
        info.scaling.zero = stored.zero;
    }
    if (!std::isfinite(info.scaling.scale) || info.scaling.scale == 0.0) {
        throw validation_error(errc::reserved_keyword,
                               "BSCALE of the image of unit " + std::to_string(hdu) + " is 0 or not finite",
                               {.hdu = hdu, .keyword = "BSCALE"});
    }
    if (!std::isfinite(info.scaling.zero)) {
        throw validation_error(errc::reserved_keyword,
                               "BZERO of the image of unit " + std::to_string(hdu) + " is not finite",
                               {.hdu = hdu, .keyword = "BZERO"});
    }
    if (info.blank) {
        const bitpix stored = *info.bitpix;
        if (static_cast<int>(stored) < 0) {
            throw validation_error(errc::reserved_keyword,
                                   "BLANK is for integer data alone (standard §4.4.2.5), and the image of unit " +
                                       std::to_string(hdu) + " holds floating-point data",
                                   {.hdu = hdu, .keyword = "BLANK"});
        }
        const std::int64_t value = *info.blank;
        const bool fits = stored == bitpix::uint8   ? value >= 0 && value <= 255
                          : stored == bitpix::int16 ? std::in_range<std::int16_t>(value)
                          : stored == bitpix::int32 ? std::in_range<std::int32_t>(value)
                                                    : true;
        if (!fits) {
            throw validation_error(errc::reserved_keyword,
                                   "BLANK of the image of unit " + std::to_string(hdu) + ", " + std::to_string(value) +
                                       ", is not a value of its BITPIX",
                                   {.hdu = hdu, .keyword = "BLANK"});
        }
    }
    if (count > (std::numeric_limits<std::uint64_t>::max() - block_size) / stored_size(*info.bitpix)) {
        throw validation_error(errc::invalid_structure,
                               "the data of the image of unit " + std::to_string(hdu) + " take more than 2^64 bytes",
                               {.hdu = hdu, .keyword = "NAXIS"});
    }
    return info;
}

image_values borrowed_values(std::span<const std::byte> data, numeric_type type)
{
    const std::size_t size = size_of(type);
    return [data, size](std::uint64_t first, std::size_t count) {
        return data.subspan(checked_cast<std::size_t>(first) * size, count * size);
    };
}

image_values streamed_values(const std::function<void(std::uint64_t, std::span<std::byte>)>& fill, numeric_type type)
{
    // The chunk, shared by the copies of the function, grows to the largest run asked for. Its memory is that of new[],
    // aligned for values of any numeric type.
    auto chunk = std::make_shared<scratch_buffer>(0);
    const std::size_t size = size_of(type);
    return [&fill, chunk, size](std::uint64_t first, std::size_t count) -> std::span<const std::byte> {
        const std::size_t bytes = count * size;
        if (chunk->bytes().size() < bytes) {
            *chunk = scratch_buffer(bytes);
        }
        const std::span<std::byte> values = chunk->bytes().first(bytes);
        fill(first, values);
        return values;
    };
}

std::uint64_t run_length(std::uint64_t first, std::uint64_t count, std::uint64_t row, std::size_t value_size,
                         std::size_t transfer) noexcept
{
    std::uint64_t run = std::max<std::uint64_t>(1, transfer / std::max<std::size_t>(value_size, 1));
    if (row > 0 && row <= run && first % row == 0) {
        run = run / row * row;
    }
    return std::min(run, count - first);
}

std::vector<card> image_cards(const image_info& image, bool primary)
{
    const bitpix stored = model_bitpix(image);
    const std::vector<std::uint64_t>& axes = image.geometry.axes;
    std::vector<card> cards;
    cards.reserve(axes.size() + 8);
    if (primary) {
        cards.emplace_back("SIMPLE", true, "conforms to the FITS standard");
    } else {
        cards.emplace_back("XTENSION", "IMAGE", "image extension");
    }
    cards.emplace_back("BITPIX", static_cast<std::int64_t>(stored), bitpix_comment(stored));
    cards.emplace_back("NAXIS", axes.size(), "number of axes");
    for (std::size_t i = 0; i < axes.size(); ++i) {
        const std::string axis = std::to_string(i + 1);
        cards.emplace_back("NAXIS" + axis, axes[i], "length of axis " + axis);
    }
    if (primary) {
        cards.emplace_back("EXTEND", true, "extensions may follow");
    } else {
        cards.emplace_back("PCOUNT", 0, "no parameters");
        cards.emplace_back("GCOUNT", 1, "one array");
    }
    if (image.scaling.scale != 1.0) {
        cards.emplace_back("BSCALE", number(image.scaling.scale), "physical = BZERO + BSCALE x stored");
    }
    if (image.scaling.zero != 0.0) {
        cards.emplace_back("BZERO", number(image.scaling.zero), zero_comment(image, stored));
    }
    if (image.blank) {
        cards.emplace_back("BLANK", *image.blank, "stored value of undefined values");
    }
    return cards;
}

std::uint64_t padded_data_size(bitpix stored, std::uint64_t count) noexcept
{
    const std::uint64_t size = count * stored_size(stored);
    return (size + block_size - 1) / block_size * block_size;
}

void write_image_data(output_sink& sink, const write_conversion& conversion, numeric_type type, std::uint64_t count,
                      std::uint64_t row, const image_values& values, const std::function<void(std::uint64_t)>& advance,
                      std::size_t transfer)
{
    const std::size_t from = size_of(type);
    const std::size_t to = stored_size(conversion.to);
    // Values written as they are: of one byte, or in the byte order of the file, without a conversion.
    const bool as_they_are = conversion.arithmetic == arithmetic::identity && stored_type(conversion.to) == type &&
                             (to == 1 || std::endian::native == std::endian::big);
    std::optional<scratch_buffer> staging;
    for (std::uint64_t done = 0; done < count;) {
        const auto run = checked_cast<std::size_t>(run_length(done, count, row, std::max(from, to), transfer));
        const std::span<const std::byte> given = values(done, run);
        if (as_they_are) {
            sink.write(given);
        } else {
            if (!staging) {
                staging.emplace(run * to);
            }
            const std::span<std::byte> bytes = staging->bytes().first(run * to);
            (void)convert_write(conversion, given.data(), bytes.data(), run);
            sink.write(bytes);
        }
        done += run;
        advance(std::uint64_t{run} * to);
    }
    // The zeros of the last block (standard §3.3.2).
    static constexpr std::array<std::byte, block_size> zeros{};
    const std::uint64_t size = count * to;
    const std::uint64_t fill = (block_size - (size % block_size)) % block_size;
    if (fill > 0) {
        sink.write(std::span<const std::byte>(zeros).first(fill));
        advance(fill);
    }
}

void write_reserved_data(output_sink& sink, bitpix stored, std::uint64_t count, bool undefined_values,
                         std::optional<std::int64_t> blank, const std::function<void(std::uint64_t)>& advance,
                         std::size_t transfer)
{
    const std::size_t width = stored_size(stored);
    const std::uint64_t padded = padded_data_size(stored, count);
    if (!undefined_values || count == 0) {
        sink.write_zeros(padded);
        advance(padded);
        return;
    }
    // The undefined value of the data, big-endian: BLANK, or the quiet NaN of IEEE 754 (standard §5.3).
    std::array<std::byte, 8> value{};
    switch (stored) {
    case bitpix::uint8:
        value[0] = static_cast<std::byte>(static_cast<std::uint8_t>(blank.value_or(0)));
        break;
    case bitpix::int16:
        store_big_endian(std::span(value).first<2>(), static_cast<std::uint16_t>(blank.value_or(0)));
        break;
    case bitpix::int32:
        store_big_endian(std::span(value).first<4>(), static_cast<std::uint32_t>(blank.value_or(0)));
        break;
    case bitpix::int64:
        store_big_endian(std::span(value).first<8>(), static_cast<std::uint64_t>(blank.value_or(0)));
        break;
    case bitpix::float32:
        store_big_endian(std::span(value).first<4>(), std::uint32_t{0x7FC0'0000});
        break;
    case bitpix::float64:
        store_big_endian(std::span(value).first<8>(), std::uint64_t{0x7FF8'0000'0000'0000});
        break;
    }
    const std::uint64_t per_chunk = std::max<std::uint64_t>(1, transfer / width);
    scratch_buffer chunk(checked_cast<std::size_t>(std::min(per_chunk, count) * width));
    const std::span<std::byte> bytes = chunk.bytes();
    for (std::size_t at = 0; at < bytes.size(); at += width) {
        std::copy_n(value.begin(), width, bytes.begin() + static_cast<std::ptrdiff_t>(at));
    }
    for (std::uint64_t done = 0; done < count;) {
        const std::uint64_t run = std::min(per_chunk, count - done);
        sink.write(bytes.first(checked_cast<std::size_t>(run * width)));
        advance(run * width);
        done += run;
    }
    // The zeros of the last block, fewer than a block, written as data as the fill of every unit is.
    static constexpr std::array<std::byte, block_size> zeros{};
    const std::uint64_t fill = padded - (count * width);
    sink.write(std::span<const std::byte>(zeros).first(fill));
    advance(fill);
}

} // namespace openfits::detail
