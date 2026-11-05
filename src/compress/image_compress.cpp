// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "compress/image_compress.h"

#include <openfits/error.h>

#include "compress/gzip.h"
#include "compress/hcompress.h"
#include "compress/plio.h"
#include "compress/quantize.h"
#include "compress/rice.h"
#include "compress/shuffle.h"
#include "compress/tile_grid.h"
#include "core/checked_math.h"
#include "core/endian.h"
#include "core/parallel.h"
#include "core/value_text.h"
#include "data/image_write.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <deque>
#include <limits>
#include <optional>
#include <string>

namespace openfits::detail {

namespace {

// The stored values that a batch of tiles holds at once, between which the progress is reported.
constexpr std::uint64_t batch_bytes = std::uint64_t{64} << 20;

// The largest offset and count of a P descriptor that the writer writes: 31 bits, which every reader takes as a signed
// or an unsigned integer alike (standard §7.3.5).
constexpr std::uint64_t descriptor_limit = 0x7FFF'FFFF;

// The default rows of a tile of HCOMPRESS_1 for an image of more than 30 rows: the first that leaves the last tile no
// row or at least 4, as fpack chooses them, and 17 when none does.
constexpr std::array<std::uint64_t, 9> hcompress_rows{16, 24, 20, 30, 28, 26, 22, 18, 14};

// A compression that the image cannot take, which names its algorithm.
[[noreturn]] void invalid(std::uint64_t hdu, const std::string& message)
{
    throw validation_error(errc::invalid_compression, message, {.hdu = hdu, .keyword = "ZCMPTYPE"});
}

[[noreturn]] void bad_option(std::uint64_t hdu, const std::string& message)
{
    throw usage_error(errc::invalid_argument, message, {.hdu = hdu});
}

bool is_floating(bitpix stored) noexcept
{
    return static_cast<int>(stored) < 0;
}

// The type of the elements of a column of the values of BITPIX.
column_type column_of(bitpix stored) noexcept
{
    switch (stored) {
    case bitpix::uint8:
        return column_type::uint8;
    case bitpix::int16:
        return column_type::int16;
    case bitpix::int32:
        return column_type::int32;
    case bitpix::int64:
        return column_type::int64;
    case bitpix::float32:
        return column_type::float32;
    case bitpix::float64:
        break;
    }
    return column_type::float64;
}

// The scale of HCOMPRESS_1 as the table records it and CFITSIO reads it back: the float of the request, written with 8
// significant digits, then read as a float again.
float recorded_scale(double request)
{
    const auto requested = static_cast<float>(request);
    std::array<char, 32> text{};
    const auto [end, result] = std::to_chars(text.data(), text.data() + text.size(), static_cast<double>(requested),
                                             std::chars_format::scientific, 7);
    std::string number(text.data(), result == std::errc{} ? end : text.data());
    std::ranges::replace(number, 'e', 'E');
    const std::optional<double> value = real_value(number);
    return value ? static_cast<float>(*value) : requested;
}

// The integer scale of a tile of HCOMPRESS_1: a positive scale times the noise of the tile, the smallest of its
// estimates that is not 0, a negative one its absolute value, rounded as CFITSIO rounds them.
std::int64_t tile_scale(float scale, const noise_estimate& noise)
{
    float factor = 0.0F;
    if (scale > 0.0F) {
        double estimate = noise.noise3;
        if (noise.noise2 != 0.0 && noise.noise2 < estimate) {
            estimate = noise.noise2;
        }
        if (noise.noise5 != 0.0 && noise.noise5 < estimate) {
            estimate = noise.noise5;
        }
        const double product = static_cast<double>(scale) * estimate;
        factor = static_cast<float>(product);
    } else if (scale < 0.0F) {
        factor = -scale;
    }
    const double rounded = static_cast<double>(factor) + 0.5;
    return static_cast<std::int64_t>(std::trunc(rounded));
}

// What every tile of an image needs.
struct image_plan
{
    const image_info& image;
    const write_conversion& conversion;
    numeric_type type = numeric_type::uint8;
    // The values of the batch of tiles being compressed, from the element origin of the image on.
    std::span<const std::byte> data{};
    std::uint64_t origin = 0;
    const compression_options& options;
    tile_algorithm algorithm = tile_algorithm::rice;
    bitpix stored = bitpix::uint8;
    bool quantized = false;
    std::uint32_t seed = 1;
    bool plio_offset = false;
    float hcompress_scale = 0.0F;
    std::uint64_t hdu = 0;
};

// The memory of a worker, kept for the tiles it compresses.
struct tile_encoder
{
    explicit tile_encoder(int level) : gzip(level) {}

