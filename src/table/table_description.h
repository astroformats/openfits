// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/hdu.h>
#include <openfits/header.h>
#include <openfits/image.h>
#include <openfits/limits.h>
#include <openfits/table.h>
#include <openfits/types.h>

#include "core/diagnostic_log.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

// The tables of units from their headers (standard §7.2, §7.3), and random groups (standard §6): the walk checks what
// the keywords of each say and records the problems, and the reads describe the table or the groups from the same
// checks.

namespace openfits::detail {

/// The table that the header of the unit index describes, an ASCII table for hdu_kind::table and a binary table for
/// the other kinds of tables, whose first record lies at header_offset. Each departure goes to log, with the unit, the
/// keyword and the offset of its record in its context; the mandatory keywords of every extension, which the walk
/// checks first, are only read. Nothing when an error leaves the columns unknown.
/// @throws the exceptions of log in strict mode.
[[nodiscard]] std::optional<table_info> analyze_table(const header& cards, hdu_kind kind, std::uint64_t index,
                                                      std::uint64_t header_offset, diagnostic_log& log);

/// The random groups that the primary header describes (standard §6), whose departures go to log as analyze_table()
/// sends them; nothing when an error leaves them unknown.
[[nodiscard]] std::optional<groups_info> analyze_groups(const header& cards, std::uint64_t header_offset,
                                                        diagnostic_log& log);

/// The table of a unit, for the reads: analyze_table() of its header, with the header in the result. A compressed
/// image or table is the binary table that it is.
/// @throws usage_error (errc::not_a_table) for a unit that holds no table, and invalid_data_error with the first error
///         of the analysis, or of the walk when the unit gives no size.
[[nodiscard]] table_info describe_table(const hdu_info& unit, bool with_header = true);

/// The random groups of a unit, for the reads, as describe_table() describes a table, their parameters within
/// limits.max_allocation: PCOUNT, which nothing in an empty or truncated unit bounds, gives their number.
/// @throws usage_error (errc::not_random_groups) for a unit that holds none, limit_error (errc::allocation_too_large)
///         when the parameters take more than limits.max_allocation, and invalid_data_error as describe_table().
[[nodiscard]] groups_info describe_groups(const hdu_info& unit, const limits& limits, bool with_header = true);

/// The dimensions of a TDIMn value, (l,m,...), spaces allowed around the numbers, each from 1; nothing for another
/// text.
[[nodiscard]] std::optional<std::vector<std::uint64_t>> parse_dimensions(std::string_view text);

/// The TDIMn value of dimensions: (l,m,...).
[[nodiscard]] std::string format_dimensions(const std::vector<std::uint64_t>& dimensions);

/// True when a display format may show the values of a column of the format (standard §7.3.4, Tables 16 and 20): A
/// and G characters; L and G logicals; every code but A and L the numbers of integer columns, bytes of bits among them;
/// the codes of reals, F, E, EN, ES, G and D, real and complex values.
[[nodiscard]] bool displays(const display_format& display, const column_format& format) noexcept;

} // namespace openfits::detail
