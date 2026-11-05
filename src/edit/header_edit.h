// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/card.h>
#include <openfits/header.h>

#include "header/record.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

// The headers that the editor writes: a header changed by the application, checked against the one it was read as,
// written into blocks with the records of its cards kept, and completed with its checksums.

namespace openfits::detail {

/// Throws validation_error (errc::structural_keyword), with the unit hdu and the keyword in its context, when edited,
/// a header that the application changed from original, changes its structure: the structural keywords that the
/// library writes from the model of a unit (is_structural_keyword()), but CHECKSUM and DATASUM, added, removed, given
/// other values or another order, or another card put before or among those that start the header (standard §4.4.1).
void check_structure(const header& original, const header& edited, std::uint64_t hdu);

/// The blocks of a header as the editor writes it (standard §3.1, §4.1): the record of each card, that which it was
/// read from as it is, bytes outside printable ASCII included, or for a card that a change made the records that the
/// writer makes of it under options, a long string continued over CONTINUE records; LONGSTRN = 'OGIP 1.0' before the
/// first card that a change made into a long string when longstrn asks for it and the header has none; END; and spaces
/// to the end of the last block, of which there are at least minimum_blocks: a header that shrinks keeps its blocks,
/// the records freed as blank cards before END, which stays in the last block, the fill space that standard §4.4.2.4
/// lets later keywords take.
/// @throws validation_error for a card that no record can hold, as the writer throws it.
[[nodiscard]] std::string edited_blocks(const header& cards, const record_options& options, bool longstrn,
                                        std::uint64_t minimum_blocks);

/// The header with its first DATASUM card made to record data_sum, when datasum, and its first CHECKSUM card the
/// placeholder of 16 zeros of standard Appendix J.1, when checksum, each in the fixed format and with the comment that
/// it had: the records that seal_checksum() completes.
[[nodiscard]] header with_checksum_cards(header cards, std::optional<std::uint32_t> datasum, bool checksum);

/// Completes the CHECKSUM record of blocks, the first record of that keyword, which holds the placeholder of 16 zeros:
/// its value is made such that the blocks and data records whose sum is data_sum sum to total (standard Appendix J.2),
/// which is negative zero, 0xFFFFFFFF, for a unit that verifies, or what a changed unit summed to before the change, so
/// that a unit that did not verify still does not (Appendix J.4).
/// @throws usage_error (errc::invalid_argument) when the blocks hold no record of CHECKSUM with the placeholder.
void seal_checksum(std::string& blocks, std::uint32_t data_sum, std::uint32_t total);

/// The header of the table of a compressed image or table (standard §10.1, §10.3) whose header as the reader presents
/// it, restored, was changed into edited: the cards of table that hold the table and its compression as they are, in
/// their order, with the cards of edited that are not structural in the place of those of restored, its fill space
/// after them; the CHECKSUM and DATASUM of edited become ZHECKSUM and ZDATASUM.
[[nodiscard]] header compressed_header(const header& table, const header& restored, const header& edited);

} // namespace openfits::detail