    gzip_encoder gzip;
    std::vector<std::byte> stored{};
    std::vector<std::byte> shuffled{};
    std::vector<std::byte> codes{};
    std::vector<std::uint8_t> u8{};
    std::vector<std::uint16_t> u16{};
    std::vector<std::uint32_t> u32{};
    std::vector<std::uint64_t> u64{};
    std::vector<std::int32_t> i32{};
    std::vector<std::int64_t> i64{};
    std::vector<std::int16_t> words{};
    std::vector<float> floats{};
    std::vector<double> doubles{};
};

// Gathers the values of the tile whose box is start and size into encoder.stored, converted into their stored values,
// big-endian.
void gather(const image_plan& plan, std::span<const std::uint64_t> start, std::span<const std::uint64_t> size,
            tile_encoder& encoder)
{
    const std::size_t width = stored_size(plan.stored);
    const std::size_t value_size = size_of(plan.type);
    const std::vector<std::uint64_t>& axes = plan.image.geometry.axes;
    std::uint64_t count = 1;
    for (const std::uint64_t length : size) {
        count *= length;
    }
    encoder.stored.resize(count * width);
    std::vector<std::uint64_t> k(axes.size(), 0);
    std::uint64_t position = 0;
    for (;;) {
        std::uint64_t element = 0;
        std::uint64_t stride = 1;
        for (std::size_t a = 0; a < axes.size(); ++a) {
            element += (start[a] + k[a]) * stride;
            stride *= axes[a];
        }
        (void)convert_write(plan.conversion, plan.data.data() + ((element - plan.origin) * value_size),
                            encoder.stored.data() + (position * width), size[0]);
        position += size[0];
        std::size_t a = 1;
        for (; a < axes.size(); ++a) {
            if (++k[a] < size[a]) {
                break;
            }
            k[a] = 0;
        }
        if (a >= axes.size()) {
            return;
        }
    }
}

// The rows of the two-dimensional view of a tile, as CFITSIO shapes it: the length of its first axis longer than 1,
// and the product of the others.
std::pair<std::size_t, std::size_t> view_of(std::span<const std::uint64_t> size)
{
    std::uint64_t count = 1;
    std::uint64_t row = 1;
    bool found = false;
    for (const std::uint64_t length : size) {
        count *= length;
        if (!found && length > 1) {
            row = length;
            found = true;
        }
    }
    return {checked_cast<std::size_t>(row), checked_cast<std::size_t>(count / row)};
}

// The integers of the stored values, big-endian, of width bytes: signed, but bytes.
void load_stored(std::span<const std::byte> stored, std::size_t width, std::span<std::int64_t> values) noexcept
{
    const auto shift = static_cast<unsigned>(64 - (width * 8));
    for (std::size_t i = 0; i < values.size(); ++i) {
        std::uint64_t bits = 0;
        for (std::size_t b = 0; b < width; ++b) {
            bits = (bits << 8U) | std::to_integer<std::uint64_t>(stored[(i * width) + b]);
        }
        values[i] = width == 1 ? static_cast<std::int64_t>(bits) : static_cast<std::int64_t>(bits << shift) >> shift;
    }
}

void gzip_bytes(std::span<const std::byte> values, std::size_t width, bool shuffle, tile_encoder& encoder,
                std::vector<std::byte>& output)
{
    if (shuffle && width > 1) {
        encoder.shuffled.resize(values.size());
        shuffle_bytes(values, encoder.shuffled, width);
        encoder.gzip.compress(encoder.shuffled, output);
    } else {
        encoder.gzip.compress(values, output);
    }
}

void store_words(std::span<const std::int16_t> words, std::vector<std::byte>& output)
{
    output.resize(words.size() * 2);
    for (std::size_t i = 0; i < words.size(); ++i) {
        store_big_endian(std::span(output).subspan(i * 2).first<2>(), static_cast<std::uint16_t>(words[i]));
    }
}

void encode_plio(const image_plan& plan, std::uint64_t index, std::span<const std::int64_t> values, std::int64_t offset,
                 tile_encoder& encoder, encoded_tile& tile)
{
    encoder.i32.resize(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::int64_t value = values[i] + offset;
        if (value < 0 || value > plio_max_value) {
            invalid(plan.hdu, "PLIO_1 compresses the values from 0 to 2^24 - 1, and tile " + std::to_string(index + 1) +
                                  " of unit " + std::to_string(plan.hdu) + " holds " + std::to_string(value) +
                                  " (standard §10.4.3)");
        }
        encoder.i32[i] = static_cast<std::int32_t>(value);
    }
    encoder.words.clear();
    plio_encode(encoder.i32, encoder.words);
    store_words(encoder.words, tile.bytes);
    tile.elements = encoder.words.size();
}

void encode_hcompress(const image_plan& plan, std::uint64_t index, std::span<const std::int64_t> values,
                      std::span<const std::uint64_t> size, std::optional<std::int32_t> null, tile_encoder& encoder,
                      encoded_tile& tile)
{
    const auto [row, rows] = view_of(size);
    // A lossy scale changes the values that mark undefined pixels, BLANK or the null code of a quantized tile, which
    // would no longer read as undefined: a tile that holds one is compressed losslessly.
    const bool undefined_pixels = null && std::ranges::find(values, std::int64_t{*null}) != values.end();
    std::int64_t scale = 0;
    if (plan.hcompress_scale != 0.0F && !undefined_pixels) {
        noise_estimate noise;
        if (plan.hcompress_scale > 0.0F) {
            bool fits = true;
            encoder.i32.resize(values.size());
            for (std::size_t i = 0; i < values.size(); ++i) {
                fits = fits && values[i] >= std::numeric_limits<std::int32_t>::min() &&
                       values[i] <= std::numeric_limits<std::int32_t>::max();
                encoder.i32[i] = static_cast<std::int32_t>(values[i]);
            }
            if (fits) {
                noise = estimate_noise(encoder.i32, row, rows, null);
            } else {
                encoder.doubles.resize(values.size());
                std::ranges::transform(values, encoder.doubles.begin(),
                                       [](std::int64_t value) { return static_cast<double>(value); });
                noise = estimate_noise(encoder.doubles, row, rows, false);
            }
        }
        scale = tile_scale(plan.hcompress_scale, noise);
    }
    // The coefficients of larger values would leave the range that the coder of the standard writes exactly. The
    // extremes first, without a branch on the signs, which are those of noise; the error names the first value beyond.
    const std::uint64_t limit = hcompress_max_magnitude(rows, row, scale);
    const auto magnitude = [](std::int64_t value) {
        return value < 0 ? 0 - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
    };
    std::int64_t low = 0;
    std::int64_t high = 0;
    for (const std::int64_t value : values) {
        low = std::min(low, value);
        high = std::max(high, value);
    }
    if (std::max(magnitude(low), magnitude(high)) > limit) {
        for (const std::int64_t value : values) {
            if (magnitude(value) > limit) {
                invalid(plan.hdu, "HCOMPRESS_1 compresses the values of magnitude up to " + std::to_string(limit) +
                                      " in tiles of this shape, and tile " + std::to_string(index + 1) + " of unit " +
                                      std::to_string(plan.hdu) + " holds " + std::to_string(value));
            }
        }
    }
    hcompress_encode(values, rows, row, scale, tile.bytes);
    tile.elements = tile.bytes.size();
}

// Compresses the integers of a tile, the stored values of an integer image or the codes of a quantized one.
void encode_integers(const image_plan& plan, std::uint64_t index, std::span<const std::byte> big_endian,
                     std::size_t width, std::span<const std::uint64_t> size, std::optional<std::int32_t> null,
                     tile_encoder& encoder, encoded_tile& tile)
{
    const std::size_t count = big_endian.size() / width;
    tile.bytes.clear();
    switch (plan.algorithm) {
    case tile_algorithm::rice:
        switch (width) {
        case 1:
            encoder.u8.resize(count);
            std::ranges::transform(big_endian, encoder.u8.begin(),
                                   [](std::byte value) { return std::to_integer<std::uint8_t>(value); });
            rice_encode(std::span<const std::uint8_t>(encoder.u8), plan.options.rice_block_size, tile.bytes);
            break;
        case 2:
            encoder.u16.resize(count);
            for (std::size_t i = 0; i < count; ++i) {
                encoder.u16[i] = load_big_endian<std::uint16_t>(big_endian.subspan(i * 2).first<2>());
            }
            rice_encode(std::span<const std::uint16_t>(encoder.u16), plan.options.rice_block_size, tile.bytes);
            break;
        case 4:
            encoder.u32.resize(count);
            for (std::size_t i = 0; i < count; ++i) {
                encoder.u32[i] = load_big_endian<std::uint32_t>(big_endian.subspan(i * 4).first<4>());
            }
            rice_encode(std::span<const std::uint32_t>(encoder.u32), plan.options.rice_block_size, tile.bytes);
            break;
        default:
            encoder.u64.resize(count);
            for (std::size_t i = 0; i < count; ++i) {
                encoder.u64[i] = load_big_endian<std::uint64_t>(big_endian.subspan(i * 8).first<8>());
            }
            rice_encode(std::span<const std::uint64_t>(encoder.u64), plan.options.rice_block_size, tile.bytes);
            break;
        }
        tile.elements = tile.bytes.size();
        return;
    case tile_algorithm::gzip_1:
    case tile_algorithm::gzip_2:
        gzip_bytes(big_endian, width, plan.algorithm == tile_algorithm::gzip_2, encoder, tile.bytes);
        tile.elements = tile.bytes.size();
        return;
    case tile_algorithm::plio:
    case tile_algorithm::hcompress:
        break;
    case tile_algorithm::nocompress:
        tile.column = tile_payload::holder::uncompressed;
        tile.bytes.assign(big_endian.begin(), big_endian.end());
        tile.elements = count;
        return;
    }
    encoder.i64.resize(count);
    load_stored(big_endian, width, encoder.i64);
    if (plan.algorithm == tile_algorithm::plio) {
        encode_plio(plan, index, encoder.i64, plan.plio_offset ? 32768 : 0, encoder, tile);
    } else {
        encode_hcompress(plan, index, encoder.i64, size, null, encoder, tile);
    }
}

template <typename T> void load_reals(std::span<const std::byte> stored, std::vector<T>& values)
{
    values.resize(stored.size() / sizeof(T));
    for (std::size_t i = 0; i < values.size(); ++i) {
        if constexpr (std::is_same_v<T, float>) {
            values[i] = std::bit_cast<float>(load_big_endian<std::uint32_t>(stored.subspan(i * 4).template first<4>()));
        } else {
            values[i] =
                std::bit_cast<double>(load_big_endian<std::uint64_t>(stored.subspan(i * 8).template first<8>()));
        }
    }
}

// Quantizes the values of a floating-point tile and compresses its codes; a tile that cannot be quantized is stored
// with GZIP in GZIP_COMPRESSED_DATA, as it is.
template <typename T>
void encode_quantized(const image_plan& plan, double level, std::uint64_t index, std::span<const std::uint64_t> size,
                      std::vector<T>& values, tile_encoder& encoder, encoded_tile& tile)
{
    load_reals(encoder.stored, values);
    tile.nulls = prepare_tile(std::span<T>(values));
    const auto [row, rows] = view_of(size);
    encoder.i32.resize(values.size());
    const std::optional<quantized_scaling> scaling = quantize(
        std::span<const T>(values), row, rows, tile.nulls,
        quantize_request{.level = level, .method = plan.options.quantize_method, .row = index + 1, .seed = plan.seed},
        encoder.i32);
    if (!scaling) {
        if (!plan.options.lossless_fallback) {
            invalid(plan.hdu, "tile " + std::to_string(index + 1) + " of unit " + std::to_string(plan.hdu) +
                                  " cannot be quantized, and the options keep no tile lossless (standard §10.2)");
        }
        tile.column = tile_payload::holder::gzip_compressed;
        tile.bytes.clear();
        encoder.gzip.compress(encoder.stored, tile.bytes);
        tile.elements = tile.bytes.size();
        return;
    }
    tile.scale = scaling->scale;
    tile.zero = scaling->zero;
    // The codes, big-endian, as the integers of the tile.
    encoder.codes.resize(encoder.i32.size() * 4);
    for (std::size_t i = 0; i < encoder.i32.size(); ++i) {
        store_big_endian(std::span<std::byte>(encoder.codes).subspan(i * 4).first<4>(),
                         static_cast<std::uint32_t>(encoder.i32[i]));
    }
    encode_integers(plan, index, encoder.codes, 4, size, tile.nulls ? std::optional(quantized_null) : std::nullopt,
                    encoder, tile);
}

// Compresses the stored values of a tile that encoder.stored holds.
void encode_gathered(const image_plan& plan, std::uint64_t index, std::span<const std::uint64_t> size,
                     tile_encoder& encoder, encoded_tile& tile)
{
    const std::size_t width = stored_size(plan.stored);
    if (!is_floating(plan.stored)) {
        std::optional<std::int32_t> null;
        if (plan.image.blank && *plan.image.blank >= std::numeric_limits<std::int32_t>::min() &&
            *plan.image.blank <= std::numeric_limits<std::int32_t>::max()) {
            null = static_cast<std::int32_t>(*plan.image.blank);
        }
        encode_integers(plan, index, encoder.stored, width, size, null, encoder, tile);
        return;
    }
    // A quantized plan has a level.
    if (plan.quantized && plan.options.quantize_level) {
        const double level = *plan.options.quantize_level;
        if (plan.stored == bitpix::float32) {
            encode_quantized(plan, level, index, size, encoder.floats, encoder, tile);
        } else {
            encode_quantized(plan, level, index, size, encoder.doubles, encoder, tile);
        }
        return;
    }
    // Lossless: the values as they are, NaN payloads and signed zeros kept.
    tile.bytes.clear();
    if (plan.algorithm == tile_algorithm::nocompress) {
        tile.column = tile_payload::holder::uncompressed;
        tile.bytes.assign(encoder.stored.begin(), encoder.stored.end());
        tile.elements = encoder.stored.size() / width;
        return;
    }
    gzip_bytes(encoder.stored, width, plan.algorithm == tile_algorithm::gzip_2, encoder, tile.bytes);
    tile.elements = tile.bytes.size();
}

void encode_tile(const image_plan& plan, std::uint64_t index, std::span<const std::uint64_t> start,
                 std::span<const std::uint64_t> size, tile_encoder& encoder, encoded_tile& tile)
{
    gather(plan, start, size, encoder);
    encode_gathered(plan, index, size, encoder, tile);
}

// The seed of the dithering taken from the values of the first tile, as CFITSIO takes it.
std::uint32_t seed_from_data(const image_plan& plan, const tile_grid& grid)
{
    tile_encoder encoder(-1);
    std::vector<std::uint64_t> start(grid.axes().size());
    std::vector<std::uint64_t> size(grid.axes().size());
    grid.box(0, start, size);
    gather(plan, start, size, encoder);
    if (plan.stored == bitpix::float32) {
        load_reals(encoder.stored, encoder.floats);
        (void)prepare_tile(std::span<float>(encoder.floats));
        return dither_seed_of(encoder.floats);
    }
    load_reals(encoder.stored, encoder.doubles);
    (void)prepare_tile(std::span<double>(encoder.doubles));
    return dither_seed_of(encoder.doubles);
}

// The tiles of an image grouped in bands: the tiles that share their indices along every axis from band_axis on, which
// cover a run of elements of the data, whole rows of them for an image of two axes or more (standard §10.1). The band
// axis is the last along which tiles are longer than one element, the second at least: a band holds whole rows.
class tile_bands
{
public:
    tile_bands(const tile_grid& grid, std::span<const std::uint64_t> axes, std::span<const std::uint64_t> tiles)
        : grid_(grid), axes_(axes.begin(), axes.end())
    {
        std::size_t band_axis = axes.size() > 1 ? 1U : 0U;
        for (std::size_t a = 0; a < tiles.size(); ++a) {
            if (tiles[a] > 1) {
                band_axis = std::max(band_axis, a);
            }
        }
        band_axis_ = band_axis;
        per_band_ = 1;
        below_ = 1;
        for (std::size_t a = 0; a < band_axis; ++a) {
            per_band_ *= (axes[a] + tiles[a] - 1) / tiles[a];
            below_ *= axes[a];
        }
    }

