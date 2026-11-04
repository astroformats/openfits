// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/writer.h>

#include <cstddef>
#include <span>
#include <vector>

// Files written by the writer of the library, for its tests. Every file written in tests can be recorded for other
// readers, astropy and the tools of CFITSIO, which CI runs over them: set OPENFITS_WRITTEN_FILES_DIR to an existing
// directory.

namespace openfits::test {

/// The sinks that the writer writes into.
enum class sink_kind
{
    rewritable,  ///< A memory_sink, which can rewrite.
    append_only, ///< A callback_sink without a rewrite function.
};

/// Writes a file to the directory of OPENFITS_WRITTEN_FILES_DIR, when it is set, named after the running test:
/// suite.name.n.fits, the files of one test numbered in order.
void record_file(std::span<const std::byte> file);

/// The file that output writes with options into a sink of the given kind, recorded with record_file(), and verified:
/// each finding of verify() of it is a failure of the test.
[[nodiscard]] std::vector<std::byte> written(const writer& output, const write_options& options = {},
                                             sink_kind kind = sink_kind::rewritable);

} // namespace openfits::test
