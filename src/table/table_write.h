// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/card.h>
#include <openfits/io.h>
#include <openfits/table.h>
#include <openfits/types.h>
#include <openfits/writer.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

// The writes of tables (standard §7.2, §7.3): the model of a table as the writer keeps it, the structural keywords of
// its header, and its rows and heap, filled to the end of their last block.

namespace openfits::detail {

/// What the values of a column are, as column_values holds them.
enum class values_kind : std::uint8_t
{
    numbers,
    complex_float32,
    complex_float64,
    strings,
    logicals,
    bits,
};

/// The values of a column, as the writer reads them.
struct values_view
{
    values_kind kind = values_kind::numbers;
    numeric_type type = numeric_type::uint8;
    std::span<const std::byte> bytes{};
    std::span<const std::string> strings{};
    std::span<const std::uint8_t> undefined{};
    std::span<const std::uint64_t> offsets{};
    bool variable = false;
    std::size_t count = 0;
};

struct column_values_access
{
    [[nodiscard]] static values_view view(const column_values& values) noexcept;
};

/// A column of a table as the writer keeps it: its model with a format, the values borrowed, and for variable-length
/// arrays the descriptor of each row in the heap.
struct column_model
{
    column_info info{};
    values_view values{};
    /// The format of the column: that of info, or the one derived from the values, as resolve_table() leaves it.
    column_format format{};
    /// The elements of each field, or of each substring of characters.
    std::uint64_t elements = 0;
    /// For characters: the width of each string and their number in a field.
    std::uint64_t string_width = 0;
    std::uint64_t strings = 0;
    /// For variable-length arrays: the elements and the heap offset of the array of each row.
    std::vector<std::uint64_t> counts{};
    std::vector<std::uint64_t> heap_offsets{};
    /// True when the format was derived from the values, which may then take Q descriptors.
    bool derived = false;
};

/// The model of a table as the writer keeps it, every size computed.
struct table_model
{
    table_info info{};
    std::vector<column_model> columns{};
    std::uint64_t heap_size = 0;
    table_write_options options{};
};

/// The most bytes of rows that a write converts at once.
inline constexpr std::size_t table_transfer = std::size_t{4} << 20;

/// The model of the table of unit hdu with values for its columns.
/// @throws usage_error and validation_error as writer::add_table() does.
[[nodiscard]] table_model resolve_table(table_info info, std::span<const column_values> values,
                                        const table_write_options& options, std::uint64_t hdu);

/// The structural cards of a table: its mandatory keywords, and those of each column.
[[nodiscard]] std::vector<card> table_cards(const table_model& table);

/// The bytes of the data of a table, rows and heap, with the fill of their last block.
[[nodiscard]] std::uint64_t padded_table_size(const table_model& table) noexcept;

/// Writes the rows of a table and its heap into sink, then the fill of the last block, spaces for an ASCII table and
/// zeros for a binary one (standard §7.2.3, §7.3.3); advance receives the bytes of each write.
/// @throws invalid_data_error for a value beyond its stored type or its field, and the exceptions of the sink and of
///         advance.
void write_table_data(output_sink& sink, const table_model& table, const std::function<void(std::uint64_t)>& advance,
                      std::uint64_t hdu, std::size_t transfer = table_transfer);

} // namespace openfits::detail
