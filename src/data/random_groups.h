// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/hdu.h>
#include <openfits/image.h>
#include <openfits/reader.h>
#include <openfits/types.h>

#include "io/thread_safe_source.h"
#include "table/column_read.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// The reads of random groups (standard §6): the parameters of the groups, which lie at the same place in each group as
// the fields of a column lie in each row, and the array of a group, read as an image.

namespace openfits::detail {

/// The groups that options select, every group when they give none.
/// @throws usage_error (errc::invalid_argument) when they lie beyond the groups.
[[nodiscard]] std::uint64_t selected_groups(const groups_info& groups, const parameter_read_options& options,
                                            std::uint64_t hdu);

/// Reads the parameter at index parameter of the selected groups of unit, or the sum of the physical values of the
/// parameters named name when name is not empty, into destination as values of type: reader::read_parameter(). A sum
/// reads blocks of groups whose values of every parameter fit in transfer bytes.
void read_parameters(const thread_safe_source& source, const hdu_info& unit, const groups_info& groups,
                     std::size_t parameter, std::string_view name, numeric_type type, std::span<std::byte> destination,
                     const parameter_read_options& options, std::size_t transfer = column_transfer);

/// The image of the array of each group.
[[nodiscard]] image_info group_image(const groups_info& groups);

/// Reads the array of the group at index group: reader::read_group().
pixel_read_result read_group(const thread_safe_source& source, const hdu_info& unit, const groups_info& groups,
                             std::uint64_t group, numeric_type type, std::span<std::byte> destination,
                             const pixel_read_options& options);

} // namespace openfits::detail
