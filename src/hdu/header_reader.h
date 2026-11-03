// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/card.h>
#include <openfits/limits.h>

#include "core/diagnostic_log.h"
#include "core/scratch_buffer.h"
#include "io/thread_safe_source.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
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
    /// The checksum of the blocks of a header that holds a CHECKSUM card and ends with END in a whole block (standard
    /// §4.4.2.7): nothing otherwise.
    std::optional<std::uint32_t> sum{};
};

/// The bytes of the last read of the walk, which serve the headers that lie within them. A read takes four blocks, or
/// twice what the read before took when the header lies within its bytes or close after them, up to 256 KiB: a file
/// of many small units is read in a few large reads, and one of large data units in a read of four blocks for each
/// header, which a header longer than them completes after them. The bound keeps the bytes of a reader in memory that
/// the heap reuses, where the larger blocks that it maps afresh for each reader cost their pages again at each open.
class read_window
{
public:
    /// The size bytes of source from offset, which the source holds: from the bytes kept when they hold them, and read
    /// otherwise.
    /// @throws the exceptions of the source.
    [[nodiscard]] std::span<const std::byte> bytes(const thread_safe_source& source, std::uint64_t offset,
                                                   std::uint64_t size);

private:
    // Room for size bytes, the first kept of those held among them.
    void grow(std::size_t size, std::size_t kept);

    // Grown without filling it first, since each read fills what it takes.
    scratch_buffer buffer_{0};
    std::size_t size_ = 0;
    std::uint64_t offset_ = 0;
    // The bytes of the next read, beyond those asked for.
    std::uint64_t ahead_ = 0;
};

/// Reads the header that starts at offset, before the end of the source, and whose first record must have the keyword
/// first_keyword with a value: records of 80 bytes up to END, the record whose first eight bytes are END and spaces
/// (standard §4.4.1), four blocks at a time and then as many as were read so far, through window, or seen in the
/// content of the source in memory. The departures of its records and of its blocks go to log, with the unit hdu and
/// the offset of each record in their context.
/// @throws the exceptions of the source, and of log in strict mode.
[[nodiscard]] unit_header read_header(const thread_safe_source& source, std::uint64_t offset, std::uint64_t hdu,
                                      std::string_view first_keyword, const limits& limits, diagnostic_log& log,
                                      read_window& window);

} // namespace openfits::detail
