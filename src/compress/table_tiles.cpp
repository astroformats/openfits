// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "compress/table_tiles.h"

#include <openfits/error.h>

#include "compress/gzip.h"
#include "compress/rice.h"
#include "compress/shuffle.h"
#include "core/ascii.h"
#include "core/checked_math.h"
#include "core/endian.h"
#include "core/quote.h"
#include "core/scratch_buffer.h"
#include "hdu/walk.h"
#include "header/record.h"
#include "io/range_check.h"
#include "table/table_description.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace openfits::detail {

namespace {

// The rows of a tile by default: about 10 MB of rows, as CFITSIO chooses them.
constexpr std::uint64_t default_tile_bytes = 10'000'000;

[[noreturn]] void refuse(std::uint64_t hdu, const std::string& message, std::string_view keyword = {})
{
    throw invalid_data_error(errc::invalid_compression, message, {.hdu = hdu, .keyword = std::string(keyword)});
}

[[noreturn]] void throw_corrupt(const std::string& message)
{
    throw integrity_error(errc::corrupt_compressed_data, message);
}

std::string indexed(std::string_view root, std::size_t index)
{
    return std::string(root) + std::to_string(index);
}

template <typename T> std::optional<T> keyword_of(const header& cards, const std::string& keyword, std::uint64_t hdu)
{
    try {
        return cards.find<T>(keyword);
    } catch (const invalid_data_error&) {
        refuse(hdu, keyword + " does not hold a value of its type (standard §10.3.1)", keyword);
    }
}

template <typename T> T required(const header& cards, const std::string& keyword, std::uint64_t hdu)
{
    const std::optional<T> value = keyword_of<T>(cards, keyword, hdu);
    if (!value) {
        refuse(hdu, "the compressed table has no " + keyword + ", which standard §10.3.1 requires", keyword);
    }
    return *value;
}

// The bytes of an element of a column of the type, and of a bit column, its bytes for count bits.
std::size_t element_bytes(column_type type) noexcept
{
    switch (type) {
    case column_type::int16:
        return 2;
    case column_type::int32:
    case column_type::float32:
        return 4;
    case column_type::int64:
    case column_type::float64:
    case column_type::complex_float32:
        return 8;
    case column_type::complex_float64:
        return 16;
    case column_type::logical:
    case column_type::bit:
    case column_type::uint8:
    case column_type::character:
        break;
    }
    return 1;
}

std::uint64_t array_bytes(column_type type, std::uint64_t count)
{
    if (type == column_type::bit) {
        return (count + 7) / 8;
    }
    return checked_multiply<std::uint64_t>(count, element_bytes(type));
}

// The values of a field that GZIP_2 shuffles, and their size: the numbers of 2, 4 and 8 bytes. A variable-length array
// of single-precision complex values is shuffled as values of 8 bytes, as CFITSIO shuffles them; its other complex
// values and its fixed ones are not, which is how CFITSIO writes them.
std::size_t shuffle_size(column_type type, bool variable) noexcept
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
    case column_type::complex_float32:
        return variable ? 8 : 1;
    case column_type::complex_float64:
    case column_type::logical:
    case column_type::bit:
    case column_type::uint8:
    case column_type::character:
        break;
    }
    return 1;
}

// The bytes of a value of RICE_1 of a column, 0 for a type that it does not take.
std::size_t rice_bytes(column_type type) noexcept
{
    switch (type) {
    case column_type::uint8:
        return 1;
    case column_type::int16:
        return 2;
    case column_type::int32:
        return 4;
    default:
        return 0;
    }
}

bool allows(tile_algorithm algorithm, column_type type) noexcept
{
    switch (algorithm) {
    case tile_algorithm::gzip_1:
    case tile_algorithm::nocompress:
        return true;
    case tile_algorithm::gzip_2:
        return shuffle_size(type, false) > 1;
    case tile_algorithm::rice:
        return rice_bytes(type) != 0;
    case tile_algorithm::plio:
    case tile_algorithm::hcompress:
        break;
    }
    return false;
}

