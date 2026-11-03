// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/card.h>
#include <openfits/header.h>

#include "header/record.h"

#include <cstddef>
#include <span>
#include <string>

// Headers written into blocks (standard §3.1, §4.1): the records of the structural keywords, which the writer makes
// from the model of a unit, then those of the caller's cards.

namespace openfits::detail {

/// How header_blocks() writes a header.
struct emission_options
{
    /// The forms of records outside the standard's own: the HIERARCH convention, and comments cut.
    record_options records{};
    /// LONGSTRN = 'OGIP 1.0' before the caller's cards when one of them is a long string and none is LONGSTRN: the
    /// convention of OGIP that announces CONTINUE records to readers older than FITS 4.0.
    bool longstrn = false;
    /// Blank records before END: the fill space that later changes of the header take (standard §4.4.2.4).
    std::size_t reserved_cards = 0;
    /// Cards of the writer after those of the caller: those of the checksums, which come last (standard §4.4.2.7).
    std::span<const card> closing{};
};

/// The blocks of a header: the records of the structural cards; LONGSTRN when options ask for it; the records of the
/// caller's cards, its fill space left out, as card_records() makes them; those of options.closing;
/// options.reserved_cards blank records; END; and spaces to the end of the last block (standard §3.1). A card read
/// from a file is its own record.
/// @throws validation_error with errc::structural_keyword for a structural keyword among the caller's cards, which the
///         writer makes from the model, and the errors of card_records().
[[nodiscard]] std::string header_blocks(std::span<const card> structural, const header& caller,
                                        const emission_options& options);

} // namespace openfits::detail
