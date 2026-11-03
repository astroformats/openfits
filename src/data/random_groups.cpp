// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "data/random_groups.h"

#include <openfits/error.h>
#include <openfits/table.h>

#include "core/ascii.h"
#include "core/endian.h"
#include "data/conversion.h"
#include "data/image_read.h"
#include "table/column_read.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <string>
#include <vector>

namespace openfits::detail {

namespace {

constexpr std::uint64_t max_uint64 = std::numeric_limits<std::uint64_t>::max();

[[noreturn]] void refuse(const std::string& message, std::uint64_t hdu)
{
    throw usage_error(errc::invalid_argument, message, {.hdu = hdu});
}

// The type of the fields that hold the values of a BITPIX (standard Table 18).
column_type field_type(bitpix stored) noexcept
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

// The bytes of a group: PCOUNT parameters and an array (standard §6.2, Eq. 4 and 6).
std::uint64_t group_bytes(const groups_info& groups)
{
    // The walk sized the data from the same keywords in 64 bits, so that none of this overflows.
    std::uint64_t values = groups.parameters.size();
    std::uint64_t elements = 1;
    for (const std::uint64_t axis : groups.geometry.axes) {
        elements *= axis;
    }
    values += elements;
    return values * stored_size(groups.bitpix);
}

bool same_name(std::string_view left, std::string_view right) noexcept
{
    left = trim_trailing_spaces(left);
    right = trim_trailing_spaces(right);
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(),
                                                     [](char a, char b) { return to_upper(a) == to_upper(b); });
}

// The groups as the rows of a table whose columns are the chosen parameters, each a field at its place in the group.
table_info groups_table(const groups_info& groups, const std::vector<std::size_t>& chosen)
{
    table_info table;
    table.rows = groups.count;
    table.row_size = group_bytes(groups);
    const std::uint64_t size = stored_size(groups.bitpix);
    table.columns.reserve(chosen.size());
    for (const std::size_t p : chosen) {
        column_info column;
        column.name = groups.parameters[p].name;
        column.format = column_format{.type = field_type(groups.bitpix)};
        column.scaling = groups.parameters[p].scaling;
        column.offset = p * size;
        table.columns.push_back(std::move(column));
    }
    return table;
}

} // namespace

std::uint64_t selected_groups(const groups_info& groups, const parameter_read_options& options, std::uint64_t hdu)
{
    return select_rows(options.groups, groups.count, hdu).count;
}

void read_parameters(const thread_safe_source& source, const hdu_info& unit, const groups_info& groups,
                     std::size_t parameter, std::string_view name, numeric_type type, std::span<std::byte> destination,
                     const parameter_read_options& options, std::size_t transfer)
{
    const std::uint64_t hdu = unit.index();
    std::vector<std::size_t> chosen;
    if (name.empty()) {
        if (parameter >= groups.parameters.size()) {
            refuse("the groups of unit " + std::to_string(hdu) + " have " + std::to_string(groups.parameters.size()) +
                       " parameters, and none at index " + std::to_string(parameter),
                   hdu);
        }
        chosen.push_back(parameter);
    } else {
        for (std::size_t p = 0; p < groups.parameters.size(); ++p) {
            if (same_name(groups.parameters[p].name, name)) {
                chosen.push_back(p);
            }
        }
        if (chosen.empty()) {
            refuse("the groups of unit " + std::to_string(hdu) + " have no parameter named '" + std::string(name) + "'",
                   hdu);
        }
    }
    if (options.scaling == scaling_policy::apply) {
        for (const std::size_t p : chosen) {
            if (groups.parameters[p].scaling.scale == 0.0) {
                throw invalid_data_error(errc::reserved_keyword,
                                         "PSCAL" + std::to_string(p + 1) +
                                             " is 0, which leaves the physical values of the parameter unavailable",
                                         {.hdu = hdu, .keyword = "PSCAL" + std::to_string(p + 1)});
            }
        }
    }
    const row_selection selected = select_rows(options.groups, groups.count, hdu);
    if (destination.size() != selected.count * size_of(type)) {
        refuse("the destination of the parameter does not have a value for each group", hdu);
    }
    const table_info table = groups_table(groups, chosen);
    column_read_options read{.rows = options.groups,
                             .scaling = options.scaling,
                             .override_scaling = options.override_scaling,
                             .overflow = options.overflow,
                             .rounding = options.rounding};
    if (chosen.size() == 1) {
        const column_destination target{.column = 0, .type = static_cast<element_type>(type), .values = destination};
        (void)read_columns(source, unit, table, std::span<const column_destination>(&target, 1), read);
        return;
    }
    // The physical values of the parameters of one name, summed in double (standard §6.1.2, §6.3), then converted: a
    // block of groups at a time, whose values of every parameter fit in transfer bytes.
    const read_conversion conversion = make_read_conversion(bitpix::float64, type, {}, std::nullopt, blank_policy::nan,
                                                            0.0, options.overflow, options.rounding, hdu);
    const std::uint64_t block = std::min<std::uint64_t>(
        std::max<std::uint64_t>(transfer / (chosen.size() * sizeof(double)), 1), selected.count);
    std::vector<std::vector<double>> values(chosen.size(), std::vector<double>(block));
    std::vector<std::byte> sums(block * sizeof(double));
    const std::size_t value_size = size_of(type);
    for (std::uint64_t done = 0; done < selected.count;) {
        const std::size_t count = std::min(block, selected.count - done);
        read.rows = row_range{.first = selected.first + done, .count = count};
        std::vector<column_destination> targets;
        targets.reserve(chosen.size());
        for (std::size_t k = 0; k < chosen.size(); ++k) {
            targets.push_back(column_destination::of(k, std::span<double>(values[k]).first(count)));
        }
        (void)read_columns(source, unit, table, targets, read);
        for (std::size_t g = 0; g < count; ++g) {
            double sum = 0.0;
            for (const std::vector<double>& parameter_values : values) {
                sum += parameter_values[g];
            }
            store_big_endian<std::uint64_t>(std::span<std::byte, 8>(sums.data() + (g * 8), 8),
                                            std::bit_cast<std::uint64_t>(sum));
        }
        (void)convert_read(conversion, sums.data(), 1, destination.data() + (done * value_size), nullptr, count);
        done += count;
    }
}

image_info group_image(const groups_info& groups)
{
    image_info image;
    image.bitpix = groups.bitpix;
    image.geometry = groups.geometry;
    if (image.geometry.axes.empty()) {
        // NAXIS = 1: one element in each group, the empty product of Eq. 6, read as an axis of one.
        image.geometry.axes = {1};
    }
    image.scaling = groups.scaling;
    image.blank = groups.blank;
    return image;
}

pixel_read_result read_group(const thread_safe_source& source, const hdu_info& unit, const groups_info& groups,
                             std::uint64_t group, numeric_type type, std::span<std::byte> destination,
                             const pixel_read_options& options)
{
    if (group >= groups.count) {
        refuse("unit " + std::to_string(unit.index()) + " has " + std::to_string(groups.count) +
                   " groups, and none at index " + std::to_string(group),
               unit.index());
    }
    const std::uint64_t size = group_bytes(groups);
    if (group > max_uint64 / std::max<std::uint64_t>(size, 1)) {
        refuse("the group lies beyond 64 bits", unit.index());
    }
    const std::uint64_t offset = (group * size) + (groups.parameters.size() * stored_size(groups.bitpix));
    return read_pixels(source, unit, group_image(groups), type, destination, options, pixel_transfer, offset);
}

} // namespace openfits::detail