// The algorithm of a type by default: RICE_1 for 32-bit integers, GZIP_2 for the other numbers that it shuffles, and
// GZIP_1 for the rest, complex values among them, whose shuffled form CFITSIO does not read.
tile_algorithm default_algorithm(column_type type) noexcept
{
    if (type == column_type::int32) {
        return tile_algorithm::rice;
    }
    return shuffle_size(type, false) > 1 ? tile_algorithm::gzip_2 : tile_algorithm::gzip_1;
}

// Decompresses the bytes of a column of a tile, or of an array, into output, by algorithm, for elements of type.
void decompress_bytes(std::span<const std::byte> input, tile_algorithm algorithm, column_type type, bool variable,
                      std::span<std::byte> output)
{
    switch (algorithm) {
    case tile_algorithm::gzip_1:
        gzip_decompress(input, output);
        return;
    case tile_algorithm::gzip_2: {
        const std::size_t size = shuffle_size(type, variable);
        if (size == 1) {
            gzip_decompress(input, output);
            return;
        }
        scratch_buffer shuffled(output.size());
        gzip_decompress(input, shuffled.bytes());
        unshuffle_bytes(shuffled.bytes(), output, size);
        return;
    }
    case tile_algorithm::rice: {
        const std::size_t size = rice_bytes(type);
        if (size == 0 || output.size() % size != 0) {
            throw_corrupt("RICE_1 holds no column of this type");
        }
        const std::size_t count = output.size() / size;
        if (size == 1) {
            std::vector<std::uint8_t> values(count);
            rice_decode(input, rice_block_32, std::span<std::uint8_t>(values));
            std::ranges::transform(values, output.begin(), [](std::uint8_t value) { return std::byte{value}; });
        } else if (size == 2) {
            std::vector<std::uint16_t> values(count);
            rice_decode(input, rice_block_32, std::span<std::uint16_t>(values));
            for (std::size_t i = 0; i < count; ++i) {
                store_big_endian(output.subspan(i * 2).first<2>(), values[i]);
            }
        } else {
            std::vector<std::uint32_t> values(count);
            rice_decode(input, rice_block_32, std::span<std::uint32_t>(values));
            for (std::size_t i = 0; i < count; ++i) {
                store_big_endian(output.subspan(i * 4).first<4>(), values[i]);
            }
        }
        return;
    }
    case tile_algorithm::nocompress:
        if (input.size() != output.size()) {
            throw_corrupt("the NOCOMPRESS column holds " + std::to_string(input.size()) + " bytes, and its fields " +
                          std::to_string(output.size()));
        }
        std::ranges::copy(input, output.begin());
        return;
    case tile_algorithm::plio:
    case tile_algorithm::hcompress:
        break;
    }
    throw_corrupt("the algorithm of the column compresses no table");
}

// Compresses bytes of a column of a tile, or of an array, by algorithm, for elements of type, into output.
void compress_bytes(std::span<const std::byte> input, tile_algorithm algorithm, column_type type, bool variable,
                    gzip_encoder& gzip, std::vector<std::byte>& output)
{
    output.clear();
    switch (algorithm) {
    case tile_algorithm::gzip_1:
        gzip.compress(input, output);
        return;
    case tile_algorithm::gzip_2: {
        const std::size_t size = shuffle_size(type, variable);
        if (size == 1) {
            gzip.compress(input, output);
            return;
        }
        std::vector<std::byte> shuffled(input.size());
        shuffle_bytes(input, shuffled, size);
        gzip.compress(shuffled, output);
        return;
    }
    case tile_algorithm::rice: {
        const std::size_t size = rice_bytes(type);
        const std::size_t count = input.size() / size;
        if (size == 1) {
            std::vector<std::uint8_t> values(count);
            std::ranges::transform(input, values.begin(),
                                   [](std::byte value) { return std::to_integer<std::uint8_t>(value); });
            rice_encode(std::span<const std::uint8_t>(values), rice_block_32, output);
        } else if (size == 2) {
            std::vector<std::uint16_t> values(count);
            for (std::size_t i = 0; i < count; ++i) {
                values[i] = load_big_endian<std::uint16_t>(input.subspan(i * 2).first<2>());
            }
            rice_encode(std::span<const std::uint16_t>(values), rice_block_32, output);
        } else {
            std::vector<std::uint32_t> values(count);
            for (std::size_t i = 0; i < count; ++i) {
                values[i] = load_big_endian<std::uint32_t>(input.subspan(i * 4).first<4>());
            }
            rice_encode(std::span<const std::uint32_t>(values), rice_block_32, output);
        }
        return;
    }
    case tile_algorithm::nocompress:
    case tile_algorithm::plio:
    case tile_algorithm::hcompress:
        break;
    }
    output.assign(input.begin(), input.end());
}

