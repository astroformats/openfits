// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "compress/image_tiles.h"

#include <openfits/error.h>

#include "checksum/checksum.h"
#include "compress/gzip.h"
#include "compress/hcompress.h"
#include "compress/plio.h"
#include "compress/quantize.h"
#include "compress/rice.h"
#include "compress/shuffle.h"
#include "compress/tile_cache.h"
#include "compress/tile_grid.h"
#include "core/ascii.h"
#include "core/checked_math.h"
#include "core/endian.h"
#include "core/parallel.h"
#include "core/quote.h"
#include "data/conversion.h"
#include "data/image_description.h"
#include "data/image_read.h"
#include "header/record.h"
#include "io/range_check.h"
#include "table/table_description.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace openfits::detail {

namespace {

// The rows and the compressed bytes of tiles that lie less than this apart are read together.
constexpr std::uint64_t coalescing_gap = std::uint64_t{64} << 10;

// The decoded bytes that a batch of tiles holds at once, between which the progress is reported.
constexpr std::uint64_t batch_bytes = std::uint64_t{64} << 20;

[[noreturn]] void refuse(std::uint64_t hdu, const std::string& message, std::string_view keyword = {})
{
    throw invalid_data_error(errc::invalid_compression, message, {.hdu = hdu, .keyword = std::string(keyword)});
}

[[noreturn]] void throw_corrupt(const std::string& message)
{
    throw integrity_error(errc::corrupt_compressed_data, message);
}

// The BITPIX of the image, which the parse of its keywords always gives, from ZBITPIX.
bitpix stored_bitpix(const compressed_image& image) noexcept
{
    return image.image.bitpix.value_or(bitpix::uint8);
}

std::string indexed(std::string_view root, std::size_t index)
{
    return std::string(root) + std::to_string(index);
}

// The value of a keyword of the compression, which must convert to T when it is present.
template <typename T> std::optional<T> keyword_of(const header& cards, const std::string& keyword, std::uint64_t hdu)
{
    try {
        return cards.find<T>(keyword);
    } catch (const invalid_data_error&) {
        refuse(hdu, keyword + " does not hold a value of its type (standard §10.1)", keyword);
    }
}

template <typename T> T required(const header& cards, const std::string& keyword, std::uint64_t hdu)
{
    const std::optional<T> value = keyword_of<T>(cards, keyword, hdu);
    if (!value) {
        refuse(hdu, "the compressed image has no " + keyword + ", which standard §10.1.1 requires", keyword);
    }
    return *value;
}

// A string keyword without its trailing spaces.
std::optional<std::string> string_keyword(const header& cards, const std::string& keyword, std::uint64_t hdu)
{
    const std::optional<std::string> value = keyword_of<std::string>(cards, keyword, hdu);
    if (!value) {
        return std::nullopt;
    }
    return std::string(trim_trailing_spaces(*value));
}

std::size_t element_size(column_type type) noexcept
{
    switch (type) {
    case column_type::int16:
        return 2;
    case column_type::int32:
    case column_type::float32:
        return 4;
    case column_type::int64:
    case column_type::float64:
        return 8;
    case column_type::logical:
    case column_type::bit:
    case column_type::uint8:
    case column_type::character:
    case column_type::complex_float32:
    case column_type::complex_float64:
        break;
    }
    return 1;
}

bool is_variable(const column_format& format) noexcept
{
    return format.storage != column_storage::fixed;
}

bool is_integer(column_type type) noexcept
{
    return type == column_type::uint8 || type == column_type::int16 || type == column_type::int32 ||
           type == column_type::int64;
}

// The column of the table named name, of a format that accepts allows; nothing without one.
template <typename Accepts>
std::optional<tile_column> find_column(const table_info& table, std::string_view name, std::uint64_t hdu,
                                       Accepts accepts)
{
    const std::optional<std::size_t> index = table.find_column(name);
    if (!index) {
        return std::nullopt;
    }
    const column_info& column = table.columns[*index];
    if (!column.format || !accepts(*column.format)) {
        refuse(hdu, "the column " + quote(name) + " of the compressed image is of the format " +
                        (column.format ? column.format->to_string() : std::string("(none)")) +
                        ", which standard §10.1.3 does not give it");
    }
    return tile_column{.index = *index, .offset = column.offset, .format = *column.format};
}

// The name of a keyword in bytes 1 to 8 of the record of a card, which keeps its value and comment where they are.
card renamed(const card& original, std::string_view name)
{
    std::string text(original.text());
    text.resize(record_length, ' ');
    std::string field(name);
    field.resize(8, ' ');
    text.replace(0, 8, field);
    return card::parse(text);
}

// True when name is root followed by the digits of a positive number.
bool is_indexed(std::string_view name, std::string_view root) noexcept
{
    if (!name.starts_with(root) || name.size() == root.size()) {
        return false;
    }
    return std::ranges::all_of(name.substr(root.size()), [](char c) { return c >= '0' && c <= '9'; });
}

// The number after root, which is_indexed() found.
std::size_t index_after(std::string_view name, std::string_view root) noexcept
{
    std::size_t value = 0;
    for (const char c : name.substr(root.size())) {
        value = std::min<std::size_t>((value * 10) + static_cast<std::size_t>(c - '0'), 1'000'000);
    }
    return value;
}

// True for the keywords of the table and of the compression, which the restored image does not have.
bool is_table_keyword(std::string_view name, std::size_t fields) noexcept
{
    static constexpr std::array<std::string_view, 28> dropped{
        "XTENSION", "BITPIX",   "NAXIS",    "PCOUNT",   "GCOUNT",   "TFIELDS",  "THEAP",
        "ZIMAGE",   "ZCMPTYPE", "ZMASKCMP", "ZQUANTIZ", "ZDITHER0", "ZBLANK",   "ZSCALE",
        "ZZERO",    "ZBITPIX",  "ZNAXIS",   "ZSIMPLE",  "ZEXTEND",  "ZBLOCKED", "ZTENSION",
        "ZPCOUNT",  "ZGCOUNT",  "CHECKSUM", "DATASUM",  "SIMPLE",   "EXTEND",   "BLOCKED"};
    if (std::ranges::find(dropped, name) != dropped.end()) {
        return true;
    }
    for (const std::string_view root : {"NAXIS", "ZNAXIS", "ZTILE", "ZNAME", "ZVAL"}) {
        if (is_indexed(name, root)) {
            return true;
        }
    }
    static constexpr std::array<std::string_view, 8> columns{"TTYPE", "TFORM", "TUNIT", "TSCAL",
                                                             "TZERO", "TNULL", "TDISP", "TDIM"};
    return std::ranges::any_of(columns, [name, fields](std::string_view root) {
        return is_indexed(name, root) && index_after(name, root) <= fields;
    });
}

// The descriptor of a variable-length array in the field at field: its elements and its offset in the heap.
struct descriptor
{
    std::uint64_t count = 0;
    std::uint64_t offset = 0;
};

descriptor descriptor_at(std::span<const std::byte> field, const column_format& format, std::uint64_t hdu)
{
    if (format.storage == column_storage::variable_p) {
        return {.count = load_big_endian<std::uint32_t>(field.first<4>()),
                .offset = load_big_endian<std::uint32_t>(field.subspan<4, 4>())};
    }
    const auto count = static_cast<std::int64_t>(load_big_endian<std::uint64_t>(field.first<8>()));
    const auto offset = static_cast<std::int64_t>(load_big_endian<std::uint64_t>(field.subspan<8, 8>()));
    if (count < 0 || offset < 0) {
        throw invalid_data_error(errc::invalid_descriptor,
                                 "a descriptor of the compressed image holds a negative value (standard §7.3.5)",
                                 {.hdu = hdu});
    }
    return {.count = static_cast<std::uint64_t>(count), .offset = static_cast<std::uint64_t>(offset)};
}

// A range of bytes of the heap.
struct heap_range
{
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

// The bytes of the array of a descriptor of a column, which must lie within the heap.
heap_range range_of(const descriptor& array, const column_format& format, const compressed_image& image,
                    std::uint64_t hdu)
{
    const std::size_t size = element_size(format.type);
    if (array.count > std::numeric_limits<std::uint64_t>::max() / size || array.offset > image.heap_size ||
        array.count * size > image.heap_size - array.offset) {
        throw invalid_data_error(errc::invalid_descriptor,
                                 "a descriptor of the compressed image points outside its heap of " +
                                     std::to_string(image.heap_size) + " bytes (standard §7.3.5)",
                                 {.hdu = hdu});
    }
    return {.offset = array.offset, .size = array.count * size};
}

// A real of a fixed column of one element.
double real_at(std::span<const std::byte> field, column_type type)
{
    if (type == column_type::float32) {
        return static_cast<double>(std::bit_cast<float>(load_big_endian<std::uint32_t>(field.first<4>())));
    }
    if (type == column_type::float64) {
        return std::bit_cast<double>(load_big_endian<std::uint64_t>(field.first<8>()));
    }
    std::uint64_t bits = 0;
    const std::size_t size = element_size(type);
    for (std::size_t i = 0; i < size; ++i) {
        bits = (bits << 8U) | std::to_integer<std::uint64_t>(field[i]);
    }
    if (type == column_type::uint8) {
        return static_cast<double>(bits);
    }
    const auto shift = static_cast<unsigned>(64 - (size * 8));
    return static_cast<double>(static_cast<std::int64_t>(bits << shift) >> shift);
}

// An integer of a fixed column of one element.
std::int64_t integer_at(std::span<const std::byte> field, column_type type)
{
    std::uint64_t bits = 0;
    const std::size_t size = element_size(type);
    for (std::size_t i = 0; i < size; ++i) {
        bits = (bits << 8U) | std::to_integer<std::uint64_t>(field[i]);
    }
    if (type == column_type::uint8) {
        return static_cast<std::int64_t>(bits);
    }
    const auto shift = static_cast<unsigned>(64 - (size * 8));
    return static_cast<std::int64_t>(bits << shift) >> shift;
}

// The memory of the decoding of a tile, kept by a thread for the tiles it decodes, so that nothing is allocated for
// each tile.
struct tile_workspace
{
    std::vector<std::byte> bytes{};
    std::vector<std::uint8_t> u8{};
    std::vector<std::uint16_t> u16{};
    std::vector<std::uint32_t> u32{};
    std::vector<std::uint64_t> u64{};
    std::vector<std::int16_t> words{};
    std::vector<std::int32_t> i32{};
    std::vector<std::int64_t> i64{};
    std::vector<float> floats{};
    std::vector<double> doubles{};
    std::vector<std::uint8_t> mask{};
};

// The integers of count elements of size bytes, big-endian at in: signed, but bytes, which are unsigned (BITPIX 8).
void load_integers(std::span<const std::byte> in, std::size_t size, std::span<std::int64_t> out) noexcept
{
    const auto shift = static_cast<unsigned>(64 - (size * 8));
    for (std::size_t i = 0; i < out.size(); ++i) {
        std::uint64_t bits = 0;
        for (std::size_t b = 0; b < size; ++b) {
            bits = (bits << 8U) | std::to_integer<std::uint64_t>(in[(i * size) + b]);
        }
        out[i] = size == 1 ? static_cast<std::int64_t>(bits) : static_cast<std::int64_t>(bits << shift) >> shift;
    }
}

// The range of the values that BITPIX of integers stores.
std::pair<std::int64_t, std::int64_t> integer_range(bitpix stored) noexcept
{
    switch (stored) {
    case bitpix::uint8:
        return {0, 255};
    case bitpix::int16:
        return {std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max()};
    case bitpix::int32:
        return {std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()};
    case bitpix::int64:
    case bitpix::float32:
    case bitpix::float64:
        break;
    }
    return {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()};
}

[[noreturn]] void throw_beyond(std::int64_t value, bitpix stored)
{
    throw_corrupt("the tile holds the value " + std::to_string(value) + ", beyond its BITPIX of " +
                  std::to_string(static_cast<int>(stored)));
}

// The integers of values, which the range checks keep within U, as big-endian values of U.
template <std::unsigned_integral U> void store_as(std::span<const std::int64_t> values, std::span<std::byte> tile)
{
    for (std::size_t i = 0; i < values.size(); ++i) {
        store_big_endian(tile.subspan(i * sizeof(U)).template first<sizeof(U)>(), static_cast<U>(values[i]));
    }
}

// Stores integers as the big-endian values of BITPIX stored, which must hold them.
void store_integers(std::span<const std::int64_t> values, bitpix stored, std::span<std::byte> tile)
{
    const auto [low, high] = integer_range(stored);
    for (const std::int64_t value : values) {
        if (value < low || value > high) {
            throw_beyond(value, stored);
        }
    }
    switch (stored_size(stored)) {
    case 1:
        store_as<std::uint8_t>(values, tile);
        break;
    case 2:
        store_as<std::uint16_t>(values, tile);
        break;
    case 4:
        store_as<std::uint32_t>(values, tile);
        break;
    default:
        store_as<std::uint64_t>(values, tile);
        break;
    }
}

// Stores reals as the big-endian values of BITPIX -32 or -64.
template <typename T> void store_reals(std::span<const T> values, bitpix stored, std::span<std::byte> tile)
{
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (stored == bitpix::float32) {
            float value = 0.0F;
            if constexpr (std::is_same_v<T, float>) {
                value = values[i];
            } else {
                value = static_cast<float>(values[i]);
            }
            store_big_endian(tile.subspan(i * 4).template first<4>(), std::bit_cast<std::uint32_t>(value));
        } else {
            double value = 0.0;
            if constexpr (std::is_same_v<T, float>) {
                value = static_cast<double>(values[i]);
            } else {
                value = values[i];
            }
            store_big_endian(tile.subspan(i * 8).template first<8>(), std::bit_cast<std::uint64_t>(value));
        }
    }
}

// The size of what a gzip stream decompresses to, modulo 2^32, which its trailer gives; nothing for another stream.
std::optional<std::uint32_t> gzip_size(std::span<const std::byte> stream) noexcept
{
    if (stream.size() < 18 || stream[0] != std::byte{0x1F} || stream[1] != std::byte{0x8B}) {
        return std::nullopt;
    }
    std::uint32_t size = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        size |= std::to_integer<std::uint32_t>(stream[stream.size() - 4 + i]) << (8U * i);
    }
    return size;
}

