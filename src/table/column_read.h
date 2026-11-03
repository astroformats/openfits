// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/hdu.h>
#include <openfits/limits.h>
#include <openfits/reader.h>
#include <openfits/table.h>

#include "io/thread_safe_source.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The reads of the values of table columns (standard §7.2, §7.3): rows read in chunks, their fields gathered and
// converted in one pass, and the variable-length arrays of the heap read in ranges of their sorted descriptors.

namespace openfits::detail {

/// The most bytes that a read of columns takes from the source at once, and the chunk of rows that it gathers from.
inline constexpr std::size_t column_transfer = std::size_t{4} << 20;

/// The rows of a read: count rows from the row first.
struct row_selection
{
    std::uint64_t first = 0;
    std::uint64_t count = 0;
};

/// The rows that a row_range selects in a table of table_rows rows, every row when rows is empty.
/// @throws usage_error (errc::invalid_argument) when they lie beyond the table.
[[nodiscard]] row_selection select_rows(const std::optional<row_range>& rows, std::uint64_t table_rows,
                                        std::uint64_t hdu);

/// The elements of a field that a read gives: the repeat count, one for each bit of a bit column, and one for a field
/// of an ASCII table.
[[nodiscard]] std::uint64_t field_elements(const column_info& column) noexcept;

/// Reads the columns of the destinations, from table, the table of unit, with at most transfer bytes taken from the
/// source at once: reader::read_columns().
column_read_result read_columns(const thread_safe_source& source, const hdu_info& unit, const table_info& table,
                                std::span<const column_destination> columns, const column_read_options& options,
                                std::size_t transfer = column_transfer);

/// Reads a column as strings: reader::read_strings(), with the allocations within limits.
[[nodiscard]] std::vector<std::string> read_strings(const thread_safe_source& source, const hdu_info& unit,
                                                    const table_info& table, std::size_t column,
                                                    const column_read_options& options, const limits& limits,
                                                    std::size_t transfer = column_transfer);

/// Reads the arrays of a column: reader::read_arrays(). allocate gives the memory of the values of the elements of
/// every row, once their number is known; offsets and, with blank_policy::mask, mask receive the offsets of the rows
/// and the mask of the values.
void read_arrays(const thread_safe_source& source, const hdu_info& unit, const table_info& table, std::size_t column,
                 element_type type, const column_read_options& options, const limits& limits,
                 const std::function<std::span<std::byte>(std::uint64_t)>& allocate,
                 std::vector<std::uint64_t>& offsets, std::vector<std::uint8_t>& mask,
                 std::size_t transfer = column_transfer);

} // namespace openfits::detail
