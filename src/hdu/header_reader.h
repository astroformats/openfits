// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/card.h>
#include <openfits/limits.h>

#include "core/diagnostic_log.h"
#include "io/thread_safe_source.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// The headers of a file read from its blocks (standard §3.1, §4.1): records of 80 bytes up to END.

namespace openfits::detail {

/// The size of a block (standard §3.1).
inline constexpr std::uint64_t block_size = 2880;

/// What read_header() finds where a header should start.
enum class header_outcome : std::uint8_t
{
    read,         ///< A header.
    not_a_header, ///< The first record is not the keyword that starts a header there.
    too_large,    ///< A header longer than limits::max_header_bytes, which an error in the log reports.
};

/// A header as the walk reads it from the blocks of its unit.
struct unit_header
{
    header_outcome outcome = header_outcome::read;
    /// The cards before END, in their order. A conforming CONTINUE record that continues no string is a commentary
    /// card (standard §4.2.1.2).
    std::vector<card> cards{};
    /// The bytes of the blocks of the header, the block of END included, or up to the end of the source when it ends
    /// first.
    std::uint64_t size = 0;
};

/// Reads the header that starts at offset, before the end of the source, and whose first record must have the keyword
/// first_keyword with a value: records of 80 bytes up to END, the record whose first eight bytes are END and spaces
/// (standard §4.4.1), read four blocks at a time and then as many as were read so far, or seen in the content of the
/// source in memory. The departures of its records and of its blocks go to log, with the unit hdu and the offset of
/// each record in their context.
/// @throws the exceptions of the source, and of log in strict mode.
[[nodiscard]] unit_header read_header(const thread_safe_source& source, std::uint64_t offset, std::uint64_t hdu,
                                      std::string_view first_keyword, const limits& limits, diagnostic_log& log);

} // namespace openfits::detail