// The bytes of each value of a gzip stream of count values: expected, or as CFITSIO reads them whatever ZBITPIX says,
// the first of 2, 4, 8 and 1 whose values make the size of the trailer.
std::size_t gzip_width(std::span<const std::byte> stream, std::size_t count, std::size_t expected) noexcept
{
    const std::optional<std::uint32_t> size = gzip_size(stream);
    if (!size) {
        return expected;
    }
    for (const std::size_t width : {expected, std::size_t{2}, std::size_t{4}, std::size_t{8}, std::size_t{1}}) {
        if (static_cast<std::uint32_t>(width * count) == *size) {
            return width;
        }
    }
    return expected;
}

// The values of a GZIP tile, of width bytes each, big-endian, into target, which holds them exactly; unshuffled for
// GZIP_2.
void inflate_into(std::span<const std::byte> stream, std::size_t width, bool shuffled, std::span<std::byte> target,
                  tile_workspace& workspace)
{
    if (!shuffled || width == 1) {
        gzip_decompress(stream, target);
        return;
    }
    workspace.u8.resize(target.size());
    gzip_decompress(stream, std::as_writable_bytes(std::span(workspace.u8)));
    unshuffle_bytes(std::as_bytes(std::span(workspace.u8)), target, width);
}

// The values of a GZIP tile, of width bytes each, big-endian, in workspace.bytes; unshuffled for GZIP_2.
std::span<const std::byte> inflate_values(std::span<const std::byte> stream, std::size_t count, std::size_t width,
                                          bool shuffled, tile_workspace& workspace)
{
    workspace.bytes.resize(count * width);
    inflate_into(stream, width, shuffled, workspace.bytes, workspace);
    return workspace.bytes;
}

