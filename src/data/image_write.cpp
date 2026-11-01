// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "data/image_write.h"

#include <openfits/error.h>

#include "core/scratch_buffer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
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

image_info resolve_image(image_info info, numeric_type type, std::uint64_t values, std::uint64_t hdu)
{
    const error_context where{.hdu = hdu};
    if (info.geometry.axes.size() > 999) {
        throw validation_error(errc::invalid_structure,
                               "an image has at most 999 axes (standard §4.4.1.1), and that of unit " +
                                   std::to_string(hdu) + " has " + std::to_string(info.geometry.axes.size()),
                               {.hdu = hdu, .keyword = "NAXIS"});
    }
    const std::uint64_t count = info.geometry.element_count();
    if (values != count) {
        throw usage_error(errc::invalid_argument,
                          "the data hold " + std::to_string(values) + " values, and the axes of the image of unit " +
                              std::to_string(hdu) + " have " + std::to_string(count),
                          where);
    }
    if (!info.bitpix) {
        if (info.scaling != scaling{}) {
            throw usage_error(errc::invalid_argument,
                              "the image of unit " + std::to_string(hdu) +
                                  " has a scaling and no BITPIX, which the scaling converts its values into",
                              where);
        }
        const storage stored = storage_of(type);
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
    if (count > std::numeric_limits<std::uint64_t>::max() / stored_size(*info.bitpix) - block_size) {
        throw usage_error(errc::arithmetic_overflow,
                          "the data of the image of unit " + std::to_string(hdu) + " do not fit in 64 bits", where);
    }
    return info;
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

void write_image_data(output_sink& sink, const write_conversion& conversion, numeric_type type,
                      std::span<const std::byte> data, const std::function<void(std::uint64_t)>& advance,
                      std::size_t transfer)
{
    const std::size_t from = size_of(type);
    const std::size_t to = stored_size(conversion.to);
    const std::size_t count = data.size() / from;
    // Values written as they are: of one byte, or in the byte order of the file, without a conversion.
    const bool as_they_are = conversion.arithmetic == arithmetic::identity && stored_type(conversion.to) == type &&
                             (to == 1 || std::endian::native == std::endian::big);
    const std::size_t piece = std::max<std::size_t>(1, transfer / std::max(from, to));
    if (as_they_are) {
        for (std::size_t done = 0; done < count;) {
            const std::size_t values = std::min(piece, count - done);
            sink.write(data.subspan(done * from, values * from));
            done += values;
            advance(values * to);
        }
    } else if (count > 0) {
        scratch_buffer staging(std::min(piece, count) * to);
        for (std::size_t done = 0; done < count;) {
            const std::size_t values = std::min(piece, count - done);
            const std::span<std::byte> bytes = staging.bytes().first(values * to);
            (void)convert_write(conversion, data.data() + (done * from), bytes.data(), values);
            sink.write(bytes);
            done += values;
            advance(values * to);
        }
    }
    // The zeros of the last block (standard §3.3.2).
    static constexpr std::array<std::byte, block_size> zeros{};
    const std::uint64_t size = std::uint64_t{count} * to;
    const std::uint64_t fill = (block_size - (size % block_size)) % block_size;
    if (fill > 0) {
        sink.write(std::span<const std::byte>(zeros).first(fill));
        advance(fill);
    }
}

} // namespace openfits::detail
