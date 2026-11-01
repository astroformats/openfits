// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/error.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace openfits::detail {

/// Throws the exception for an error found in a file, which says what kind of failure the code is: limit_error for a
/// limit of the walk (errc::too_many_hdus, errc::header_too_large), and invalid_data_error for every other code.
[[noreturn]] void throw_file_error(errc code, std::string_view message, error_context context);

/// The diagnostics recorded while a file is read. In strict mode, an error fails the read instead.
///
/// The log lists at most max_diagnostics of them (limits::max_diagnostics, zero for no limit), since a file can hold a
/// million problems. When there are more, the last one stands for itself and all those after it: it says how many
/// they are, and has the severity of the most serious of them and the code of the first of those, without a context.
class diagnostic_log
{
public:
    diagnostic_log(bool strict, std::uint64_t max_diagnostics) noexcept
        : strict_(strict), max_diagnostics_(max_diagnostics)
    {}

    /// Something the standard allows was noted.
    void info(errc code, std::string message, error_context context = {});

    /// A harmless departure from the standard was tolerated.
    void warning(errc code, std::string message, error_context context = {});

    /// Part of the file is unavailable. Throws the exception of throw_file_error() instead in strict mode.
    void error(errc code, std::string message, error_context context = {});

    /// Records a diagnostic of any severity, as the function of that severity does.
    void add(openfits::severity level, errc code, std::string message, error_context context = {});

    [[nodiscard]] const std::vector<diagnostic>& entries() const noexcept
    {
        return entries_;
    }

    /// Moves the diagnostics out of the log, which is empty afterwards.
    [[nodiscard]] std::vector<diagnostic> release() noexcept;

private:
    void record(openfits::severity level, errc code, std::string message, error_context context);

    bool strict_;
    std::uint64_t max_diagnostics_;
    std::vector<diagnostic> entries_;
    // The number of problems that the last entry stands for when the log is full, or zero.
    std::uint64_t unlisted_ = 0;
};

} // namespace openfits::detail