// Big-endian values of width bytes as values of the floating-point type of stored, into tile.
void store_floating(std::span<const std::byte> values, std::size_t width, bitpix stored, std::span<std::byte> tile,
                    tile_workspace& workspace)
{
    const std::size_t count = values.size() / width;
    if (width == stored_size(stored)) {
        std::ranges::copy(values, tile.begin());
        return;
    }
    if (width == 4) {
        workspace.floats.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            workspace.floats[i] =
                std::bit_cast<float>(load_big_endian<std::uint32_t>(values.subspan(i * 4).first<4>()));
        }
        store_reals(std::span<const float>(workspace.floats), stored, tile);
        return;
    }
    if (width == 8) {
        workspace.doubles.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            workspace.doubles[i] =
                std::bit_cast<double>(load_big_endian<std::uint64_t>(values.subspan(i * 8).first<8>()));
        }
        store_reals(std::span<const double>(workspace.doubles), stored, tile);
        return;
    }
    throw_corrupt("the floating-point values of the tile are of " + std::to_string(width) + " bytes");
}

// Dequantizes codes into the floating-point values of stored, into tile.
void store_dequantized(std::span<const std::int32_t> codes, const compressed_image& image, std::uint64_t row,
                       const tile_payload& payload, bool wide, std::span<std::byte> tile, tile_workspace& workspace)
{
    std::optional<std::int32_t> null{};
    if (payload.null) {
        if (*payload.null < std::numeric_limits<std::int32_t>::min() ||
            *payload.null > std::numeric_limits<std::int32_t>::max()) {
            throw_corrupt("the ZBLANK of the tile, " + std::to_string(*payload.null) + ", is beyond 32-bit integers");
        }
        null = static_cast<std::int32_t>(*payload.null);
    }
    const dequantize_request request{.scaling = {.scale = payload.scale, .zero = payload.zero},
                                     .method = image.method,
                                     .row = row,
                                     .seed = image.seed,
                                     .null = null,
                                     .wide = wide};
    if (stored_bitpix(image) == bitpix::float32) {
        workspace.floats.resize(codes.size());
        dequantize(codes, request, std::span<float>(workspace.floats));
        copy_big_endian(std::as_bytes(std::span(workspace.floats)), tile, 4);
    } else {
        workspace.doubles.resize(codes.size());
        dequantize(codes, request, std::span<double>(workspace.doubles));
        copy_big_endian(std::as_bytes(std::span(workspace.doubles)), tile, 8);
    }
}

// The values of a RICE_1 tile of BYTEPIX bytes, in the unsigned integers of that size in workspace, as bytes in the
// byte order of the host.
std::span<const std::byte> rice_values(const compressed_image& image, std::span<const std::byte> bytes,
                                       std::size_t count, tile_workspace& workspace)
{
    switch (image.bytepix) {
    case 1:
        workspace.u8.resize(count);
        rice_decode(bytes, image.block_size, std::span<std::uint8_t>(workspace.u8));
        return std::as_bytes(std::span(workspace.u8));
    case 2:
        workspace.u16.resize(count);
        rice_decode(bytes, image.block_size, std::span<std::uint16_t>(workspace.u16));
        return std::as_bytes(std::span(workspace.u16));
    case 4:
        workspace.u32.resize(count);
        rice_decode(bytes, image.block_size, std::span<std::uint32_t>(workspace.u32));
        return std::as_bytes(std::span(workspace.u32));
    default:
        workspace.u64.resize(count);
        rice_decode(bytes, image.block_size, std::span<std::uint64_t>(workspace.u64));
        return std::as_bytes(std::span(workspace.u64));
    }
}

// The stored values of a tile of an integer image whose algorithm gives values of the width of the stored ones, into
// tile as they are: the values that decode_integers() and store_integers() give, which always lie within the range of
// the stored type, without the 64-bit integers between them. False, and nothing decoded, for another tile.
bool decode_stored(const compressed_image& image, std::span<const std::byte> bytes, std::size_t width,
                   std::span<std::byte> tile, tile_workspace& workspace)
{
    const std::size_t count = tile.size() / width;
    switch (image.algorithm) {
    case tile_algorithm::rice:
        if (image.bytepix != width) {
            return false;
        }
        copy_big_endian(rice_values(image, bytes, count, workspace), tile, width);
        return true;
    case tile_algorithm::gzip_1:
    case tile_algorithm::gzip_2:
        if (gzip_width(bytes, count, width) != width) {
            return false;
        }
        inflate_into(bytes, width, image.algorithm == tile_algorithm::gzip_2, tile, workspace);
        return true;
    case tile_algorithm::plio:
    case tile_algorithm::hcompress:
    case tile_algorithm::nocompress:
        break;
    }
    return false;
}

// The codes of a quantized tile whose algorithm gives 32-bit integers, into workspace.i32: those that decode_integers()
// and codes_of() give, without the 64-bit integers between them. Nothing, and nothing decoded, for another tile.
std::optional<std::span<const std::int32_t>> decode_codes(const compressed_image& image,
                                                          std::span<const std::byte> bytes, std::size_t count,
                                                          tile_workspace& workspace)
{
    if (image.algorithm == tile_algorithm::rice && image.bytepix == 4) {
        workspace.u32.resize(count);
        rice_decode(bytes, image.block_size, std::span<std::uint32_t>(workspace.u32));
        workspace.i32.resize(count);
        std::ranges::transform(workspace.u32, workspace.i32.begin(),
                               [](std::uint32_t value) { return static_cast<std::int32_t>(value); });
        return workspace.i32;
    }
    if ((image.algorithm == tile_algorithm::gzip_1 || image.algorithm == tile_algorithm::gzip_2) &&
        gzip_width(bytes, count, 4) == 4) {
        workspace.i32.resize(count);
        const std::span<std::byte> codes = std::as_writable_bytes(std::span(workspace.i32));
        inflate_into(bytes, 4, image.algorithm == tile_algorithm::gzip_2, codes, workspace);
        convert_big_endian(codes, 4);
        return workspace.i32;
    }
    return std::nullopt;
}

// Integers as the codes of a quantized tile, which must hold 32 bits.
std::span<const std::int32_t> codes_of(std::span<const std::int64_t> values, tile_workspace& workspace)
{
    workspace.i32.resize(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (values[i] < std::numeric_limits<std::int32_t>::min() ||
            values[i] > std::numeric_limits<std::int32_t>::max()) {
            throw_corrupt("the quantized tile holds the code " + std::to_string(values[i]) +
                          ", beyond 32-bit integers");
        }
        workspace.i32[i] = static_cast<std::int32_t>(values[i]);
    }
    return workspace.i32;
}

