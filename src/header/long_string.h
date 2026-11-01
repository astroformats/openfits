// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/card.h>

#include "header/record.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

// Long strings continued over CONTINUE records (standard §4.2.1.2): joined when a header is read, and split into
// records when a card is written.

namespace openfits::detail {

/// True when value is a conforming CONTINUE record, which continues a string: the keyword CONTINUE, spaces in bytes 9
/// and 10, and a string, which the record reader reads into a card without a value indicator.
[[nodiscard]] bool is_continuation(const card& value) noexcept;

/// The number of CONTINUE records after the card at index that continue its string: as long as the string so far ends
/// with an ampersand and the next record is a conforming CONTINUE record.
[[nodiscard]] std::size_t continuation_length(std::span<const card> cards, std::size_t index) noexcept;

/// The string of the card at index, which holds a string, with those of the CONTINUE records that continue it
/// appended, each ampersand that continues it removed.
[[nodiscard]] std::string joined_string(std::span<const card> cards, std::size_t index);

/// The comment of the card at index, with those of the CONTINUE records that continue its string appended: after a
/// space where the record before ends before byte 80, and without one where it fills the record, as a comment split
/// inside a word does.
[[nodiscard]] std::string joined_comment(std::span<const card> cards, std::size_t index);

/// The records of a card, 80 characters each: the record it was read from; for a string longer than a record holds, a
/// record of the keyword and CONTINUE records, the comment on the last one where it fits and on CONTINUE records of
/// null strings after it otherwise, which hold a comment of any length; and otherwise the record that write_record()
/// makes.
/// @throws validation_error as write_record() does, and with errc::continued_reserved_keyword for a long string of a
///         mandatory or reserved keyword, which CONTINUE must not continue (standard §4.2.1.2).
[[nodiscard]] std::vector<std::string> card_records(const card& value, const record_options& options);

} // namespace openfits::detail
