// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/error.h>
#include <openfits/hdu.h>
#include <openfits/io.h>
#include <openfits/limits.h>

#include "core/diagnostic_log.h"
#include "hdu/header_reader.h"
#include "hdu/structure.h"
#include "io/thread_safe_source.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The walk of the header and data units of a file (standard §3), on demand.

namespace openfits::detail {

/// Access to the members of hdu_info, which the walk sets.
struct hdu_access
{
    [[nodiscard]] static hdu_info make(std::size_t index, header cards, std::uint64_t header_offset,
                                       std::uint64_t header_size, unit_structure structure,
                                       std::vector<diagnostic> diagnostics);
};

/// The units of a file, walked as they are asked for: the header of each read and parsed, the data of each skipped by
/// their size. The walked units are kept in a deque, whose elements never move, and never change; the walk extends
/// under a lock, so that any number of threads can ask for units at once.
///
/// A unit whose header gives no size stops the walk there: asking for a unit after it throws the error that its
/// diagnostics give. Bytes after the last unit that are not a header, special records (standard §3.5) or else, end
/// the walk with a warning.
class unit_walk
{
public:
    /// Walks the primary header of the source, and every unit in strict mode. start holds the first bytes of the
    /// source when its caller read them, which the walk takes rather than read them again.
    /// @throws usage_error when source is null, invalid_data_error (errc::not_a_fits_file) when it does not start with
    ///         a primary header, limit_error when the primary header is beyond the limits, the first error of a strict
    ///         walk, and the exceptions of the source.
    unit_walk(std::unique_ptr<input_source> source, const limits& limits, bool strict,
              std::span<const std::byte> start = {});

    /// The unit at index, which walks the file as far as it; nullptr when the file has fewer units.
    /// @throws the error that stopped the walk before it, and the exceptions of the source.
    [[nodiscard]] const hdu_info* unit(std::size_t index);

    /// Walks every unit, and returns them.
    /// @throws the exceptions of the source.
    [[nodiscard]] std::vector<std::reference_wrapper<const hdu_info>> units();

    /// The index of the first unit at or after index from for which match is true, walking as far as it; nothing when
    /// none is.
    /// @throws the error that stopped the walk before such a unit, and the exceptions of the source.
    [[nodiscard]] std::optional<std::size_t> find(const std::function<bool(const hdu_info&)>& match,
                                                  std::size_t from = 0);

    /// The diagnostics of the walk that are not of a header.
    [[nodiscard]] std::vector<diagnostic> diagnostics() const;

    [[nodiscard]] const thread_safe_source& source() const noexcept
    {
        return source_;
    }

private:
    // Walks one more unit, under the lock. False when there is none.
    bool walk_next();
    // Ends the walk with the error of a limit, which units after it throw.
    void stop(errc code, std::string_view section, const std::string& message, error_context context);

    thread_safe_source source_;
    limits limits_;
    bool strict_;
    mutable std::mutex mutex_;
    std::deque<hdu_info> units_;
    diagnostic_log log_;
    // The bytes of the last read of the walk, which serve the headers of small units after it.
    read_window window_;
    std::uint64_t next_offset_ = 0;
    std::uint64_t header_bytes_ = 0;
    std::uint64_t diagnostics_ = 0;
    bool ended_ = false;
    // The error that stopped the walk, which asking for a unit after the last one throws.
    std::optional<diagnostic> stop_{};
};

} // namespace openfits::detail
