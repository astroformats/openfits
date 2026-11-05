// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "edit/rows.h"

#include <openfits/error.h>
#include <openfits/io.h>

#include "core/checked_math.h"
#include "table/table_write.h"

#include <string>

namespace openfits::detail {

namespace {

// The model of the rows of a table as the writer writes them: its columns where they lie in its rows.
table_model model_of(const table_info& table, scaling_policy scaling, overflow_policy overflow)
{
    table_model model;
    model.info.kind = table.kind;
    model.info.row_size = table.row_size;
    model.options.scaling = scaling;
    model.options.overflow = overflow;
    model.columns.reserve(table.columns.size());
    for (const column_info& column : table.columns) {
        column_model entry;
        entry.info = column;
        // A reader gives every column its format.
        entry.format = column.format.value_or(column_format{});
        model.columns.push_back(std::move(entry));
    }
    return model;
}

// The rows and the heap of a part of a table that the writer writes, without their fill.
std::vector<std::byte> rows_and_heap(const table_model& part, std::uint64_t hdu)
{
    memory_sink sink;
    write_table_data(sink, part, [](std::uint64_t) {}, hdu);
    std::vector<std::byte> bytes = sink.release();
    bytes.resize(checked_cast<std::size_t>(checked_add(part.info.heap_offset, part.heap_size)));
    return bytes;
}

} // namespace

data_patch cell_patch(const table_info& table, std::size_t column, std::uint64_t first, const column_values& values,
                      scaling_policy scaling, overflow_policy overflow, std::uint64_t hdu)
{
    if (column >= table.columns.size()) {
        throw usage_error(errc::column_not_found,
                          "the table of unit " + std::to_string(hdu) + " has " + std::to_string(table.columns.size()) +
                              " columns, and none at index " + std::to_string(column),
                          {.hdu = hdu});
    }
    const column_info& info = table.columns[column];
    const column_format format = info.format.value_or(column_format{});
    if (format.storage != column_storage::fixed) {
        throw usage_error(errc::not_editable,
                          "column " + std::to_string(column + 1) + " of unit " + std::to_string(hdu) +
                              " holds variable-length arrays, whose sizes change the heap: a rewrite of the table "
                              "(editor::replace_hdu()) changes them",
                          {.hdu = hdu, .column = column + 1});
    }
    const std::uint64_t field = format.field_size();
    if (field == 0) {
        return {};
    }
    // The column alone, its fields one after the other.
    table_info alone;
    alone.kind = table.kind;
    alone.row_size = field;
    alone.columns.push_back(info);
    alone.columns.back().offset = 0;
    const table_model part = resolve_rows(model_of(alone, scaling, overflow), std::span(&values, 1), 0, hdu);
    const std::uint64_t rows = part.info.rows;
    if (first > table.rows || rows > table.rows - first) {
        throw usage_error(errc::invalid_argument,
                          "the " + std::to_string(rows) + " rows from row " + std::to_string(first) +
                              " lie beyond the " + std::to_string(table.rows) + " rows of the table of unit " +
                              std::to_string(hdu),
                          {.hdu = hdu, .column = column + 1});
    }
    data_patch patch;
    if (rows == 0) {
        return patch;
    }
    patch.bytes = rows_and_heap(part, hdu);
    patch.runs.push_back({.offset = checked_add(checked_multiply(first, table.row_size), info.offset),
                          .count = rows,
                          .width = field,
                          .stride = table.row_size});
    return patch;
}

appended_rows append_rows(const table_info& table, std::span<const column_values> values, std::uint64_t heap,
                          scaling_policy scaling, overflow_policy overflow, std::uint64_t hdu)
{
    const table_model part = resolve_rows(model_of(table, scaling, overflow), values, heap, hdu);
    appended_rows result;
    result.count = part.info.rows;
    std::vector<std::byte> bytes = rows_and_heap(part, hdu);
    const auto rows = static_cast<std::ptrdiff_t>(part.info.heap_offset);
    result.heap.assign(bytes.begin() + rows, bytes.end());
    bytes.resize(static_cast<std::size_t>(rows));
    result.rows = std::move(bytes);
    result.emax.reserve(part.columns.size());
    for (const column_model& model : part.columns) {
        result.emax.push_back(model.format.storage == column_storage::fixed ? 0
                                                                            : model.format.max_elements.value_or(0));
    }
    return result;
}

} // namespace openfits::detail