// The integers of a tile from its compressed bytes, by the algorithm of the image, into workspace.i64; the codes of a
// quantized tile, or the stored values of an integer image. Returns whether they are 32-bit integers, among which
// SUBTRACTIVE_DITHER_2 reserves the zero.
bool decode_integers(const compressed_image& image, std::span<const std::uint64_t> size,
                     std::span<const std::byte> bytes, std::size_t count, tile_workspace& workspace)
{
    std::vector<std::int64_t>& values = workspace.i64;
    values.resize(count);
    const bool quantized = image.quantized;
    const std::size_t expected = quantized ? 4 : stored_size(stored_bitpix(image));
    switch (image.algorithm) {
    case tile_algorithm::rice:
        switch (image.bytepix) {
        case 1:
            workspace.u8.resize(count);
            rice_decode(bytes, image.block_size, std::span<std::uint8_t>(workspace.u8));
            std::ranges::copy(workspace.u8, values.begin());
            return false;
        case 2:
            workspace.u16.resize(count);
            rice_decode(bytes, image.block_size, std::span<std::uint16_t>(workspace.u16));
            std::ranges::transform(workspace.u16, values.begin(),
                                   [](std::uint16_t value) { return std::int64_t{static_cast<std::int16_t>(value)}; });
            return false;
        case 4:
            workspace.u32.resize(count);
            rice_decode(bytes, image.block_size, std::span<std::uint32_t>(workspace.u32));
            std::ranges::transform(workspace.u32, values.begin(),
                                   [](std::uint32_t value) { return std::int64_t{static_cast<std::int32_t>(value)}; });
            return true;
        default:
            workspace.u64.resize(count);
            rice_decode(bytes, image.block_size, std::span<std::uint64_t>(workspace.u64));
            std::ranges::transform(workspace.u64, values.begin(),
                                   [](std::uint64_t value) { return static_cast<std::int64_t>(value); });
            return false;
        }
    case tile_algorithm::gzip_1:
    case tile_algorithm::gzip_2: {
        const std::size_t width = gzip_width(bytes, count, expected);
        if (width != 1 && width != 2 && width != 4 && width != 8) {
            throw_corrupt("the GZIP tile holds values of " + std::to_string(width) + " bytes");
        }
        const std::span<const std::byte> inflated =
            inflate_values(bytes, count, width, image.algorithm == tile_algorithm::gzip_2, workspace);
        load_integers(inflated, width, values);
        return width == 4;
    }
    case tile_algorithm::plio: {
        // The words of 1PI, big-endian.
        if (bytes.size() % 2 != 0) {
            throw_corrupt("the PLIO_1 list of the tile holds a part of a word");
        }
        workspace.words.resize(bytes.size() / 2);
        for (std::size_t i = 0; i < workspace.words.size(); ++i) {
            workspace.words[i] =
                static_cast<std::int16_t>(load_big_endian<std::uint16_t>(bytes.subspan(i * 2).first<2>()));
        }
        workspace.i32.resize(count);
        plio_decode(workspace.words, workspace.i32);
        const std::int64_t offset = image.plio_offset ? 32768 : 0;
        std::ranges::transform(workspace.i32, values.begin(),
                               [offset](std::int32_t value) { return std::int64_t{value} - offset; });
        return true;
    }
    case tile_algorithm::hcompress: {
        // Two-dimensional tiles: the first axis longer than 1 is a row, the product of the others the rows.
        std::size_t row = 1;
        std::size_t rows = 1;
        bool first = true;
        for (const std::uint64_t length : size) {
            if (length <= 1) {
                continue;
            }
            if (first) {
                row = length;
                first = false;
            } else {
                rows *= length;
            }
        }
        hcompress_decode(bytes, rows, row, image.smooth, std::span<std::int64_t>(values));
        // A lossy tile restores values a little beyond those that it compressed, which lay within the integers of the
        // image, or the codes of 32 bits (standard §10.4.4): such a value is taken to the nearest of them.
        const auto [low, high] = quantized
                                     ? std::pair<std::int64_t, std::int64_t>{std::numeric_limits<std::int32_t>::min(),
                                                                             std::numeric_limits<std::int32_t>::max()}
                                     : integer_range(stored_bitpix(image));
        for (std::int64_t& value : values) {
            value = std::clamp(value, low, high);
        }
        return true;
    }
    case tile_algorithm::nocompress:
        if (bytes.size() != count * expected) {
            throw_corrupt("the NOCOMPRESS tile holds " + std::to_string(bytes.size()) + " bytes, and its " +
                          std::to_string(count) + " values " + std::to_string(count * expected));
        }
        load_integers(bytes, expected, values);
        return expected == 4;
    }
    return true;
}

// Sets the pixels of the tile that its null pixel mask marks undefined (standard §10.2.2): NaN, or BLANK.
void apply_mask(const compressed_image& image, std::span<const std::byte> mask, std::span<std::byte> tile,
                tile_workspace& workspace)
{
    const std::size_t stored = stored_size(stored_bitpix(image));
    const std::size_t count = tile.size() / stored;
    std::vector<std::uint8_t>& marks = workspace.mask;
    marks.resize(count);
    switch (image.mask_algorithm) {
    case tile_algorithm::plio: {
        if (mask.size() % 2 != 0) {
            throw_corrupt("the PLIO_1 list of the null pixel mask holds a part of a word");
        }
        std::vector<std::int16_t> words(mask.size() / 2);
        for (std::size_t i = 0; i < words.size(); ++i) {
            words[i] = static_cast<std::int16_t>(load_big_endian<std::uint16_t>(mask.subspan(i * 2).first<2>()));
        }
        std::vector<std::int32_t> values(count);
        plio_decode(words, values);
        std::ranges::transform(values, marks.begin(),
                               [](std::int32_t value) { return static_cast<std::uint8_t>(value != 0 ? 1 : 0); });
        break;
    }
    case tile_algorithm::gzip_1:
    case tile_algorithm::gzip_2:
    case tile_algorithm::nocompress: {
        std::vector<std::byte> inflated;
        std::size_t width = 1;
        if (image.mask_algorithm == tile_algorithm::nocompress) {
            width = mask.size() / std::max<std::size_t>(count, 1);
            inflated.assign(mask.begin(), mask.end());
        } else {
            width = gzip_width(mask, count, 1);
            inflated.resize(count * width);
            gzip_decompress(mask, inflated);
        }
        if (width == 0 || inflated.size() != count * width) {
            throw_corrupt("the null pixel mask does not hold a value for each pixel of its tile");
        }
        for (std::size_t i = 0; i < count; ++i) {
            const auto values = std::span(inflated).subspan(i * width, width);
            marks[i] = std::ranges::any_of(values, [](std::byte b) { return b != std::byte{0}; }) ? 1 : 0;
        }
        break;
    }
    case tile_algorithm::rice: {
        std::vector<std::uint8_t> values(count);
        rice_decode(mask, rice_block_32, std::span<std::uint8_t>(values));
        std::ranges::transform(values, marks.begin(),
                               [](std::uint8_t value) { return static_cast<std::uint8_t>(value != 0 ? 1 : 0); });
        break;
    }
    case tile_algorithm::hcompress: {
        std::vector<std::int64_t> values(count);
        hcompress_decode(mask, 1, count, false, std::span<std::int64_t>(values));
        std::ranges::transform(values, marks.begin(),
                               [](std::int64_t value) { return static_cast<std::uint8_t>(value != 0 ? 1 : 0); });
        break;
    }
    }
    const bitpix type = stored_bitpix(image);
    std::vector<std::byte> null_value(stored);
    if (type == bitpix::float32) {
        store_big_endian(std::span(null_value).first<4>(),
                         std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN()));
    } else if (type == bitpix::float64) {
        store_big_endian(std::span(null_value).first<8>(),
                         std::bit_cast<std::uint64_t>(std::numeric_limits<double>::quiet_NaN()));
    } else if (image.image.blank) {
        const std::array<std::int64_t, 1> blank{*image.image.blank};
        store_integers(blank, type, null_value);
    } else {
        // An integer image without BLANK has no value for undefined pixels.
        return;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (marks[i] != 0) {
            std::ranges::copy(null_value, tile.begin() + static_cast<std::ptrdiff_t>(i * stored));
        }
    }
}

