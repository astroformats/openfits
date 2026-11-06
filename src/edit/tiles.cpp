// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "edit/tiles.h"

#include <openfits/error.h>
#include <openfits/table.h>

#include "checksum/checksum.h"
#include "compress/image_compress.h"
#include "compress/quantize.h"
#include "compress/tile_grid.h"
#include "core/checked_math.h"
#include "core/endian.h"
#include "core/parallel.h"
#include "data/conversion.h"
#include "table/table_description.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace openfits::detail {

namespace {

// The largest offset and count of a P descriptor that the writer writes (standard §7.3.5).
constexpr std::uint64_t descriptor_limit = 0x7FFF'FFFF;

[[noreturn]] void refuse(std::uint64_t hdu, const std::string& message)
{
    throw unsupported_error(errc::not_editable, message, {.hdu = hdu});
}

// The bytes of an array of count elements of a type.
std::uint64_t array_bytes(column_type type, std::uint64_t count)
{
    column_format format;
    format.type = type;
    format.repeat = count;
    return format.field_size();
}

struct descriptor
{
    std::uint64_t count = 0;
    std::uint64_t offset = 0;
};

descriptor read_descriptor(std::span<const std::byte> field, column_storage storage, std::uint64_t hdu)
{
    if (storage == column_storage::variable_p) {
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

void store_descriptor(std::span<std::byte> field, column_storage storage, const descriptor& array)
{
    if (storage == column_storage::variable_p) {
        store_big_endian(field.first<4>(), static_cast<std::uint32_t>(array.count));
        store_big_endian(field.subspan<4, 4>(), static_cast<std::uint32_t>(array.offset));
    } else {
        store_big_endian(field.first<8>(), array.count);
        store_big_endian(field.subspan<8, 8>(), array.offset);
    }
}

// A real value into a fixed field of one element of a column of floating-point numbers.
void store_real(std::span<std::byte> field, column_type type, double value, std::uint64_t hdu)
{
    if (type == column_type::float64) {
        store_big_endian(field.first<8>(), std::bit_cast<std::uint64_t>(value));
    } else if (type == column_type::float32) {
        store_big_endian(field.first<4>(), std::bit_cast<std::uint32_t>(static_cast<float>(value)));
    } else {
        refuse(hdu, "the ZSCALE or ZZERO column of the compressed image of unit " + std::to_string(hdu) +
                        " holds integers, which no tile quantized again can record");
    }
}

// An integer into a fixed field of one element of a column of integers.
void store_integer(std::span<std::byte> field, column_type type, std::int64_t value)
{
    switch (type) {
    case column_type::uint8:
        field[0] = static_cast<std::byte>(static_cast<std::uint8_t>(value));
        break;
    case column_type::int16:
        store_big_endian(field.first<2>(), static_cast<std::uint16_t>(value));
        break;
    case column_type::int32:
        store_big_endian(field.first<4>(), static_cast<std::uint32_t>(value));
        break;
    default:
        store_big_endian(field.first<8>(), static_cast<std::uint64_t>(value));
        break;
    }
}

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

// Calls visit(offset, at, bytes) for each row of the tile at index, its elements along the first axis: offset is where
// the row lies in the data of the image uncompressed, and at where in the tile, of width bytes for each value.
template <typename Visit>
void for_each_tile_row(const tile_grid& grid, std::uint64_t index, std::size_t width, Visit visit)
{
    const std::vector<std::uint64_t>& axes = grid.axes();
    std::vector<std::uint64_t> start(axes.size());
    std::vector<std::uint64_t> size(axes.size());
    grid.box(index, start, size);
    std::vector<std::uint64_t> k(axes.size(), 0);
    const std::uint64_t row_bytes = size[0] * width;
    for (std::uint64_t row = 0;; ++row) {
        std::uint64_t element = start[0];
        std::uint64_t stride = axes[0];
        for (std::size_t a = 1; a < axes.size(); ++a) {
            element += (start[a] + k[a]) * stride;
            stride *= axes[a];
        }
        visit(element * width, row * row_bytes, row_bytes);
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

// The tiles of the grid that the bytes written meet, values of width bytes.
std::vector<bool> changed_tiles(const tile_grid& grid, const byte_overlay& writes, std::size_t width)
{
    const std::vector<std::uint64_t>& axes = grid.axes();
    const std::vector<std::uint64_t>& tile = grid.tile();
    const std::vector<std::uint64_t>& counts = grid.counts();
    std::vector<bool> changed(checked_cast<std::size_t>(grid.count()), false);
    std::vector<std::uint64_t> coordinates(axes.size());
    for (const auto& [offset, bytes] : writes.stretches()) {
        if (bytes.empty()) {
            continue;
        }
        const std::uint64_t first = offset / width;
        const std::uint64_t last = (offset + bytes.size() - 1) / width;
        for (std::uint64_t row = first / axes[0]; row <= last / axes[0]; ++row) {
            const std::uint64_t low = row == first / axes[0] ? first % axes[0] : 0;
            const std::uint64_t high = row == last / axes[0] ? last % axes[0] : axes[0] - 1;
            // The tile of the row along every axis but the first, and the run of tiles along it.
            std::uint64_t rest = row;
            std::uint64_t base = 0;
            std::uint64_t stride = counts[0];
            for (std::size_t a = 1; a < axes.size(); ++a) {
                base += ((rest % axes[a]) / tile[a]) * stride;
                rest /= axes[a];
                stride *= counts[a];
            }
            for (std::uint64_t t = low / tile[0]; t <= high / tile[0]; ++t) {
                changed[checked_cast<std::size_t>(base + t)] = true;
            }
        }
    }
    return changed;
}

// A column of the table built again: its format, and the column of the table as it was that it continues.
struct table_column
{
    std::string name{};
    column_format format{};
    std::optional<std::size_t> old{};
    std::uint64_t offset = 0;
    std::uint64_t emax = 0;
};

// The bytes of the data of the table of the unit, in memory or read.
std::span<const std::byte> table_bytes(const thread_safe_source& source, const hdu_info& unit, std::uint64_t size,
                                       std::vector<std::byte>& held)
{
    if (!source.bytes().empty() && unit.data_offset() <= source.size() && size <= source.size() - unit.data_offset()) {
        return source.bytes().subspan(unit.data_offset(), size);
    }
    held.resize(checked_cast<std::size_t>(size));
    source.read(unit.data_offset(), held);
    return held;
}

} // namespace

recompressed_image recompress_image(const thread_safe_source& source, const hdu_info& unit,
                                    const compressed_image& image, const byte_overlay& writes,
                                    const openfits::limits& limits)
{
    const std::uint64_t hdu = unit.index();
    const bitpix stored = image.image.bitpix.value_or(bitpix::uint8);
    const std::size_t width = stored_size(stored);
    if (image.scale || image.zero) {
        refuse(hdu, "the compressed image of unit " + std::to_string(hdu) +
                        " gives ZSCALE and ZZERO as keywords, as old files do, which no tile quantized again keeps");
    }
    if (image.algorithm == tile_algorithm::rice && image.bytepix != (image.quantized ? 4U : width)) {
        refuse(hdu, "the RICE_1 tiles of the compressed image of unit " + std::to_string(hdu) + " have a BYTEPIX of " +
                        std::to_string(image.bytepix) +
                        ", which is not the size of their values: the library encodes no such tile");
    }
    const tile_grid grid(image.image.geometry.axes, image.tile);
    if (limits.max_tiles != 0 && grid.count() > limits.max_tiles) {
        throw limit_error(errc::too_many_tiles,
                          "the compressed image of unit " + std::to_string(hdu) + " has " +
                              std::to_string(grid.count()) + " tiles, more than limits::max_tiles",
                          {.hdu = hdu});
    }
    std::uint64_t tile_size = width;
    for (const std::uint64_t length : grid.tile()) {
        tile_size = checked_multiply(tile_size, length);
    }
    const std::uint64_t table_size = checked_add(image.heap_offset, image.heap_size);
    const std::uint64_t most =
        limits.max_allocation == 0 ? std::numeric_limits<std::uint64_t>::max() : limits.max_allocation;
    if ((limits.max_tile_bytes != 0 && tile_size > limits.max_tile_bytes) || tile_size > most || table_size > most) {
        throw limit_error(errc::allocation_too_large,
                          "the tiles or the table of the compressed image of unit " + std::to_string(hdu) +
                              " take more memory than the limits allow",
                          {.hdu = hdu});
    }
    std::vector<std::byte> held;
    const std::span<const std::byte> bytes = table_bytes(source, unit, table_size, held);
    const auto row_of = [&](std::uint64_t index) {
        return bytes.subspan(checked_cast<std::size_t>(index * image.row_size),
                             checked_cast<std::size_t>(image.row_size));
    };
    const std::span<const std::byte> heap =
        bytes.subspan(checked_cast<std::size_t>(image.heap_offset), checked_cast<std::size_t>(image.heap_size));
    const table_info table = describe_table(unit, false);

    // The tiles that the writes meet, decoded, changed and encoded again; the change of the sum of their values.
    recompressed_image result;
    const tile_encoding encoding{.algorithm = image.algorithm,
                                 .stored = stored,
                                 .block_size = image.block_size,
                                 .hcompress_scale =
                                     image.algorithm == tile_algorithm::hcompress
                                         ? static_cast<float>(unit.header().find<double>("ZVAL1").value_or(0.0))
                                         : 0.0F,
                                 .quantized = image.quantized,
                                 .method = image.method,
                                 .seed = image.seed,
                                 .plio_offset = image.plio_offset,
                                 .blank = image.image.blank};
    const std::vector<bool> changed = changed_tiles(grid, writes, width);
    std::vector<std::uint64_t> tiles;
    for (std::uint64_t t = 0; t < grid.count(); ++t) {
        if (changed[checked_cast<std::size_t>(t)]) {
            tiles.push_back(t);
        }
    }
    // A tile encoded again, and the sums of its values before and after, which combine in any order.
    struct tile_change
    {
        encoded_tile fresh{};
        std::uint32_t before = 0;
        std::uint32_t after = 0;
    };
    std::vector<tile_change> changes(tiles.size());
    // Each worker decodes, changes and encodes again its share of the tiles, as a save encodes its tiles, with memory
    // of its own; the decoders and encoders, the overlay and the image are only read.
    const std::size_t share = std::min(concurrency(), tiles.size());
    parallel_for(share, [&](std::size_t worker) {
        std::vector<std::byte> values;
        std::vector<std::byte> restored;
        std::vector<std::uint64_t> start(grid.axes().size());
        std::vector<std::uint64_t> size(grid.axes().size());
        for (std::size_t i = worker; i < tiles.size(); i += share) {
            const std::uint64_t t = tiles[i];
            tile_change& change = changes[i];
            grid.box(t, start, size);
            const tile_payload old = payload_of(image, row_of(t), heap, t, hdu);
            values.resize(checked_cast<std::size_t>(grid.elements(t) * width));
            decode_tile(image, t, size, old, values);
            for_each_tile_row(grid, t, width, [&](std::uint64_t offset, std::uint64_t at, std::uint64_t length) {
                const std::span<std::byte> row =
                    std::span(values).subspan(checked_cast<std::size_t>(at), checked_cast<std::size_t>(length));
                change.before = add_sums(change.before, sum_at(row, offset));
                writes.apply(offset, row);
            });
            const std::optional<double> spacing = image.quantized && old.column == tile_payload::holder::compressed
                                                      ? std::optional(old.scale)
                                                      : std::nullopt;
            change.fresh = encode_tile_again(encoding, t, size, values, spacing, hdu);
            const encoded_tile& fresh = change.fresh;
            if (fresh.nulls && !image.nulls && image.null && *image.null != quantized_null) {
                refuse(hdu, "a tile of the compressed image of unit " + std::to_string(hdu) +
                                " quantized again holds undefined values, which its ZBLANK, " +
                                std::to_string(*image.null) + ", does not code as the library codes them");
            }
            // The values that the tile gives once encoded again, lossy or not, which a read decodes.
            const tile_payload again{.column = fresh.column,
                                     .bytes = fresh.bytes,
                                     .uncompressed_type = column_of(stored),
                                     .scale = fresh.scale,
                                     .zero = fresh.zero,
                                     .null = fresh.nulls ? std::optional<std::int64_t>(quantized_null) : old.null};
            restored.resize(values.size());
            decode_tile(image, t, size, again, restored);
            for_each_tile_row(grid, t, width, [&](std::uint64_t offset, std::uint64_t at, std::uint64_t length) {
                change.after =
                    add_sums(change.after, sum_at(std::span(restored).subspan(checked_cast<std::size_t>(at),
                                                                              checked_cast<std::size_t>(length)),
                                                  offset));
            });
        }
    });
    std::map<std::uint64_t, encoded_tile> encoded;
    bool nulls = false;
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        result.change.before = add_sums(result.change.before, changes[i].before);
        result.change.after = add_sums(result.change.after, changes[i].after);
        nulls = nulls || changes[i].fresh.nulls;
        encoded.emplace(tiles[i], std::move(changes[i].fresh));
    }

    // The columns of the table built again: those it has, and those that the tiles encoded again need.
    std::vector<table_column> columns;
    columns.reserve(table.columns.size() + 2);
    for (std::size_t c = 0; c < table.columns.size(); ++c) {
        columns.push_back(
            {.name = table.columns[c].name, .format = table.columns[c].format.value_or(column_format{}), .old = c});
    }
    const auto needs = [&encoded](tile_payload::holder holder) {
        return std::ranges::any_of(encoded, [holder](const auto& entry) { return entry.second.column == holder; });
    };
    const column_storage descriptors = image.compressed.format.storage;
    const auto add_column = [&](std::string name, column_type type) {
        column_format format;
        format.type = type;
        format.storage = descriptors;
        columns.push_back({.name = std::move(name), .format = format});
        return columns.size() - 1;
    };
    std::optional<std::size_t> gzip_column =
        image.gzip_compressed ? std::optional(image.gzip_compressed->index) : std::nullopt;
    std::optional<std::size_t> raw_column =
        image.uncompressed ? std::optional(image.uncompressed->index) : std::nullopt;
    const std::size_t old_columns = columns.size();
    if (!gzip_column && needs(tile_payload::holder::gzip_compressed)) {
        gzip_column = add_column("GZIP_COMPRESSED_DATA", column_type::uint8);
    }
    if (!raw_column && needs(tile_payload::holder::uncompressed)) {
        raw_column = add_column("UNCOMPRESSED_DATA", column_of(stored));
    }

    // The array of each cell: the bytes of a tile encoded again in the column that holds it, none in its other columns
    // and its mask, and the arrays of the table as they were otherwise.
    const auto array_of = [&](std::uint64_t t, std::size_t c, std::uint64_t& count) -> std::span<const std::byte> {
        const table_column& column = columns[c];
        const auto found = encoded.find(t);
        if (found != encoded.end()) {
            const encoded_tile& fresh = found->second;
            const bool holder = (c == image.compressed.index && fresh.column == tile_payload::holder::compressed) ||
                                (c == gzip_column && fresh.column == tile_payload::holder::gzip_compressed) ||
                                (c == raw_column && fresh.column == tile_payload::holder::uncompressed);
            const bool tile_column = c == image.compressed.index || c == gzip_column || c == raw_column ||
                                     (image.mask && c == image.mask->index);
            if (holder) {
                count = fresh.elements;
                return fresh.bytes;
            }
            if (tile_column) {
                count = 0;
                return {};
            }
        }
        if (!column.old) {
            count = 0;
            return {};
        }
        const column_info& info = table.columns[*column.old];
        const descriptor array =
            read_descriptor(row_of(t).subspan(checked_cast<std::size_t>(info.offset)), column.format.storage, hdu);
        const std::uint64_t length = array_bytes(column.format.type, array.count);
        if (array.offset > heap.size() || length > heap.size() - array.offset) {
            throw invalid_data_error(errc::invalid_descriptor,
                                     "a descriptor of the compressed image points outside its heap (standard §7.3.5)",
                                     {.hdu = hdu});
        }
        count = array.count;
        return heap.subspan(checked_cast<std::size_t>(array.offset), checked_cast<std::size_t>(length));
    };

    // The heap and the largest arrays, which choose the descriptors: Q ones once a P one cannot hold a count or an
    // offset.
    std::uint64_t heap_size = 0;
    bool wide = false;
    for (std::uint64_t t = 0; t < grid.count(); ++t) {
        for (std::size_t c = 0; c < columns.size(); ++c) {
            if (columns[c].format.storage == column_storage::fixed) {
                continue;
            }
            std::uint64_t count = 0;
            const std::span<const std::byte> array = array_of(t, c, count);
            columns[c].emax = std::max(columns[c].emax, count);
            wide = wide || count > descriptor_limit || (count > 0 && heap_size > descriptor_limit);
            heap_size = checked_add<std::uint64_t>(heap_size, array.size());
        }
    }
    std::uint64_t row_size = 0;
    for (table_column& column : columns) {
        if (column.format.storage != column_storage::fixed) {
            if (wide) {
                column.format.storage = column_storage::variable_q;
            }
            column.format.max_elements = column.emax;
        }
        column.offset = row_size;
        row_size = checked_add(row_size, column.format.field_size());
    }
    if (limits.max_heap_bytes != 0 && heap_size > limits.max_heap_bytes) {
        throw limit_error(errc::array_too_large,
                          "the heap of the compressed image of unit " + std::to_string(hdu) +
                              " once its tiles are encoded again is larger than limits::max_heap_bytes",
                          {.hdu = hdu});
    }
    const std::uint64_t rows_size = checked_multiply(row_size, grid.count());
    const std::uint64_t data_size = checked_add(rows_size, heap_size);
    if (data_size > most) {
        throw limit_error(errc::allocation_too_large,
                          "the table of the compressed image of unit " + std::to_string(hdu) +
                              " once its tiles are encoded again takes more than limits::max_allocation",
                          {.hdu = hdu});
    }

    // The rows, then the heap, in the order of the rows and of the columns.
    result.data.resize(checked_cast<std::size_t>(data_size));
    std::uint64_t heap_at = 0;
    for (std::uint64_t t = 0; t < grid.count(); ++t) {
        const std::span<std::byte> row =
            std::span(result.data)
                .subspan(checked_cast<std::size_t>(t * row_size), checked_cast<std::size_t>(row_size));
        const auto found = encoded.find(t);
        for (std::size_t c = 0; c < columns.size(); ++c) {
            const table_column& column = columns[c];
            const std::span<std::byte> field = row.subspan(checked_cast<std::size_t>(column.offset),
                                                           checked_cast<std::size_t>(column.format.field_size()));
            if (column.format.storage != column_storage::fixed) {
                std::uint64_t count = 0;
                const std::span<const std::byte> array = array_of(t, c, count);
                std::ranges::copy(array, result.data.begin() + static_cast<std::ptrdiff_t>(rows_size + heap_at));
                store_descriptor(field, column.format.storage, {.count = count, .offset = count == 0 ? 0 : heap_at});
                heap_at += array.size();
                continue;
            }
            if (!column.old) {
                continue;
            }
            const column_info& info = table.columns[*column.old];
            if (found != encoded.end() && image.scales && c == image.scales->index) {
                store_real(field, column.format.type, found->second.scale, hdu);
            } else if (found != encoded.end() && image.zeros && c == image.zeros->index) {
                store_real(field, column.format.type, found->second.zero, hdu);
            } else if (found != encoded.end() && image.nulls && c == image.nulls->index && found->second.nulls) {
                store_integer(field, column.format.type, quantized_null);
            } else {
                std::ranges::copy(row_of(t).subspan(checked_cast<std::size_t>(info.offset), field.size()),
                                  field.begin());
            }
        }
    }

    // The cards of the table that change.
    std::vector<card>& cards = result.cards;
    cards.emplace_back("NAXIS1", row_size, "width of table in bytes");
    cards.emplace_back("PCOUNT", heap_size, "size of special data area");
    if (columns.size() != old_columns) {
        cards.emplace_back("TFIELDS", columns.size(), "number of fields in each row");
    }
    for (std::size_t c = 0; c < columns.size(); ++c) {
        const std::string n = std::to_string(c + 1);
        if (c >= old_columns) {
            cards.emplace_back("TTYPE" + n, columns[c].name, "label for field " + n);
        }
        if (c >= old_columns || columns[c].format.storage != column_storage::fixed) {
            cards.emplace_back("TFORM" + n, columns[c].format.to_string(), "data format of field " + n);
        }
    }
    if (nulls && image.quantized && !image.nulls && !image.null) {
        cards.emplace_back("ZBLANK", quantized_null, "null value in the compressed integer array");
    }
    if (unit.header().contains("THEAP")) {
        result.removed.emplace_back("THEAP");
    }
    return result;
}

} // namespace openfits::detail
