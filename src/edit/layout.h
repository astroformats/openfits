// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/io.h>

#include "edit/overlay.h"
#include "io/thread_safe_device.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// The file that a commit makes: its units as pieces, bytes of the file as it is, new bytes, fills and units that write
// themselves; written into a new file, or into the file itself, moving its bytes.

namespace openfits::detail {

/// A piece of the file that a commit makes.
struct layout_piece
{
    enum class kind : std::uint8_t
    {
        file,  ///< size bytes of the file as it is, from source.
        bytes, ///< The bytes given.
        fill,  ///< size bytes of fill.
        unit,  ///< A unit that write writes into a sink, reporting its bytes: size bytes, or as many as it writes.
    };

    kind type = kind::bytes;
    std::uint64_t size = 0;
    std::uint64_t source = 0;
    std::shared_ptr<const std::vector<std::byte>> bytes{};
    /// For bytes that go where the file holds previous, of the same size: only the blocks of 2880 bytes that differ are
    /// written in place.
    std::shared_ptr<const std::vector<std::byte>> previous{};
    std::byte fill{};
    /// For the file and the fill of the data of a unit: the writes of the editor laid over them, the piece being the
    /// data from data_offset on.
    const byte_overlay* overlay = nullptr;
    std::uint64_t data_offset = 0;
    std::function<void(output_sink&, const std::function<void(std::uint64_t)>& advance)> write{};
    /// False for a unit whose size is known once it is written, which only the last piece of a file moved in place,
    /// or any piece of a file written anew, can be.
    bool size_known = true;
};

/// A unit of the file that a commit makes: its header and its data records, in pieces, the header first.
struct layout_unit
{
    std::vector<layout_piece> pieces{};
    /// When set, the header of the unit, the first piece, is completed once its data are written: given the sum of
    /// the data records (standard §4.4.2.7), it gives the header, of the same size.
    std::function<std::vector<std::byte>(std::uint32_t data_sum)> seal{};
};

/// The bytes of the pieces of every unit, those of a unit of unknown size left out.
[[nodiscard]] std::uint64_t layout_size(const std::vector<layout_unit>& units) noexcept;

/// The progress of a commit: the bytes that it writes.
using layout_progress = std::function<void(std::uint64_t bytes)>;

/// Writes the units into sink one after the other, the bytes of the file read from file in pieces of at most transfer
/// bytes with the writes of the editor laid over them, and completes the header of each unit that seals once its data
/// are written, which sink must rewrite. Calls neither finish() nor flush().
/// @throws the exceptions of the file, the sink, the pieces and advance.
void write_layout(output_sink& sink, const std::vector<layout_unit>& units, const thread_safe_device& file,
                  const layout_progress& advance, std::size_t transfer = std::size_t{4} << 20);

/// Makes file hold the units, in place: the pieces of the file that lie elsewhere moved first, in pieces of at most
/// transfer bytes, in an order that reads each byte before a move overwrites it; then every other piece written, a
/// piece of bytes with previous only in its blocks that differ; then each header completed that seals, from a pass over
/// the data of its unit; the file made longer first and shorter last. moved is set once the first move begins, after
/// which a failure leaves the file damaged.
/// @throws usage_error (errc::invalid_argument) for a unit of unknown size before the last piece; and the exceptions of
///         the file, the pieces and advance.
void shift_layout(thread_safe_device& file, const std::vector<layout_unit>& units, const layout_progress& advance,
                  bool& moved, std::size_t transfer = std::size_t{4} << 20);

} // namespace openfits::detail