void decode_tile_into(const compressed_image& image, std::uint64_t index, std::span<const std::uint64_t> size,
                      const tile_payload& payload, std::span<std::byte> tile, tile_workspace& workspace)
{
    const bitpix stored = stored_bitpix(image);
    const std::size_t width = stored_size(stored);
    const std::size_t count = tile.size() / width;
    const bool floating = static_cast<int>(stored) < 0;
    switch (payload.column) {
    case tile_payload::holder::uncompressed: {
        // The values as a column of their type holds them: integers, or reals of either size.
        const std::size_t from = element_size(payload.uncompressed_type);
        if (payload.bytes.size() != count * from) {
            throw_corrupt("the UNCOMPRESSED_DATA of the tile holds " + std::to_string(payload.bytes.size()) +
                          " bytes, and its " + std::to_string(count) + " values " + std::to_string(count * from));
        }
        if (payload.uncompressed_type == column_type::float32 || payload.uncompressed_type == column_type::float64) {
            if (!floating) {
                refuse(0, "the UNCOMPRESSED_DATA of an integer image holds reals");
            }
            store_floating(payload.bytes, from, stored, tile, workspace);
        } else {
            workspace.i64.resize(count);
            load_integers(payload.bytes, from, workspace.i64);
            if (floating) {
                workspace.doubles.resize(count);
                std::ranges::transform(workspace.i64, workspace.doubles.begin(),
                                       [](std::int64_t value) { return static_cast<double>(value); });
                store_reals(std::span<const double>(workspace.doubles), stored, tile);
            } else {
                store_integers(workspace.i64, stored, tile);
            }
        }
        break;
    }
    case tile_payload::holder::gzip_compressed: {
        // The values of a tile that could not be quantized, never shuffled.
        if (!floating) {
            refuse(0, "the GZIP_COMPRESSED_DATA of an integer image holds a tile");
        }
        const std::size_t from = gzip_width(payload.bytes, count, width);
        if (from != 4 && from != 8) {
            throw_corrupt("the GZIP_COMPRESSED_DATA of the tile holds values of " + std::to_string(from) + " bytes");
        }
        store_floating(inflate_values(payload.bytes, count, from, false, workspace), from, stored, tile, workspace);
        break;
    }
    case tile_payload::holder::compressed:
        if (floating && !image.quantized) {
            if (image.algorithm == tile_algorithm::nocompress) {
                if (payload.bytes.size() != count * width) {
                    throw_corrupt("the NOCOMPRESS tile holds " + std::to_string(payload.bytes.size()) +
                                  " bytes, and its values " + std::to_string(count * width));
                }
                std::ranges::copy(payload.bytes, tile.begin());
                break;
            }
            const std::size_t from = gzip_width(payload.bytes, count, width);
            if (from != 4 && from != 8) {
                throw_corrupt("the GZIP tile holds floating-point values of " + std::to_string(from) + " bytes");
            }
            if (from == width) {
                // The values as they are, straight into the tile.
                inflate_into(payload.bytes, from, image.algorithm == tile_algorithm::gzip_2, tile, workspace);
                break;
            }
            store_floating(
                inflate_values(payload.bytes, count, from, image.algorithm == tile_algorithm::gzip_2, workspace), from,
                stored, tile, workspace);
            break;
        }
        if (floating) {
            if (const std::optional<std::span<const std::int32_t>> codes =
                    decode_codes(image, payload.bytes, count, workspace)) {
                store_dequantized(*codes, image, index + 1, payload, true, tile, workspace);
                break;
            }
        } else if (decode_stored(image, payload.bytes, width, tile, workspace)) {
            break;
        }
        {
            const bool wide = decode_integers(image, size, payload.bytes, count, workspace);
            if (floating) {
                store_dequantized(codes_of(workspace.i64, workspace), image, index + 1, payload, wide, tile, workspace);
            } else {
                store_integers(workspace.i64, stored, tile);
            }
        }
        break;
    }
    if (!payload.mask.empty()) {
        apply_mask(image, payload.mask, tile, workspace);
    }
}

// The rows of the tiles of a read, and the payloads of the tiles in their bytes.
class tile_rows
{
public:
    tile_rows(const thread_safe_source& source, const hdu_info& unit, const compressed_image& image)
        : source_(source), unit_(unit), image_(image)
    {}

    // Reads the rows of tiles, which are in increasing order, in runs of rows that lie close together.
    void read(std::span<const std::uint64_t> tiles)
    {
        rows_.clear();
        buffers_.clear();
        const std::uint64_t size = image_.row_size;
        const std::uint64_t gap_rows = std::max<std::uint64_t>(1, coalescing_gap / std::max<std::uint64_t>(size, 1));
        std::size_t i = 0;
        while (i < tiles.size()) {
            std::size_t j = i + 1;
            while (j < tiles.size() && tiles[j] - tiles[j - 1] <= gap_rows) {
                ++j;
            }
            const std::uint64_t first = tiles[i];
            const std::uint64_t last = tiles[j - 1];
            const std::span<const std::byte> bytes =
                fetch(unit_.data_offset() + (first * size), (last - first + 1) * size);
            for (std::size_t k = i; k < j; ++k) {
                rows_.push_back(bytes.subspan((tiles[k] - first) * size, size));
            }
            i = j;
        }
    }

    [[nodiscard]] std::span<const std::byte> row(std::size_t position) const noexcept
    {
        return rows_[position];
    }

    // Reads the bytes of the heap of the ranges, in runs of ranges that lie close together, and gives each its span.
    std::vector<std::span<const std::byte>> read_heap(std::span<const heap_range> ranges)
    {
        std::vector<std::size_t> order(ranges.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::ranges::sort(order,
                          [&ranges](std::size_t a, std::size_t b) { return ranges[a].offset < ranges[b].offset; });
        std::vector<std::span<const std::byte>> spans(ranges.size());
        std::size_t i = 0;
        while (i < order.size()) {
            const std::uint64_t start = ranges[order[i]].offset;
            std::uint64_t end = start + ranges[order[i]].size;
            std::size_t j = i + 1;
            while (j < order.size() && ranges[order[j]].offset <= end + coalescing_gap &&
                   ranges[order[j]].offset + ranges[order[j]].size - start <= batch_bytes) {
                end = std::max(end, ranges[order[j]].offset + ranges[order[j]].size);
                ++j;
            }
            const std::span<const std::byte> bytes =
                end == start ? std::span<const std::byte>()
                             : fetch(unit_.data_offset() + image_.heap_offset + start, end - start);
            for (std::size_t k = i; k < j; ++k) {
                const heap_range& range = ranges[order[k]];
                spans[order[k]] =
                    range.size == 0 ? std::span<const std::byte>() : bytes.subspan(range.offset - start, range.size);
            }
            i = j;
        }
        return spans;
    }

private:
    std::span<const std::byte> fetch(std::uint64_t offset, std::uint64_t size)
    {
        const std::span<const std::byte> held = source_.bytes();
        if (!held.empty()) {
            check_read_range(source_.size(), offset, size, source_.description());
            return held.subspan(offset, size);
        }
        buffers_.emplace_back(checked_cast<std::size_t>(size));
        const std::span<std::byte> bytes = buffers_.back().bytes();
        source_.read(offset, bytes);
        return bytes;
    }

    const thread_safe_source& source_;
    const hdu_info& unit_;
    const compressed_image& image_;
    std::vector<std::span<const std::byte>> rows_{};
    std::vector<scratch_buffer> buffers_{};
};

// The payload of a tile from its row, and the ranges of the heap that it needs.
struct tile_request
{
    tile_payload payload{};
    heap_range data{};
    heap_range mask{};
};

tile_request request_of(const compressed_image& image, std::span<const std::byte> row, std::uint64_t index,
                        std::uint64_t hdu)
{
    tile_request request;
    tile_payload& payload = request.payload;
    const auto array_of = [&](const tile_column& column) {
        return descriptor_at(row.subspan(column.offset), column.format, hdu);
    };
    const descriptor compressed = array_of(image.compressed);
    if (compressed.count > 0) {
        request.data = range_of(compressed, image.compressed.format, image, hdu);
        payload.column = tile_payload::holder::compressed;
    } else {
        // An empty cell of COMPRESSED_DATA: the tile is in the column whose cell is not empty.
        std::optional<descriptor> other;
        if (image.gzip_compressed) {
            const descriptor gzip = array_of(*image.gzip_compressed);
            if (gzip.count > 0) {
                request.data = range_of(gzip, image.gzip_compressed->format, image, hdu);
                payload.column = tile_payload::holder::gzip_compressed;
                other = gzip;
            }
        }
        if (!other && image.uncompressed) {
            const descriptor raw = array_of(*image.uncompressed);
            if (raw.count > 0) {
                request.data = range_of(raw, image.uncompressed->format, image, hdu);
                payload.column = tile_payload::holder::uncompressed;
                payload.uncompressed_type = image.uncompressed->format.type;
                other = raw;
            }
        }
        if (!other) {
            throw integrity_error(errc::corrupt_compressed_data,
                                  "tile " + std::to_string(index + 1) + " of the compressed image of unit " +
                                      std::to_string(hdu) + " has no data in any column",
                                  {.hdu = hdu});
        }
    }
    if (image.mask) {
        const descriptor mask = array_of(*image.mask);
        if (mask.count > 0) {
            request.mask = range_of(mask, image.mask->format, image, hdu);
        }
    }
    payload.scale = image.scales ? real_at(row.subspan(image.scales->offset), image.scales->format.type)
                                 : image.scale.value_or(1.0);
    payload.zero =
        image.zeros ? real_at(row.subspan(image.zeros->offset), image.zeros->format.type) : image.zero.value_or(0.0);
    payload.null = image.nulls ? std::optional(integer_at(row.subspan(image.nulls->offset), image.nulls->format.type))
                               : image.null;
    return request;
}

// The progress of a read, reported to the progress function of its options on the calling thread.
class tile_progress
{
public:
    tile_progress(const progress_function& function, std::uint64_t total, std::uint64_t hdu)
        : function_(function), total_(total), hdu_(hdu)
    {}

    void advance(std::uint64_t bytes)
    {
        done_ += bytes;
        report();
    }