// A descriptor of a variable-length array: its elements and its offset.
struct descriptor
{
    std::uint64_t count = 0;
    std::uint64_t offset = 0;
};

descriptor read_descriptor(std::span<const std::byte> field, column_storage storage)
{
    if (storage == column_storage::variable_p) {
        return {.count = load_big_endian<std::uint32_t>(field.first<4>()),
                .offset = load_big_endian<std::uint32_t>(field.subspan<4, 4>())};
    }
    return {.count = load_big_endian<std::uint64_t>(field.first<8>()),
            .offset = load_big_endian<std::uint64_t>(field.subspan<8, 8>())};
}

std::size_t descriptor_size(column_storage storage) noexcept
{
    return storage == column_storage::variable_p ? 8 : 16;
}

// The bytes of a range of a buffer, which must lie within it.
std::span<const std::byte> within(std::span<const std::byte> bytes, std::uint64_t offset, std::uint64_t size,
                                  std::uint64_t hdu)
{
    if (offset > bytes.size() || size > bytes.size() - offset) {
        throw invalid_data_error(errc::invalid_descriptor,
                                 "a descriptor of the compressed table points outside its heap (standard §7.3.5)",
                                 {.hdu = hdu});
    }
    return bytes.subspan(offset, size);
}

std::span<std::byte> within(std::span<std::byte> bytes, std::uint64_t offset, std::uint64_t size, std::uint64_t hdu)
{
    if (offset > bytes.size() || size > bytes.size() - offset) {
        throw invalid_data_error(errc::invalid_descriptor,
                                 "a descriptor of the table that the compressed table holds points outside its heap "
                                 "(standard §7.3.5)",
                                 {.hdu = hdu});
    }
    return bytes.subspan(offset, size);
}

bool is_indexed(std::string_view name, std::string_view root) noexcept
{
    return name.starts_with(root) && name.size() > root.size() &&
           std::ranges::all_of(name.substr(root.size()), [](char c) { return c >= '0' && c <= '9'; });
}

} // namespace

