// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/header.h>
#include <openfits/types.h>

#include "core/diagnostic_log.h"

#include <cstdint>
#include <optional>
#include <string>

// The structure of a unit from the mandatory keywords of its header (standard §4.4.1), and the form of its reserved
// keywords (standard §4.4.2, §8, §9).

namespace openfits::detail {

/// What the header of a unit says of it.
struct unit_structure
{
    hdu_kind kind = hdu_kind::unknown;
    /// The bytes of the data without their fill (standard §4.4.1, Eq. 1 and 2; §6.1.1, Eq. 4); nothing when the header
    /// gives no size, which an error in the log reports.
    std::optional<std::uint64_t> data_size{};
    /// EXTNAME, or else HDUNAME, without trailing spaces.
    std::string name{};
    /// EXTVER, 1 without it.
    std::int64_t version = 1;
};

/// The structure of the header of the unit index, the primary HDU for 0, whose first record lies at header_offset.
/// Each departure goes to log, with the unit, the keyword and the offset of its record in its context.
/// @throws the exceptions of log in strict mode.
[[nodiscard]] unit_structure analyze_header(const header& cards, std::uint64_t index, std::uint64_t header_offset,
                                            diagnostic_log& log);

} // namespace openfits::detail
