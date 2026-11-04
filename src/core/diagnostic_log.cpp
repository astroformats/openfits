// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "core/diagnostic_log.h"

#include <utility>

namespace openfits::detail {

void throw_file_error(errc code, std::string_view message, error_context context)
{
    if (code == errc::too_many_hdus || code == errc::header_too_large) {
        throw limit_error(code, message, std::move(context));
    }
    throw invalid_data_error(code, message, std::move(context));
}

void diagnostic_log::info(errc code, std::string_view section, std::string message, error_context context)
{
    record(severity::info, code, section, std::move(message), std::move(context));
}

void diagnostic_log::warning(errc code, std::string_view section, std::string message, error_context context)
{
    record(severity::warning, code, section, std::move(message), std::move(context));
}

void diagnostic_log::error(errc code, std::string_view section, std::string message, error_context context)
{
    if (strict_) {
        throw_file_error(code, message, std::move(context));
    }
    record(severity::error, code, section, std::move(message), std::move(context));
}

void diagnostic_log::add(openfits::severity level, errc code, std::string_view section, std::string message,
                         error_context context)
{
    switch (level) {
    case severity::info:
        info(code, section, std::move(message), std::move(context));
        break;
    case severity::warning:
        warning(code, section, std::move(message), std::move(context));
        break;
    case severity::error:
        error(code, section, std::move(message), std::move(context));
        break;
    }
}

void diagnostic_log::add(diagnostic entry)
{
    add(entry.severity, entry.code, entry.section, std::move(entry.message), std::move(entry.context));
}

std::vector<diagnostic> diagnostic_log::release() noexcept
{
    unlisted_ = 0;
    return std::exchange(entries_, {});
}

void diagnostic_log::record(openfits::severity level, errc code, std::string_view section, std::string message,
                            error_context context)
{
    if (max_diagnostics_ == 0 || entries_.size() < max_diagnostics_) {
        entries_.push_back({.severity = level,
                            .code = code,
                            .section = std::string(section),
                            .message = std::move(message),
                            .context = std::move(context)});
        return;
    }
    diagnostic& last = entries_.back();
    if (unlisted_ == 0) {
        // The problem that the last entry was about.
        unlisted_ = 1;
        last.context = {};
    }
    ++unlisted_;
    if (level > last.severity) {
        last.severity = level;
        last.code = code;
        last.section = section;
    }
    last.message = std::to_string(unlisted_) + " more problems were found, which are not listed";
}

} // namespace openfits::detail