compressed_table describe_compressed_table(const hdu_info& unit)
{
    const std::uint64_t hdu = unit.index();
    const header& cards = unit.header();
    compressed_table table;
    table.table = describe_table(unit, false);
    const auto non_negative = [&](const std::string& keyword) {
        const auto value = required<std::int64_t>(cards, keyword, hdu);
        if (value < 0) {
            refuse(hdu, keyword + " is " + std::to_string(value) + ", a negative size", keyword);
        }
        return static_cast<std::uint64_t>(value);
    };
    table.rows = non_negative("ZNAXIS2");
    table.row_size = non_negative("ZNAXIS1");
    table.heap_size = non_negative("ZPCOUNT");
    const auto tile_rows = required<std::int64_t>(cards, "ZTILELEN", hdu);
    if (tile_rows < 1) {
        refuse(hdu, "ZTILELEN is " + std::to_string(tile_rows) + ", where a tile has at least one row", "ZTILELEN");
    }
    // A tile longer than the table holds its rows, as CFITSIO reads it.
    table.tile_rows = std::min(static_cast<std::uint64_t>(tile_rows), std::max<std::uint64_t>(table.rows, 1));
    const std::uint64_t rows_bytes = checked_multiply(table.rows, table.row_size);
    table.heap_offset = rows_bytes;
    if (const std::optional<std::int64_t> theap = keyword_of<std::int64_t>(cards, "ZTHEAP", hdu)) {
        if (std::cmp_less(*theap, rows_bytes)) {
            refuse(hdu, "ZTHEAP is " + std::to_string(*theap) + ", within the rows of the table", "ZTHEAP");
        }
        table.heap_offset = static_cast<std::uint64_t>(*theap);
    }
    const std::uint64_t tiles = table.rows == 0 ? 0 : ((table.rows - 1) / table.tile_rows) + 1;
    if (tiles != table.table.rows) {
        refuse(hdu,
               "the compressed table has " + std::to_string(table.table.rows) + " rows, and its table " +
                   std::to_string(tiles) + " tiles of " + std::to_string(table.tile_rows) + " rows",
               "NAXIS2");
    }
    std::uint64_t offset = 0;
    for (std::size_t n = 1; n <= table.table.columns.size(); ++n) {
        const column_info& stored = table.table.columns[n - 1];
        if (!stored.format || stored.format->storage == column_storage::fixed ||
            stored.format->type != column_type::uint8) {
            refuse(hdu,
                   "column " + std::to_string(n) +
                       " of the compressed table is not an array of bytes, 1QB "
                       "(standard §10.3.2)",
                   indexed("TFORM", n));
        }
        const std::string zform = indexed("ZFORM", n);
        compressed_column column;
        try {
            column.format = column_format::parse_binary(required<std::string>(cards, zform, hdu));
        } catch (const invalid_data_error& failure) {
            refuse(hdu, zform + " is not a format of standard §7.3.1: " + failure.what(), zform);
        }
        const std::string ctype = indexed("ZCTYP", n);
        if (const std::optional<std::string> name = keyword_of<std::string>(cards, ctype, hdu)) {
            const std::optional<tile_algorithm> algorithm = algorithm_of(*name);
            if (!algorithm || *algorithm == tile_algorithm::plio || *algorithm == tile_algorithm::hcompress) {
                throw unsupported_error(errc::unsupported_compression,
                                        ctype + " is " + quote(*name) +
                                            ", which is not an algorithm of tables of standard §10.3.5",
                                        {.hdu = hdu, .keyword = ctype});
            }
            column.algorithm = *algorithm;
        }
        column.offset = offset;
        column.field = column.format.field_size();
        offset = checked_add(offset, column.field);
        table.columns.push_back(column);
    }
    if (offset > table.row_size) {
        refuse(hdu,
               "the fields of ZFORMn take " + std::to_string(offset) + " bytes, more than ZNAXIS1, " +
                   std::to_string(table.row_size),
               "ZNAXIS1");
    }
    return table;
}

header restored_table_header(const header& compressed)
{
    std::vector<card> restored;
    const std::optional<std::size_t> ztheap = compressed.index_of("ZTHEAP");
    const bool has_theap = compressed.index_of("THEAP").has_value();
    for (const card& entry : compressed.cards()) {
        const std::string_view name = entry.keyword();
        const auto restore = [&](std::string_view from) {
            if (const std::optional<std::size_t> position = compressed.index_of(from)) {
                restored.push_back(renamed_card(compressed[*position], name));
            }
        };
        if (name == "NAXIS1" || name == "NAXIS2" || name == "PCOUNT") {
            restore("Z" + std::string(name));
        } else if (is_indexed(name, "TFORM")) {
            restore("Z" + std::string(name.substr(1)));
        } else if (name == "THEAP") {
            // CFITSIO keeps the THEAP of the table as it was, which gives the heap of both.
            if (ztheap) {
                restored.push_back(renamed_card(compressed[*ztheap], "THEAP"));
            } else {
                restored.push_back(entry);
            }
        } else if (name == "ZTHEAP") {
            if (!has_theap) {
                restored.push_back(renamed_card(entry, "THEAP"));
            }
        } else if (name == "ZHECKSUM") {
            restored.push_back(renamed_card(entry, "CHECKSUM"));
        } else if (name == "ZDATASUM") {
            restored.push_back(renamed_card(entry, "DATASUM"));
        } else if (name == "CHECKSUM" || name == "DATASUM" || name == "ZTABLE" || name == "ZTILELEN" ||
                   name == "ZNAXIS1" || name == "ZNAXIS2" || name == "ZPCOUNT" || name == "ZHEAPPTR" ||
                   is_indexed(name, "ZFORM") || is_indexed(name, "ZCTYP")) {
            continue;
        } else {
            restored.push_back(entry);
        }
    }
    return header(std::move(restored));
}