    void report() const
    {
        if (function_ && !function_(done_, total_)) {
            throw cancelled_error(errc::cancelled,
                                  "the read of the values of unit " + std::to_string(hdu_) + " was cancelled",
                                  {.hdu = hdu_});
        }
    }

private:
    const progress_function& function_;
    std::uint64_t total_;
    std::uint64_t done_ = 0;
    std::uint64_t hdu_;
};

} // namespace

std::optional<tile_algorithm> algorithm_of(std::string_view name) noexcept
{
    name = trim_trailing_spaces(name);
    if (name == "RICE_1" || name == "RICE_ONE") {
        return tile_algorithm::rice;
    }
    if (name == "GZIP_1") {
        return tile_algorithm::gzip_1;
    }
    if (name == "GZIP_2") {
        return tile_algorithm::gzip_2;
    }
    if (name == "PLIO_1") {
        return tile_algorithm::plio;
    }
    if (name == "HCOMPRESS_1") {
        return tile_algorithm::hcompress;
    }
    if (name == "NOCOMPRESS") {
        return tile_algorithm::nocompress;
    }
    return std::nullopt;
}

std::string_view algorithm_name(tile_algorithm algorithm) noexcept
{
    switch (algorithm) {
    case tile_algorithm::rice:
        return "RICE_1";
    case tile_algorithm::gzip_1:
        return "GZIP_1";
    case tile_algorithm::gzip_2:
        return "GZIP_2";
    case tile_algorithm::plio:
        return "PLIO_1";
    case tile_algorithm::hcompress:
        return "HCOMPRESS_1";
    case tile_algorithm::nocompress:
        break;
    }
    return "NOCOMPRESS";
}

void analyze_compressed_image(const header& cards, std::uint64_t index, std::uint64_t header_offset,
                              diagnostic_log& log)
{
    const auto about = [&](std::string_view keyword) {
        const std::optional<std::size_t> position = cards.index_of(keyword);
        return error_context{.hdu = index,
                             .keyword = std::string(keyword),
                             .offset = header_offset + (position.value_or(0) * record_length)};
    };
    const auto string_of = [&cards](std::string_view keyword) -> std::string {
        const std::optional<std::size_t> position = cards.index_of(keyword);
        if (!position || cards[*position].value().kind() != value_kind::string) {
            return {};
        }
        return std::string(trim_trailing_spaces(cards[*position].value().get<std::string>()));
    };
    const auto integer_of = [&cards](std::string_view keyword) -> std::optional<std::int64_t> {
        const std::optional<std::size_t> position = cards.index_of(keyword);
        if (!position || cards[*position].value().kind() != value_kind::integer) {
            return std::nullopt;
        }
        return cards[*position].value().get<std::int64_t>();
    };
    const std::string name = string_of("ZCMPTYPE");
    if (name == "RICE_ONE") {
        log.info(errc::invalid_compression, "ZCMPTYPE is 'RICE_ONE', the name of RICE_1 in old files, read as RICE_1",
                 about("ZCMPTYPE"));
    }
    if (name == "RICE_1" || name == "RICE_ONE") {
        if (string_of("ZNAME2") == "NOISEBIT") {
            log.warning(errc::invalid_compression,
                        "ZNAME2 is 'NOISEBIT', a parameter of very old files, and RICE_1 is read with BYTEPIX 4",
                        about("ZNAME2"));
        }
        const std::optional<std::int64_t> block = integer_of("ZVAL1");
        const std::optional<std::int64_t> bytes = integer_of("ZVAL2");
        if (block && bytes && *block < 16 && *bytes > 8) {
            log.warning(errc::invalid_compression,
                        "ZVAL1 and ZVAL2 of RICE_1 are reversed, " + std::to_string(*block) + " and " +
                            std::to_string(*bytes) + ", as old files have them, and read as BLOCKSIZE " +
                            std::to_string(*bytes) + " and BYTEPIX " + std::to_string(*block),
                        about("ZVAL1"));
        }
    }
    for (const std::string_view keyword : {std::string_view("ZSCALE"), std::string_view("ZZERO")}) {
        if (cards.index_of(keyword)) {
            log.info(errc::invalid_compression,
                     std::string(keyword) + " is a keyword, as old files have it, rather than a column: a constant of "
                                            "every tile (standard §10.1.3)",
                     about(keyword));
        }
    }
}