    // The tiles of each band.
    [[nodiscard]] std::uint64_t tiles_per_band() const noexcept
    {
        return per_band_;
    }

    // The first element and the number of elements of the band of the tile first, the first tile of a band.
    [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> elements(std::uint64_t first) const
    {
        std::vector<std::uint64_t> start(axes_.size());
        std::vector<std::uint64_t> size(axes_.size());
        grid_.box(first, start, size);
        std::uint64_t element = 0;
        std::uint64_t stride = 1;
        for (std::size_t a = 0; a < axes_.size(); ++a) {
            element += start[a] * stride;
            stride *= axes_[a];
        }
        return {element, axes_.empty() ? 0 : size[band_axis_] * below_};
    }

private:
    const tile_grid& grid_;
    std::vector<std::uint64_t> axes_;
    std::size_t band_axis_ = 0;
    std::uint64_t per_band_ = 1;
    std::uint64_t below_ = 1;
};

void store_descriptor(std::span<std::byte> field, bool wide, std::uint64_t count, std::uint64_t offset)
{
    if (wide) {
        store_big_endian(field.first<8>(), count);
        store_big_endian(field.subspan<8, 8>(), offset);
    } else {
        store_big_endian(field.first<4>(), static_cast<std::uint32_t>(count));
        store_big_endian(field.subspan<4, 4>(), static_cast<std::uint32_t>(offset));
    }
}

// A column of the table of a compressed image as the writer lays it out.
struct table_column
{
    std::string name{};
    column_format format{};
    std::uint64_t offset = 0;
};

} // namespace

tile_algorithm resolve_algorithm(const compression_options& options, bitpix stored) noexcept
{
    switch (options.type) {
    case compression::rice:
        return tile_algorithm::rice;
    case compression::gzip_1:
        return tile_algorithm::gzip_1;
    case compression::gzip_2:
        return tile_algorithm::gzip_2;
    case compression::plio:
        return tile_algorithm::plio;
    case compression::hcompress:
        return tile_algorithm::hcompress;
    case compression::nocompress:
        return tile_algorithm::nocompress;
    case compression::none:
    case compression::automatic:
        break;
    }
    if (stored == bitpix::int64 || (is_floating(stored) && !options.quantize_level)) {
        return tile_algorithm::gzip_2;
    }
    return tile_algorithm::rice;
}

std::vector<std::uint64_t> resolve_tiles(const compression_options& options, std::span<const std::uint64_t> axes,
                                         tile_algorithm algorithm, std::uint64_t hdu)
{
    const std::size_t count = axes.size();
    std::vector<std::uint64_t> tile(count, 1);
    if (options.tile.empty()) {
        if (count > 0) {
            tile[0] = std::max<std::uint64_t>(axes[0], 1);
        }
        if (algorithm == tile_algorithm::hcompress && count > 1) {
            const std::uint64_t rows = axes[1];
            tile[1] = rows;
            if (rows > 30) {
                tile[1] = 17;
                for (const std::uint64_t candidate : hcompress_rows) {
                    if (rows % candidate == 0 || rows % candidate > 3) {
                        tile[1] = candidate;
                        break;
                    }
                }
            }
        }
    } else {
        if (options.tile.size() != count) {
            bad_option(hdu, "the tile shape of the compression has " + std::to_string(options.tile.size()) +
                                " entries, and the image " + std::to_string(count) + " axes");
        }
        for (std::size_t a = 0; a < count; ++a) {
            tile[a] = options.tile[a] == 0 ? std::max<std::uint64_t>(axes[a], 1) : options.tile[a];
        }
    }
    for (std::size_t a = 0; a < count; ++a) {
        tile[a] = std::min(tile[a], std::max<std::uint64_t>(axes[a], 1));
    }
    if (algorithm == tile_algorithm::hcompress) {
        // Two-dimensional tiles, each side of 4 pixels at least, the last tile along it too (standard §10.4.4).
        std::vector<std::size_t> sides;
        for (std::size_t a = 0; a < count; ++a) {
            if (tile[a] > 1) {
                sides.push_back(a);
            }
        }
        if (sides.size() != 2) {
            invalid(hdu, "HCOMPRESS_1 compresses two-dimensional tiles, and those of unit " + std::to_string(hdu) +
                             " have " + std::to_string(sides.size()) +
                             " axes longer than one pixel (standard §10.4.4)");
        }
        for (const std::size_t a : sides) {
            if (tile[a] < 4) {
                invalid(hdu, "HCOMPRESS_1 compresses tiles of 4 pixels at least along each side, and those of unit " +
                                 std::to_string(hdu) + " have " + std::to_string(tile[a]) + " along axis " +
                                 std::to_string(a + 1));
            }
            // The last tile is made at least 4 pixels long by lengthening every tile a little, as CFITSIO does.
            const std::uint64_t rest = axes[a] % tile[a];
            if (rest > 0 && rest < 4) {
                const std::uint64_t whole = axes[a] / tile[a];
                tile[a] += (rest + whole - 1) / whole;
                const std::uint64_t left = axes[a] % tile[a];
                if (left > 0 && left < 4) {
                    invalid(hdu, "the last tile of unit " + std::to_string(hdu) + " along axis " +
                                     std::to_string(a + 1) +
                                     " has fewer than the 4 pixels that HCOMPRESS_1 compresses");
                }
            }
        }
    }
    return tile;
}

void check_compression(const compression_options& options, bitpix stored, std::span<const std::uint64_t> axes,
                       std::uint64_t hdu)
{
    if (options.rice_block_size != rice_block_16 && options.rice_block_size != rice_block_32) {
        bad_option(hdu, "the block size of RICE_1 is 16 or 32 pixels, not " + std::to_string(options.rice_block_size) +
                            " (standard Table 37)");
    }
    if (options.gzip_level && (*options.gzip_level < 1 || *options.gzip_level > 9)) {
        bad_option(hdu, "the level of zlib is from 1 to 9, not " + std::to_string(*options.gzip_level));
    }
    if (options.dither_seed && (*options.dither_seed < 1 || *options.dither_seed > 10'000)) {
        bad_option(hdu, "the seed of the dithering is from 1 to 10000, not " + std::to_string(*options.dither_seed) +
                            " (standard §10.2.1)");
    }
    if (options.quantize_level && (!std::isfinite(*options.quantize_level) || *options.quantize_level == 0.0)) {
        bad_option(hdu, "the level of the quantization is a finite number other than 0");
    }
    if (!std::isfinite(options.hcompress_scale)) {
        bad_option(hdu, "the scale of HCOMPRESS_1 is not finite");
    }
    const tile_algorithm algorithm = resolve_algorithm(options, stored);
    const bool floating = is_floating(stored);
    if (floating && !options.quantize_level &&
        (algorithm == tile_algorithm::rice || algorithm == tile_algorithm::plio ||
         algorithm == tile_algorithm::hcompress)) {
        invalid(hdu, std::string(algorithm_name(algorithm)) +
                         " compresses integers: the floating-point values of unit " + std::to_string(hdu) +
                         " need a quantization, or GZIP to keep them as they are");
    }
    if (algorithm == tile_algorithm::rice && stored == bitpix::int64 && !options.rice_bytepix_8) {
        invalid(hdu, "RICE_1 compresses the 64-bit integers of unit " + std::to_string(hdu) +
                         " only when the options ask for BYTEPIX 8, which readers built on CFITSIO do not decompress");
    }
    if (algorithm == tile_algorithm::hcompress && floating && options.quantize_level &&
        options.quantize_method == quantization::subtractive_dither_2) {
        invalid(hdu, "SUBTRACTIVE_DITHER_2 is not used with HCOMPRESS_1, which does not keep the zeros it reserves");
    }
    (void)resolve_tiles(options, axes, algorithm, hdu);
}

compressed_unit compress_image(const image_info& image, const write_conversion& conversion, numeric_type type,
                               const image_values& values, const compression_options& options, bool primary,
                               std::uint64_t hdu, const std::function<void(std::uint64_t)>& advance)
{
    const bitpix stored = model_bitpix(image);
    const std::vector<std::uint64_t>& axes = image.geometry.axes;
    check_compression(options, stored, axes, hdu);
    const tile_algorithm algorithm = resolve_algorithm(options, stored);
    const std::vector<std::uint64_t> tiles = resolve_tiles(options, axes, algorithm, hdu);
    const tile_grid grid(axes, tiles);
    const tile_bands bands(grid, axes, tiles);
    const std::size_t width = stored_size(stored);

    image_plan plan{.image = image,
                    .conversion = conversion,
                    .type = type,
                    .options = options,
                    .algorithm = algorithm,
                    .stored = stored,
                    .quantized = is_floating(stored) && options.quantize_level.has_value() &&
                                 algorithm != tile_algorithm::nocompress,
                    .plio_offset = algorithm == tile_algorithm::plio && image.scaling.zero == 32768.0,
                    .hcompress_scale = recorded_scale(options.hcompress_scale),
                    .hdu = hdu};
    const bool dithered = plan.quantized && options.quantize_method != quantization::no_dither;
    if (dithered && options.dither_seed) {
        plan.seed = *options.dither_seed;
    }

    // The tiles, compressed in batches of whole bands, whose values the batch takes at once, each batch by the workers
    // in parallel.
    const std::size_t workers = concurrency();
    // A deque, which builds them in place: the state of zlib does not move.
    std::deque<tile_encoder> encoders;
    for (std::size_t w = 0; w < workers; ++w) {
        encoders.emplace_back(options.gzip_level.value_or(-1));
    }
    std::vector<encoded_tile> encoded(checked_cast<std::size_t>(grid.count()));
    const std::uint64_t per_band = bands.tiles_per_band();
    std::uint64_t next = 0;
    while (next < grid.count()) {
        std::uint64_t end = next;
        std::uint64_t bytes = 0;
        while (end < grid.count()) {
            const std::uint64_t band_end = std::min(end + per_band, grid.count());
            std::uint64_t band_bytes = 0;
            for (std::uint64_t t = end; t < band_end; ++t) {
                band_bytes += grid.elements(t) * width;
            }
            if (end != next && bytes + band_bytes > batch_bytes) {
                break;
            }
            bytes += band_bytes;
            end = band_end;
        }
        const std::uint64_t first = next;
        // Bands are whole, so that the batch ends with the band of its last tile but per_band.
        const std::uint64_t origin = bands.elements(first).first;
        const std::pair<std::uint64_t, std::uint64_t> last = bands.elements(end - per_band);
        plan.origin = origin;
        plan.data = values(origin, checked_cast<std::size_t>(last.first + last.second - origin));
        if (dithered && first == 0 && !options.dither_seed) {
            plan.seed = seed_from_data(plan, grid);
        }
        const std::size_t share = std::min<std::size_t>(workers, checked_cast<std::size_t>(end - first));
        parallel_for(share, [&](std::size_t worker) {
            std::vector<std::uint64_t> start(axes.size());
            std::vector<std::uint64_t> size(axes.size());
            for (std::uint64_t index = first + worker; index < end; index += share) {
                grid.box(index, start, size);
                encode_tile(plan, index, start, size, encoders[worker], encoded[checked_cast<std::size_t>(index)]);
            }
        });
        if (advance) {
            advance(bytes);
        }
        next = end;
    }

    // The columns: COMPRESSED_DATA, ZSCALE and ZZERO of quantized tiles, and the columns of the tiles that are not in
    // COMPRESSED_DATA.
    bool fallback = false;
    bool uncompressed = false;
    bool nulls = false;
    std::uint64_t heap = 0;
    std::uint64_t largest = 0;
    for (const encoded_tile& tile : encoded) {
        fallback = fallback || tile.column == tile_payload::holder::gzip_compressed;
        uncompressed = uncompressed || tile.column == tile_payload::holder::uncompressed;
        nulls = nulls || tile.nulls;
        heap = checked_add<std::uint64_t>(heap, tile.bytes.size());
        largest = std::max(largest, tile.elements);
    }
    const bool wide = heap > descriptor_limit || largest > descriptor_limit;
    const column_storage storage = wide ? column_storage::variable_q : column_storage::variable_p;
    std::vector<table_column> columns;
    const auto max_of = [&encoded](tile_payload::holder holder) {
        std::uint64_t most = 0;
        for (const encoded_tile& tile : encoded) {
            if (tile.column == holder) {
                most = std::max(most, tile.elements);
            }
        }
        return most;
    };
    const auto add_column = [&columns](std::string name, column_format format) {
        const std::uint64_t offset = columns.empty() ? 0 : columns.back().offset + columns.back().format.field_size();
        columns.push_back({.name = std::move(name), .format = format, .offset = offset});
    };
    const auto variable = [storage](column_type of, std::uint64_t most) {
        column_format format;
        format.type = of;
        format.storage = storage;
        format.max_elements = most;
        return format;
    };
    const auto fixed_real = [] {
        column_format format;
        format.type = column_type::float64;
        return format;
    };
    add_column("COMPRESSED_DATA", variable(algorithm == tile_algorithm::plio ? column_type::int16 : column_type::uint8,
                                           max_of(tile_payload::holder::compressed)));
    if (plan.quantized) {
        add_column("ZSCALE", fixed_real());
        add_column("ZZERO", fixed_real());
    }
    if (fallback) {
        add_column("GZIP_COMPRESSED_DATA", variable(column_type::uint8, max_of(tile_payload::holder::gzip_compressed)));
    }
    if (uncompressed) {
        add_column("UNCOMPRESSED_DATA", variable(column_of(stored), max_of(tile_payload::holder::uncompressed)));
    }
    const std::uint64_t row_size = columns.back().offset + columns.back().format.field_size();

    // The rows, then the heap: the tiles in the order of the rows.
    compressed_unit unit;
    const std::uint64_t rows = grid.count();
    const std::uint64_t table_bytes = checked_multiply(row_size, rows);
    unit.data.resize(checked_cast<std::size_t>(checked_add(table_bytes, heap)));
    std::uint64_t offset = 0;
    for (std::uint64_t r = 0; r < rows; ++r) {
        const encoded_tile& tile = encoded[checked_cast<std::size_t>(r)];
        const std::span<std::byte> row =
            std::span(unit.data).subspan(checked_cast<std::size_t>(r * row_size), checked_cast<std::size_t>(row_size));
        for (const table_column& column : columns) {
            const std::span<std::byte> field = row.subspan(checked_cast<std::size_t>(column.offset));
            if (column.name == "ZSCALE" || column.name == "ZZERO") {
                const double value = column.name == "ZSCALE" ? tile.scale : tile.zero;
                store_big_endian(field.first<8>(), std::bit_cast<std::uint64_t>(value));
                continue;
            }
            const bool holds =
                (column.name == "COMPRESSED_DATA" && tile.column == tile_payload::holder::compressed) ||
                (column.name == "GZIP_COMPRESSED_DATA" && tile.column == tile_payload::holder::gzip_compressed) ||
                (column.name == "UNCOMPRESSED_DATA" && tile.column == tile_payload::holder::uncompressed);
            if (holds) {
                store_descriptor(field, wide, tile.elements, offset);
            } else {
                store_descriptor(field, wide, 0, 0);
            }
        }
        std::ranges::copy(tile.bytes, unit.data.begin() + static_cast<std::ptrdiff_t>(table_bytes + offset));
        offset += tile.bytes.size();
    }

    // The cards of the table, then those of the image under their Z names (standard §10.1.1, §10.1.2).
    std::vector<card>& cards = unit.cards;
    cards.emplace_back("XTENSION", "BINTABLE", "binary table extension");
    cards.emplace_back("BITPIX", 8, "8-bit bytes");
    cards.emplace_back("NAXIS", 2, "2-dimensional binary table");
    cards.emplace_back("NAXIS1", row_size, "width of table in bytes");
    cards.emplace_back("NAXIS2", rows, "number of rows in table");
    cards.emplace_back("PCOUNT", heap, "size of special data area");
    cards.emplace_back("GCOUNT", 1, "one data group");
    cards.emplace_back("TFIELDS", columns.size(), "number of fields in each row");
    for (std::size_t c = 0; c < columns.size(); ++c) {
        const std::string n = std::to_string(c + 1);
        cards.emplace_back("TTYPE" + n, columns[c].name, "label for field " + n);
        cards.emplace_back("TFORM" + n, columns[c].format.to_string(), "data format of field " + n);
    }
    cards.emplace_back("ZIMAGE", true, "extension contains compressed image");
    for (std::size_t a = 0; a < tiles.size(); ++a) {
        cards.emplace_back("ZTILE" + std::to_string(a + 1), tiles[a], "size of tiles to be compressed");
    }
    cards.emplace_back("ZCMPTYPE", algorithm_name(algorithm), "compression algorithm");
    if (plan.quantized && nulls) {
        cards.emplace_back("ZBLANK", quantized_null, "null value in the compressed integer array");
    }
    if (algorithm == tile_algorithm::rice) {
        cards.emplace_back("ZNAME1", "BLOCKSIZE", "compression block size");
        cards.emplace_back("ZVAL1", options.rice_block_size, "pixels per block");
        cards.emplace_back("ZNAME2", "BYTEPIX", "bytes per pixel (1, 2, 4, or 8)");
        cards.emplace_back("ZVAL2", plan.quantized ? std::size_t{4} : width, "bytes per pixel (1, 2, 4, or 8)");
    } else if (algorithm == tile_algorithm::hcompress) {
        cards.emplace_back("ZNAME1", "SCALE", "HCOMPRESS scale factor");
        cards.emplace_back("ZVAL1", static_cast<double>(plan.hcompress_scale), "HCOMPRESS scale factor");
        cards.emplace_back("ZNAME2", "SMOOTH", "HCOMPRESS smooth option");
        cards.emplace_back("ZVAL2", options.hcompress_smooth ? 1 : 0, "HCOMPRESS smooth option");
    }
    if (plan.quantized) {
        switch (options.quantize_method) {
        case quantization::no_dither:
            cards.emplace_back("ZQUANTIZ", "NO_DITHER", "No dithering during quantization");
            break;
        case quantization::subtractive_dither_1:
            cards.emplace_back("ZQUANTIZ", "SUBTRACTIVE_DITHER_1", "Pixel Quantization Algorithm");
            break;
        case quantization::subtractive_dither_2:
            cards.emplace_back("ZQUANTIZ", "SUBTRACTIVE_DITHER_2", "Pixel Quantization Algorithm");
            break;
        }
        if (dithered) {
            cards.emplace_back("ZDITHER0", plan.seed, "dithering offset when quantizing floats");
        }
    }
    for (const card& entry : image_cards(image, primary)) {
        const std::string_view name = entry.keyword();
        if (name == "BSCALE" || name == "BZERO" || name == "BLANK") {
            cards.push_back(entry);
        } else {
            const std::string renamed = name == "SIMPLE"     ? std::string("ZSIMPLE")
                                        : name == "XTENSION" ? std::string("ZTENSION")
                                                             : "Z" + std::string(name);
            cards.emplace_back(renamed, entry.value(), entry.comment());
        }
    }
    if (primary && !image.header.index_of("EXTNAME")) {
        cards.emplace_back("EXTNAME", "COMPRESSED_IMAGE", "name of this binary table extension");
    }
    return unit;
}

encoded_tile encode_tile_again(const tile_encoding& encoding, std::uint64_t index, std::span<const std::uint64_t> size,
                               std::span<const std::byte> stored, std::optional<double> spacing, std::uint64_t hdu)
{
    image_info image;
    image.bitpix = encoding.stored;
    image.blank = encoding.blank;
    compression_options options;
    options.rice_block_size = static_cast<std::uint32_t>(encoding.block_size);
    options.quantize_method = encoding.method;
    // The spacing of the levels of the tile as it was, which a negative level gives as it is.
    if (spacing) {
        options.quantize_level = -*spacing;
    }
    const write_conversion conversion{};
    const image_plan plan{.image = image,
                          .conversion = conversion,
                          .options = options,
                          .algorithm = encoding.algorithm,
                          .stored = encoding.stored,
                          .quantized = encoding.quantized && spacing.has_value(),
                          .seed = encoding.seed,
                          .plio_offset = encoding.plio_offset,
                          .hcompress_scale = encoding.hcompress_scale,
                          .hdu = hdu};
    tile_encoder encoder(-1);
    encoder.stored.assign(stored.begin(), stored.end());
    encoded_tile tile;
    if (encoding.quantized && !spacing) {
        // A tile that was not quantized stays lossless, in GZIP_COMPRESSED_DATA (standard §10.1.3).
        tile.column = tile_payload::holder::gzip_compressed;
        encoder.gzip.compress(encoder.stored, tile.bytes);
        tile.elements = tile.bytes.size();
        return tile;
    }
    encode_gathered(plan, index, size, encoder, tile);
    return tile;
}

} // namespace openfits::detail
