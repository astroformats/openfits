// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/card.h>
#include <openfits/hdu.h>
#include <openfits/header.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/writer.h>

#include "header/record.h"
#include "io/thread_safe_source.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// The units that the writer copies from a reader (writer::add_hdu()): their headers, as the source holds them or
// written anew, converted to their place in the file, and their data records as they are.

namespace openfits::detail {

/// Access to what a reader reads from. Defined beside the reader, in the library alone, which the internal sources
/// that tests and fuzz targets compile for themselves must not call: the library takes the source and gives it to them.
struct reader_access
{
    [[nodiscard]] static const thread_safe_source& source(const reader& file) noexcept;
};

/// How a copied unit takes its place in the file of the writer.
enum class copy_form : std::uint8_t
{
    as_is,        ///< The unit as it is.
    to_extension, ///< A primary HDU of an image after the first unit: an IMAGE extension.
    to_primary,   ///< An IMAGE extension first: the primary HDU.
};

/// A unit that the writer copies, as add_hdu() plans it.
struct unit_copy
{
    const thread_safe_source* source = nullptr;
    const hdu_info* unit = nullptr;
    copy_form form = copy_form::as_is;
    /// The cards of the copy but its structural ones, which replace those of the unit.
    std::optional<header> cards{};
};

/// True for a unit that cannot be the first of a file, nor become its primary HDU: an extension that holds no image.
[[nodiscard]] bool needs_primary_before(const hdu_info& unit) noexcept;

/// The form that a copy of unit takes at position in a file: a primary HDU of an image after the first unit becomes an
/// IMAGE extension, and an IMAGE extension first the primary HDU.
/// @throws usage_error (errc::invalid_argument) for random groups after the first unit, which only a primary HDU holds
///         (standard §6).
[[nodiscard]] copy_form copy_form_at(const hdu_info& unit, std::size_t position);

/// The copy of the unit at index of source, which reads bytes, placed at position in the file of the writer.
/// @throws the errors of writer::add_hdu().
[[nodiscard]] unit_copy plan_copy(const reader& source, const thread_safe_source& bytes, std::size_t index,
                                  std::size_t position, hdu_copy_options options);

/// True when the header of a copy is written as the source holds it, byte for byte: neither converted nor edited, nor
/// given checksums, every record of printable ASCII or the bytes outside it kept, and whole blocks ending with END.
[[nodiscard]] bool header_as_is(const unit_copy& copy, const record_options& records, bool checksums);

/// The blocks of the header of a copy, as the source holds them.
/// @throws the exceptions of the source.
[[nodiscard]] std::string source_header(const unit_copy& copy);

/// The cards of a copy written anew that the writer writes first, in the place the copy takes: the structural cards of
/// the unit when the copy gives the others, every card of the unit otherwise, in their order, without the fill space
/// that ends them, and without CHECKSUM and DATASUM, which the writer computes again; SIMPLE and EXTEND made XTENSION,
/// PCOUNT and GCOUNT for an extension, and the inverse for a primary HDU, as CFITSIO converts a unit that it copies.
[[nodiscard]] std::vector<card> copy_records(const unit_copy& copy);

/// The cards that a copy written anew gives after them, none when it gives none.
[[nodiscard]] header copy_cards(const unit_copy& copy);

/// The blank cards before END of a copy written anew: the fill space of the cards that it gives, or of the unit.
[[nodiscard]] std::size_t copy_fill(const unit_copy& copy) noexcept;

/// True when the unit of a copy has CHECKSUM or DATASUM, which a copy written anew computes again.
[[nodiscard]] bool has_checksum_cards(const unit_copy& copy);

/// The bytes of the data records of a copy, the fill of their last block included.
[[nodiscard]] std::uint64_t copy_data_size(const unit_copy& copy) noexcept;

/// Writes the data records of a copy into sink: the data and the fill of their last block as the source holds them,
/// read in pieces of a few MiB, and the fill that a short file lacks as zeros, or spaces for an ASCII table. advance
/// receives the bytes of each write.
/// @throws io_error (errc::end_of_data) when the file ends before the data, and the exceptions of the source, the sink
///         and advance.
void write_copy_data(output_sink& sink, const unit_copy& copy, const std::function<void(std::uint64_t)>& advance);

} // namespace openfits::detail
