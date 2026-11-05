// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/table.h>
#include <openfits/types.h>

#include "edit/patch.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// The writes of the editor into tables (standard §7.2, §7.3): the fields of a column, written in place, and rows
// appended, their fields and arrays as the writer stores them.

namespace openfits::detail {

/// The patch that writes values into the fields of the column at index column of table, the table of unit hdu, from
/// the row first on: as many rows as the values fill fields, each field as the writer stores it under scaling and
/// overflow (writer::add_table()), a string padded with spaces, a number of an ASCII table right-justified in its
/// field.
/// @throws usage_error (errc::column_not_found) for a column that the table does not have, (errc::not_editable) for a
///         column of variable-length arrays, whose arrays change the heap, and (errc::invalid_argument) for rows beyond
///         the table; and the exceptions of writer::add_table() for values that the column cannot hold.
[[nodiscard]] data_patch cell_patch(const table_info& table, std::size_t column, std::uint64_t first,
                                    const column_values& values, scaling_policy scaling, overflow_policy overflow,
                                    std::uint64_t hdu);

/// Rows appended to a table: their bytes, the rows one after the other, and the bytes of their arrays, which follow
/// those of the heap before them, where their descriptors point; and the longest array of each column of
/// variable-length arrays, 0 for the others.
struct appended_rows
{
    std::uint64_t count = 0;
    std::vector<std::byte> rows{};
    std::vector<std::byte> heap{};
    std::vector<std::uint64_t> emax{};
};

/// The rows that values give, a column_values for each column of table, the table of unit hdu, appended after its rows,
/// their arrays after the heap bytes of its heap, as the writer stores them under scaling and overflow.
/// @throws the exceptions of writer::add_table() for values that the columns cannot hold.
[[nodiscard]] appended_rows append_rows(const table_info& table, std::span<const column_values> values,
                                        std::uint64_t heap, scaling_policy scaling, overflow_policy overflow,
                                        std::uint64_t hdu);

} // namespace openfits::detail