hdu_info restored_table_unit(const hdu_info& unit, const compressed_table& table)
{
    unit_structure structure;
    structure.kind = hdu_kind::bintable;
    structure.data_size = checked_add(table.heap_offset, table.heap_size);
    structure.name = unit.name();
    structure.version = unit.version();
    return hdu_access::make(unit.index(), restored_table_header(unit.header()), 0, 0, std::move(structure), {});
}

std::vector<std::byte> decompress_table(const thread_safe_source& source, const hdu_info& unit,
                                        const compressed_table& table, const openfits::limits& limits)
{
    const std::uint64_t hdu = unit.index();
    if (limits.max_heap_bytes != 0 && table.heap_size > limits.max_heap_bytes) {
        throw limit_error(errc::array_too_large,
                          "the heap of the table that unit " + std::to_string(hdu) + " holds, " +
                              std::to_string(table.heap_size) + " bytes, is larger than limits::max_heap_bytes",
                          {.hdu = hdu});
    }
    const std::uint64_t size = checked_add(table.heap_offset, table.heap_size);
    if (limits.max_allocation != 0 && size > limits.max_allocation) {
        throw limit_error(errc::allocation_too_large,
                          "the table that unit " + std::to_string(hdu) + " holds takes " + std::to_string(size) +
                              " bytes, more than limits::max_allocation",
                          {.hdu = hdu});
    }
    // The data of the compressed table, its rows and its heap, from the source.
    const table_info& stored = table.table;
    const std::uint64_t stored_size = checked_add(stored.heap_offset, stored.heap_size);
    std::vector<std::byte> held;
    std::span<const std::byte> compressed;
    if (!source.bytes().empty()) {
        check_read_range(source.size(), unit.data_offset(), stored_size, source.description());
        compressed = source.bytes().subspan(unit.data_offset(), stored_size);
    } else {
        held.resize(checked_cast<std::size_t>(stored_size));
        source.read(unit.data_offset(), held);
        compressed = held;
    }
    const std::span<const std::byte> heap = compressed.subspan(stored.heap_offset);

    std::vector<std::byte> data(checked_cast<std::size_t>(size));
    const std::span<std::byte> rows(data);
    const std::span<std::byte> restored_heap = rows.subspan(table.heap_offset);
    std::vector<std::byte> chunk;
    std::vector<std::byte> block;
    for (std::uint64_t tile = 0; tile < stored.rows; ++tile) {
        const std::uint64_t first = tile * table.tile_rows;
        const std::uint64_t count = std::min(table.tile_rows, table.rows - first);
        const std::span<const std::byte> row = compressed.subspan(tile * stored.row_size, stored.row_size);
        for (std::size_t c = 0; c < table.columns.size(); ++c) {
            const compressed_column& column = table.columns[c];
            const column_info& cell_column = stored.columns[c];
            // The parse of the keywords refuses a column that does not hold descriptors.
            const column_storage storage =
                cell_column.format ? cell_column.format->storage : column_storage::variable_p;
            const descriptor cell = read_descriptor(row.subspan(cell_column.offset), storage);
            const std::span<const std::byte> cell_bytes = within(heap, cell.offset, cell.count, hdu);
            const bool variable = column.format.storage != column_storage::fixed;
            try {
                if (column.field == 0) {
                    continue;
                }
                if (!variable) {
                    chunk.resize(checked_cast<std::size_t>(count * column.field));
                    decompress_bytes(cell_bytes, column.algorithm, column.format.type, false, chunk);
                    for (std::uint64_t r = 0; r < count; ++r) {
                        std::ranges::copy(
                            std::span(chunk).subspan(r * column.field, column.field),
                            rows.begin() + static_cast<std::ptrdiff_t>(((first + r) * table.row_size) + column.offset));
                    }
                    continue;
                }
                // The descriptors of the table, then those of the compressed arrays, compressed with GZIP_1.
                const std::size_t own = descriptor_size(column.format.storage);
                block.resize(checked_cast<std::size_t>(count * (own + 16)));
                gzip_decompress(cell_bytes, block);
                for (std::uint64_t r = 0; r < count; ++r) {
                    const std::span<const std::byte> original = std::span<const std::byte>(block).subspan(r * own, own);
                    std::ranges::copy(original, rows.begin() + static_cast<std::ptrdiff_t>(
                                                                   ((first + r) * table.row_size) + column.offset));
                    const descriptor array = read_descriptor(original, column.format.storage);
                    const descriptor packed =
                        read_descriptor(std::span<const std::byte>(block).subspan((count * own) + (r * 16), 16),
                                        column_storage::variable_q);
                    const std::uint64_t bytes = array_bytes(column.format.type, array.count);
                    if (bytes == 0) {
                        continue;
                    }
                    const std::span<std::byte> target = within(restored_heap, array.offset, bytes, hdu);
                    const std::span<const std::byte> source_bytes = within(heap, packed.offset, packed.count, hdu);
                    if (packed.count == bytes) {
                        // An array that compression would not have made smaller, stored as it is.
                        std::ranges::copy(source_bytes, target.begin());
                    } else {
                        decompress_bytes(source_bytes, column.algorithm, column.format.type, true, target);
                    }
                }
            } catch (const integrity_error& failure) {
                throw integrity_error(failure.code(),
                                      "column " + std::to_string(c + 1) + " of tile " + std::to_string(tile + 1) +
                                          " of the compressed table of unit " + std::to_string(hdu) + ": " +
                                          failure.what(),
                                      {.hdu = hdu, .column = c + 1});
            }
        }
    }
    return data;
}