compressed_image describe_compressed_image(const hdu_info& unit, bool with_header)
{
    const std::uint64_t hdu = unit.index();
    const header& cards = unit.header();
    const table_info table = describe_table(unit, false);
    compressed_image image;
    image.rows = table.rows;
    image.row_size = table.row_size;
    image.heap_offset = table.heap_offset;
    image.heap_size = table.heap_size;

    const auto zbitpix = required<std::int64_t>(cards, "ZBITPIX", hdu);
    const std::optional<bitpix> stored = bitpix_of(zbitpix);
    if (!stored) {
        refuse(hdu, "ZBITPIX is " + std::to_string(zbitpix) + ", which is not a BITPIX of standard §4.4.1.1",
               "ZBITPIX");
    }
    image.image.bitpix = stored;
    const auto naxis = required<std::int64_t>(cards, "ZNAXIS", hdu);
    if (naxis < 0 || naxis > 999) {
        refuse(hdu, "ZNAXIS is " + std::to_string(naxis) + ", beyond the 999 axes of standard §4.4.1.1", "ZNAXIS");
    }
    std::vector<std::uint64_t>& axes = image.image.geometry.axes;
    std::uint64_t tiles = naxis == 0 ? 0 : 1;
    for (std::int64_t n = 1; n <= naxis; ++n) {
        const std::string keyword = indexed("ZNAXIS", static_cast<std::size_t>(n));
        const auto length = required<std::int64_t>(cards, keyword, hdu);
        if (length < 0) {
            refuse(hdu, keyword + " is " + std::to_string(length) + ", a negative length", keyword);
        }
        axes.push_back(static_cast<std::uint64_t>(length));
        // ZTILEn: one row of the image without them (standard §10.1.2).
        const std::string tile_keyword = indexed("ZTILE", static_cast<std::size_t>(n));
        const std::int64_t tile =
            keyword_of<std::int64_t>(cards, tile_keyword, hdu).value_or(n == 1 ? std::max<std::int64_t>(length, 1) : 1);
        if (tile < 1) {
            refuse(hdu, tile_keyword + " is " + std::to_string(tile) + ", where a tile has at least one pixel",
                   tile_keyword);
        }
        image.tile.push_back(std::min(static_cast<std::uint64_t>(tile), std::max<std::uint64_t>(axes.back(), 1)));
        const std::uint64_t along = axes.back() == 0 ? 0 : ((axes.back() - 1) / image.tile.back()) + 1;
        if (along != 0 && tiles > std::numeric_limits<std::uint64_t>::max() / along) {
            refuse(hdu, "the number of tiles of the compressed image does not fit in 64 bits");
        }
        tiles *= along;
    }
    if (tiles != image.rows) {
        refuse(hdu,
               "the table of the compressed image has " + std::to_string(image.rows) + " rows, and its image " +
                   std::to_string(tiles) + " tiles (standard §10.1.2)",
               "NAXIS2");
    }
    image.image.scaling.scale = cards.find<double>("BSCALE").value_or(1.0);
    image.image.scaling.zero = cards.find<double>("BZERO").value_or(0.0);
    const bool floating = static_cast<int>(*stored) < 0;
    if (!floating) {
        image.image.blank = cards.find<std::int64_t>("BLANK");
    }

    const std::optional<std::string> name = string_keyword(cards, "ZCMPTYPE", hdu);
    if (!name) {
        refuse(hdu, "the compressed image has no ZCMPTYPE, which standard §10.1.1 requires", "ZCMPTYPE");
    }
    const std::optional<tile_algorithm> algorithm = algorithm_of(*name);
    if (!algorithm) {
        throw unsupported_error(errc::unsupported_compression,
                                "ZCMPTYPE is " + quote(*name) + ", which is not an algorithm of standard Table 36",
                                {.hdu = hdu, .keyword = "ZCMPTYPE"});
    }
    image.algorithm = *algorithm;
    if (image.algorithm == tile_algorithm::rice) {
        // BLOCKSIZE and BYTEPIX by position, as CFITSIO reads them, with the forms of old files: NOISEBIT, a name of
        // 4 bytes per pixel, and the two values reversed.
        std::int64_t block = keyword_of<std::int64_t>(cards, "ZVAL1", hdu).value_or(32);
        std::int64_t bytes = keyword_of<std::int64_t>(cards, "ZVAL2", hdu).value_or(4);
        if (string_keyword(cards, "ZNAME2", hdu).value_or("") == "NOISEBIT") {
            bytes = 4;
        }
        if (block < 16 && bytes > 8) {
            std::swap(block, bytes);
        }
        if (block != 16 && block != 32) {
            refuse(hdu, "the BLOCKSIZE of RICE_1 is " + std::to_string(block) + ", not 16 or 32 (standard Table 37)",
                   "ZVAL1");
        }
        if (bytes != 1 && bytes != 2 && bytes != 4 && bytes != 8) {
            refuse(hdu, "the BYTEPIX of RICE_1 is " + std::to_string(bytes) + ", not 1, 2, 4 or 8 (standard Table 37)",
                   "ZVAL2");
        }
        image.block_size = static_cast<std::size_t>(block);
        image.bytepix = static_cast<std::size_t>(bytes);
    } else if (image.algorithm == tile_algorithm::hcompress) {
        image.smooth = keyword_of<std::int64_t>(cards, "ZVAL2", hdu).value_or(0) != 0;
    }

    const std::optional<tile_column> compressed =
        find_column(table, "COMPRESSED_DATA", hdu, [](const column_format& f) {
            return is_variable(f) &&
                   (f.type == column_type::uint8 || f.type == column_type::int16 || f.type == column_type::int32);
        });
    if (!compressed) {
        refuse(hdu, "the compressed image has no column COMPRESSED_DATA, which standard §10.1.3 requires");
    }
    image.compressed = *compressed;
    image.gzip_compressed = find_column(table, "GZIP_COMPRESSED_DATA", hdu, [](const column_format& f) {
        return is_variable(f) && f.type == column_type::uint8;
    });
    image.uncompressed = find_column(table, "UNCOMPRESSED_DATA", hdu, [](const column_format& f) {
        return is_variable(f) &&
               (is_integer(f.type) || f.type == column_type::float32 || f.type == column_type::float64);
    });
    const auto fixed_number = [](const column_format& f) {
        return !is_variable(f) && f.repeat == 1 &&
               (is_integer(f.type) || f.type == column_type::float32 || f.type == column_type::float64);
    };
    image.scales = find_column(table, "ZSCALE", hdu, fixed_number);
    image.zeros = find_column(table, "ZZERO", hdu, fixed_number);
    image.nulls = find_column(table, "ZBLANK", hdu, [](const column_format& f) {
        return !is_variable(f) && f.repeat == 1 && is_integer(f.type);
    });
    image.mask = find_column(table, "NULL_PIXEL_MASK", hdu, [](const column_format& f) {
        return is_variable(f) &&
               (f.type == column_type::uint8 || f.type == column_type::int16 || f.type == column_type::int32);
    });
    if (image.mask) {
        const std::optional<std::string> mask_name = string_keyword(cards, "ZMASKCMP", hdu);
        const std::optional<tile_algorithm> mask_algorithm = mask_name ? algorithm_of(*mask_name) : std::nullopt;
        if (!mask_algorithm) {
            refuse(hdu,
                   "the compressed image has a column NULL_PIXEL_MASK without an algorithm in ZMASKCMP "
                   "(standard §10.2.2)",
                   "ZMASKCMP");
        }
        image.mask_algorithm = *mask_algorithm;
    }

    if (floating) {
        // Quantized when ZSCALE and ZZERO are given, as columns or as constants of old files (standard §10.2), unless
        // ZQUANTIZ says NONE, CFITSIO's name of a lossless image.
        image.scale = keyword_of<double>(cards, "ZSCALE", hdu);
        image.zero = keyword_of<double>(cards, "ZZERO", hdu);
        image.quantized = image.scales || image.zeros || image.scale || image.zero;
        const std::optional<std::string> method = string_keyword(cards, "ZQUANTIZ", hdu);
        if (method && *method == "NONE") {
            image.quantized = false;
        } else if (!method || *method == "NO_DITHER") {
            image.method = quantization::no_dither;
        } else if (*method == "SUBTRACTIVE_DITHER_1") {
            image.method = quantization::subtractive_dither_1;
        } else if (*method == "SUBTRACTIVE_DITHER_2") {
            image.method = quantization::subtractive_dither_2;
        } else {
            refuse(hdu, "ZQUANTIZ is " + quote(*method) + ", which is not a method of standard §10.2.1", "ZQUANTIZ");
        }
        if (image.quantized && image.method != quantization::no_dither) {
            const std::int64_t seed = keyword_of<std::int64_t>(cards, "ZDITHER0", hdu).value_or(1);
            if (seed < 1 || seed > 10'000) {
                refuse(hdu, "ZDITHER0 is " + std::to_string(seed) + ", not a seed from 1 to 10000 (standard §10.1.2)",
                       "ZDITHER0");
            }
            image.seed = static_cast<std::uint32_t>(seed);
        }
        image.null = keyword_of<std::int64_t>(cards, "ZBLANK", hdu);
        if (!image.null) {
            image.null = keyword_of<std::int64_t>(cards, "BLANK", hdu);
        }
        if (!image.quantized && (image.algorithm == tile_algorithm::rice || image.algorithm == tile_algorithm::plio ||
                                 image.algorithm == tile_algorithm::hcompress)) {
            refuse(hdu,
                   "the floating-point image is compressed with " + std::string(algorithm_name(image.algorithm)) +
                       " without ZSCALE and ZZERO, which only quantized values allow (standard §10.2)",
                   "ZCMPTYPE");
        }
    }
    image.plio_offset = image.algorithm == tile_algorithm::plio && cards.find<double>("BZERO") == 32768.0;
    if (with_header) {
        image.image.header = restored_image_header(cards);
    }
    return image;
}

header restored_image_header(const header& table)
{
    std::vector<card> restored;
    const auto add = [&](const std::optional<std::size_t>& position, std::string_view name) {
        if (position) {
            restored.push_back(renamed(table[*position], name));
        }
    };
    const bool primary = table.index_of("ZSIMPLE").has_value();
    if (primary) {
        add(table.index_of("ZSIMPLE"), "SIMPLE");
    } else if (table.index_of("ZTENSION")) {
        add(table.index_of("ZTENSION"), "XTENSION");
    } else {
        restored.push_back(card::parse("XTENSION= 'IMAGE   '           / IMAGE extension"));
    }
    add(table.index_of("ZBITPIX"), "BITPIX");
    add(table.index_of("ZNAXIS"), "NAXIS");
    const std::int64_t naxis = table.find<std::int64_t>("ZNAXIS").value_or(0);
    for (std::int64_t n = 1; n <= naxis; ++n) {
        add(table.index_of(indexed("ZNAXIS", static_cast<std::size_t>(n))),
            indexed("NAXIS", static_cast<std::size_t>(n)));
    }
    if (primary) {
        add(table.index_of("ZEXTEND"), "EXTEND");
        add(table.index_of("ZBLOCKED"), "BLOCKED");
    } else {
        if (table.index_of("ZPCOUNT")) {
            add(table.index_of("ZPCOUNT"), "PCOUNT");
        } else {
            restored.push_back(card::parse("PCOUNT  =                    0 / number of parameters"));
        }
        if (table.index_of("ZGCOUNT")) {
            add(table.index_of("ZGCOUNT"), "GCOUNT");
        } else {
            restored.push_back(card::parse("GCOUNT  =                    1 / number of groups"));
        }
    }
    const std::size_t fields =
        static_cast<std::size_t>(std::max<std::int64_t>(table.find<std::int64_t>("TFIELDS").value_or(0), 0));
    for (const card& entry : table.cards()) {
        const std::string_view name = entry.keyword();
        if (name == "ZHECKSUM") {
            restored.push_back(renamed(entry, "CHECKSUM"));
            continue;
        }
        if (name == "ZDATASUM") {
            restored.push_back(renamed(entry, "DATASUM"));
            continue;
        }
        if (is_table_keyword(name, fields)) {
            continue;
        }
        if (name == "EXTNAME" && entry.value().kind() == value_kind::string &&
            trim_trailing_spaces(entry.value().get<std::string>()) == "COMPRESSED_IMAGE") {
            continue;
        }
        restored.push_back(entry);
    }
    return header(std::move(restored));
}

void decode_tile(const compressed_image& image, std::uint64_t index, std::span<const std::uint64_t> size,
                 const tile_payload& payload, std::span<std::byte> tile)
{
    tile_workspace workspace;
    decode_tile_into(image, index, size, payload, tile, workspace);
}

pixel_read_result read_compressed_pixels(const thread_safe_source& source, const hdu_info& unit,
                                         const compressed_image& image, numeric_type type,
                                         std::span<std::byte> destination, const pixel_read_options& options,
                                         const openfits::limits& limits, const std::shared_ptr<const void>& owner)
{
    const std::uint64_t hdu = unit.index();
    const pixel_read_plan plan = plan_pixel_read(unit, image.image, type, destination, options);
    const region_selection& selection = plan.selection;
    if (!plan.conversion) {
        tile_progress(options.progress, 0, hdu).report();
        return {};
    }
    const read_conversion& conversion = *plan.conversion;
    const bitpix stored = stored_bitpix(image);
    const std::size_t width = stored_size(stored);
    const std::size_t value_size = size_of(type);

    const tile_grid grid(image.image.geometry.axes, image.tile);
    if (limits.max_tiles != 0 && grid.count() > limits.max_tiles) {
        throw limit_error(errc::too_many_tiles,
                          "the compressed image of unit " + std::to_string(hdu) + " has " +
                              std::to_string(grid.count()) + " tiles, more than limits::max_tiles, " +
                              std::to_string(limits.max_tiles),
                          {.hdu = hdu});
    }
    std::uint64_t tile_bytes = width;
    for (const std::uint64_t length : grid.tile()) {
        tile_bytes = checked_multiply(tile_bytes, length);
    }
    const std::uint64_t most =
        std::min(limits.max_tile_bytes == 0 ? std::numeric_limits<std::uint64_t>::max() : limits.max_tile_bytes,
                 limits.max_allocation == 0 ? std::numeric_limits<std::uint64_t>::max() : limits.max_allocation);
    if (tile_bytes > most) {
        throw limit_error(errc::tile_too_large,
                          "the tiles of the compressed image of unit " + std::to_string(hdu) + " hold " +
                              std::to_string(tile_bytes) + " bytes, more than the limits allow, " +
                              std::to_string(most),
                          {.hdu = hdu});
    }

    const std::vector<std::uint64_t> tiles = grid.tiles_of(selection);
    std::uint64_t total = 0;
    for (const std::uint64_t index : tiles) {
        total += grid.elements(index) * width;
    }
    tile_progress progress(options.progress, total, hdu);
    progress.report();

    std::uint8_t* const mask = options.blank == blank_policy::mask ? options.mask.data() : nullptr;
    const std::size_t axes = grid.axes().size();
    const std::size_t workers = concurrency();
    std::vector<tile_workspace> workspaces(workers);
    std::vector<std::vector<std::byte>> decoded(workers);
    tile_rows rows(source, unit, image);
    std::uint64_t clamped = 0;
    std::size_t next = 0;
    while (next < tiles.size()) {
        // A batch of tiles, of at most batch_bytes once decoded, and one tile at least.
        std::size_t end = next;
        std::uint64_t bytes = 0;
        while (end < tiles.size() && (end == next || bytes + (grid.elements(tiles[end]) * width) <= batch_bytes)) {
            bytes += grid.elements(tiles[end]) * width;
            ++end;
        }
        const std::span<const std::uint64_t> batch = std::span(tiles).subspan(next, end - next);

        // The tiles of the cache, and the rows and the compressed bytes of the others.
        std::vector<decoded_tile> cached(batch.size());
        std::vector<std::uint64_t> missing;
        std::vector<std::size_t> positions;
        for (std::size_t i = 0; i < batch.size(); ++i) {
            if (options.cache != nullptr) {
                cached[i] = tile_cache_access::find(*options.cache, {.owner = owner, .hdu = hdu, .tile = batch[i]});
            }
            if (!cached[i]) {
                missing.push_back(batch[i]);
                positions.push_back(i);
            }
        }
        rows.read(missing);
        std::vector<tile_request> requests(missing.size());
        std::vector<heap_range> ranges;
        ranges.reserve(missing.size() * 2);
        for (std::size_t i = 0; i < missing.size(); ++i) {
            requests[i] = request_of(image, rows.row(i), missing[i], hdu);
            ranges.push_back(requests[i].data);
            ranges.push_back(requests[i].mask);
        }
        const std::vector<std::span<const std::byte>> spans = rows.read_heap(ranges);
        std::vector<const tile_payload*> payloads(batch.size(), nullptr);
        for (std::size_t i = 0; i < missing.size(); ++i) {
            requests[i].payload.bytes = spans[2 * i];
            requests[i].payload.mask = spans[(2 * i) + 1];
            payloads[positions[i]] = &requests[i].payload;
        }

        // Each worker decodes its share of the tiles and converts their values into the destination, where the
        // values of different tiles never meet.
        std::vector<std::uint64_t> counts(batch.size(), 0);
        const std::size_t share = std::min(workers, batch.size());
        parallel_for(share, [&](std::size_t worker) {
            std::vector<std::uint64_t> start(axes);
            std::vector<std::uint64_t> size(axes);
            for (std::size_t i = worker; i < batch.size(); i += share) {
                const std::uint64_t index = batch[i];
                grid.box(index, start, size);
                const auto length = checked_cast<std::size_t>(grid.elements(index) * width);
                std::span<const std::byte> tile;
                decoded_tile kept = cached[i];
                if (kept) {
                    tile = kept->bytes();
                } else {
                    std::span<std::byte> target;
                    std::shared_ptr<scratch_buffer> fresh;
                    if (options.cache != nullptr) {
                        fresh = std::make_shared<scratch_buffer>(length);
                        target = fresh->bytes();
                    } else {
                        decoded[worker].resize(length);
                        target = decoded[worker];
                    }
                    try {
                        decode_tile_into(image, index, size, *payloads[i], target, workspaces[worker]);
                    } catch (const integrity_error& failure) {
                        throw integrity_error(failure.code(),
                                              "tile " + std::to_string(index + 1) +
                                                  " of the compressed image of unit " + std::to_string(hdu) + ": " +
                                                  failure.what(),
                                              {.hdu = hdu});
                    } catch (const invalid_data_error& failure) {
                        throw invalid_data_error(failure.code(),
                                                 "tile " + std::to_string(index + 1) +
                                                     " of the compressed image of unit " + std::to_string(hdu) + ": " +
                                                     failure.what(),
                                                 {.hdu = hdu});
                    }
                    tile = target;
                    if (fresh) {
                        // The cache may keep no reference, when another read keeps the tile already or it is larger
                        // than the cache: the read keeps one until it has converted the values.
                        kept = fresh;
                        tile_cache_access::insert(*options.cache, {.owner = owner, .hdu = hdu, .tile = index}, fresh);
                    }
                }
                for_each_tile_run(selection, start, size, options.pitch, [&](const tile_run& run) {
                    counts[i] += convert_read(conversion, tile.data() + (run.first * width), run.step,
                                              destination.data() + (run.destination * value_size),
                                              mask == nullptr ? nullptr : mask + run.destination, run.count);
                });
            }
        });
        for (const std::uint64_t count : counts) {
            clamped += count;
        }
        progress.advance(bytes);
        next = end;
    }

    // A read of the whole image verifies the DATASUM of the table (standard §4.4.2.7), in a pass of its own over the
    // data records, which the tiles do not read in order.
    if (!options.region && options.verify_checksum) {
        const recorded_datasum datasum = datasum_of(unit.header());
        if (datasum.known) {
            checksum_accumulator sum;
            add_source_bytes(sum, source, unit.data_offset(), data_records(unit.data_size().value_or(0)));
            if (datasum.value != sum.sum()) {
                throw_datasum_mismatch(hdu, sum.sum(), datasum);
            }
        }
    }
    return {.clamped = clamped};
}

} // namespace openfits::detail