compressed_unit compress_table(const table_model& model, std::span<const std::byte> data,
                               const compression_options& options, std::uint64_t hdu,
                               const std::function<void(std::uint64_t)>& advance)
{
    const table_info& info = model.info;
    const header& caller = info.header;
    const std::size_t fields = model.columns.size();
    if (options.columns.size() > fields) {
        throw usage_error(errc::invalid_argument,
                          "the compression gives the algorithms of " + std::to_string(options.columns.size()) +
                              " columns, and the table of unit " + std::to_string(hdu) + " has " +
                              std::to_string(fields),
                          {.hdu = hdu});
    }
    const auto directive = [&caller](const std::string& keyword) -> std::optional<tile_algorithm> {
        try {
            if (const std::optional<std::string> name = caller.find<std::string>(keyword)) {
                return algorithm_of(*name);
            }
        } catch (const invalid_data_error&) {
            // A directive that is not a string names no algorithm.
            return std::nullopt;
        }
        return std::nullopt;
    };

    // The algorithm of each column: the options of the column, the directives of standard §10.3.3, the options of the
    // table, then the default of its type; one that the column cannot take is refused when the options of the column
    // give it, and replaced by the default otherwise.
    std::vector<tile_algorithm> algorithms(fields);
    for (std::size_t c = 0; c < fields; ++c) {
        const column_type type = model.columns[c].format.type;
        const auto usable = [type](std::optional<tile_algorithm> algorithm) -> std::optional<tile_algorithm> {
            if (algorithm && *algorithm == tile_algorithm::gzip_2 && !allows(*algorithm, type)) {
                // Bytes shuffled by one byte are the bytes themselves.
                return tile_algorithm::gzip_1;
            }
            return algorithm && allows(*algorithm, type) ? algorithm : std::nullopt;
        };
        if (c < options.columns.size() && options.columns[c] != compression::automatic) {
            const compression_options own{.type = options.columns[c] == compression::none ? compression::nocompress
                                                                                          : options.columns[c]};
            const tile_algorithm wanted = resolve_algorithm(own, bitpix::uint8);
            const std::optional<tile_algorithm> chosen = usable(wanted);
            if (!chosen) {
                throw validation_error(errc::invalid_compression,
                                       std::string(algorithm_name(wanted)) + " does not compress column " +
                                           std::to_string(c + 1) + " of the table of unit " + std::to_string(hdu) +
                                           " (standard §10.3.5)",
                                       {.hdu = hdu, .keyword = "ZCTYP" + std::to_string(c + 1), .column = c + 1});
            }
            algorithms[c] = *chosen;
            continue;
        }
        std::optional<tile_algorithm> chosen = usable(directive("FZALG" + std::to_string(c + 1)));
        if (!chosen) {
            chosen = usable(directive("FZALGOR"));
        }
        if (!chosen && options.type != compression::automatic && options.type != compression::none) {
            chosen = usable(resolve_algorithm(options, bitpix::uint8));
        }
        algorithms[c] = chosen.value_or(default_algorithm(type));
    }

    // The rows of each tile: the options, the directive FZTILELN, or about 10 MB of rows.
    std::uint64_t tile_rows = options.tile_rows;
    if (tile_rows == 0) {
        try {
            tile_rows = static_cast<std::uint64_t>(
                std::max<std::int64_t>(caller.find<std::int64_t>("FZTILELN").value_or(0), 0));
        } catch (const invalid_data_error&) {
            tile_rows = 0;
        }
    }
    if (tile_rows == 0) {
        tile_rows = default_tile_bytes / std::max<std::uint64_t>(info.row_size, 1);
    }
    tile_rows = std::clamp<std::uint64_t>(tile_rows, 1, std::max<std::uint64_t>(info.rows, 1));
    const std::uint64_t tiles = info.rows == 0 ? 0 : ((info.rows - 1) / tile_rows) + 1;

    const std::span<const std::byte> rows = data.first(info.rows * info.row_size);
    const std::span<const std::byte> heap = data.subspan(info.rows * info.row_size);
    gzip_encoder gzip(options.gzip_level.value_or(-1));
    gzip_encoder descriptors(-1);
    std::vector<std::byte> out_heap;
    std::vector<std::byte> cells(checked_cast<std::size_t>(tiles * fields * 16));
    std::vector<std::uint64_t> largest(fields, 0);
    std::vector<std::byte> chunk;
    std::vector<std::byte> packed;
    std::vector<std::byte> block;
    for (std::uint64_t tile = 0; tile < tiles; ++tile) {
        const std::uint64_t first = tile * tile_rows;
        const std::uint64_t count = std::min(tile_rows, info.rows - first);
        for (std::size_t c = 0; c < fields; ++c) {
            const column_model& column = model.columns[c];
            const std::uint64_t field = column.format.field_size();
            const std::uint64_t offset = column.info.offset;
            const bool variable = column.format.storage != column_storage::fixed;
            std::vector<std::byte> cell;
            if (field == 0) {
                // A column of no element holds nothing.
            } else if (!variable) {
                chunk.resize(checked_cast<std::size_t>(count * field));
                for (std::uint64_t r = 0; r < count; ++r) {
                    const auto from = rows.subspan(((first + r) * info.row_size) + offset, field);
                    std::ranges::copy(from, chunk.begin() + static_cast<std::ptrdiff_t>(r * field));
                }
                compress_bytes(chunk, algorithms[c], column.format.type, false, gzip, cell);
            } else {
                // Each array compressed alone in the heap, unless that does not make it smaller; then the descriptors
                // of the table and those of the compressed arrays, compressed with GZIP_1 (standard §10.3.6).
                const std::size_t own = descriptor_size(column.format.storage);
                block.assign(checked_cast<std::size_t>(count * (own + 16)), std::byte{0});
                for (std::uint64_t r = 0; r < count; ++r) {
                    const auto original = rows.subspan(((first + r) * info.row_size) + offset, own);
                    std::ranges::copy(original, block.begin() + static_cast<std::ptrdiff_t>(r * own));
                    const descriptor array = read_descriptor(original, column.format.storage);
                    const std::uint64_t bytes = array_bytes(column.format.type, array.count);
                    if (bytes == 0) {
                        continue;
                    }
                    const std::span<const std::byte> values = within(heap, array.offset, bytes, hdu);
                    compress_bytes(values, algorithms[c], column.format.type, true, gzip, packed);
                    const std::span<const std::byte> stored =
                        packed.size() < values.size() ? std::span<const std::byte>(packed) : values;
                    const auto at = std::span(block).subspan((count * own) + (r * 16), 16);
                    store_big_endian(at.first<8>(), stored.size());
                    store_big_endian(at.subspan<8, 8>(), out_heap.size());
                    out_heap.insert(out_heap.end(), stored.begin(), stored.end());
                }
                descriptors.compress(block, cell);
            }
            const auto descriptor_field = std::span(cells).subspan(((tile * fields) + c) * 16, 16);
            store_big_endian(descriptor_field.first<8>(), cell.size());
            store_big_endian(descriptor_field.subspan<8, 8>(), out_heap.size());
            largest[c] = std::max<std::uint64_t>(largest[c], cell.size());
            out_heap.insert(out_heap.end(), cell.begin(), cell.end());
        }
        if (advance) {
            advance(count * info.row_size);
        }
    }

    compressed_unit unit;
    unit.data.reserve(cells.size() + out_heap.size());
    unit.data.insert(unit.data.end(), cells.begin(), cells.end());
    unit.data.insert(unit.data.end(), out_heap.begin(), out_heap.end());

    // The cards of the table of compressed columns, then those of the table it holds under their Z names.
    std::vector<card>& cards = unit.cards;
    for (const card& entry : table_cards(model)) {
        const std::string_view name = entry.keyword();
        if (name == "NAXIS1") {
            cards.emplace_back("NAXIS1", fields * 16, entry.comment());
        } else if (name == "NAXIS2") {
            cards.emplace_back("NAXIS2", tiles, entry.comment());
        } else if (name == "PCOUNT") {
            cards.emplace_back("PCOUNT", out_heap.size(), entry.comment());
        } else if (is_indexed(name, "TFORM")) {
            const std::size_t c = std::stoul(std::string(name.substr(5))) - 1;
            column_format format;
            format.type = column_type::uint8;
            format.storage = column_storage::variable_q;
            format.max_elements = largest[c];
            cards.emplace_back(name, format.to_string(), "compressed data of the column");
        } else if (name == "THEAP") {
            continue;
        } else {
            cards.push_back(entry);
        }
    }
    cards.emplace_back("ZTABLE", true, "extension contains a compressed table");
    cards.emplace_back("ZTILELEN", tile_rows, "number of rows in each tile");
    cards.emplace_back("ZNAXIS1", info.row_size, "width of the rows of the table in bytes");
    cards.emplace_back("ZNAXIS2", info.rows, "number of rows in the table");
    cards.emplace_back("ZPCOUNT", model.heap_size, "size of the heap of the table");
    for (std::size_t c = 0; c < fields; ++c) {
        const std::string n = std::to_string(c + 1);
        cards.emplace_back("ZFORM" + n, model.columns[c].format.to_string(), "format of column " + n);
        cards.emplace_back("ZCTYP" + n, algorithm_name(algorithms[c]), "compression algorithm of column " + n);
    }
    return unit;
}

} // namespace openfits::detail
