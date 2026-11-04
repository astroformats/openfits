// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/error.h>
#include <openfits/export.h>
#include <openfits/reader.h>

#include <string_view>
#include <vector>

/// @file
/// The conformance report of a file: each departure from the FITS standard that the library finds in it, with the
/// section of the standard that it departs from.

namespace openfits {

/// The conformance report of the file that file reads: each departure from the standard that the library finds in it,
/// with its severity, its unit, its keyword or column, and the section of the standard that it departs from, in the
/// order of the units, those of the file outside a unit last. An empty report says that the file conforms, as far as
/// the library can tell.
///
/// The report holds what a strict reading finds, the diagnostics of the walk of every unit and of each header, which
/// the tolerance of the reader reads through, and then what reads do not check: the fill of the last block of each data
/// unit, zeros, or spaces in an ASCII table (standard §3.3.2, §6.2, §7.1.3, §7.2.3, §7.3.3); every field of every ASCII
/// table, a number of the forms of standard §7.2.5 or ASCII text; the logicals and characters of binary tables
/// (standard §7.3.3.1); every descriptor of a variable-length array in every row, against the heap and emax (standard
/// §7.3.5); the checksums of every unit that has them (standard §4.4.2.7); units of one type with the same EXTNAME,
/// EXTVER and EXTLEVEL (standard §4.4.2.6); the descriptions of the images, tables and random groups that reads take,
/// compressed ones included (standard §10); and ZQUANTIZ = 'NONE', a convention outside the standard. A problem that
/// repeats in the fields of a column, or in the rows of a table, is listed ten times, and the eleventh finding of it
/// counts the others.
///
/// The data of every unit are read, some more than once. Diagnostics of the walk beyond limits::max_diagnostics of the
/// reader are counted, not listed, as the reader counts them; a unit that the walk cannot reach is not reported on.
/// @throws the exceptions of the source.
/// @throws cancelled_error when options.progress returns false. Its units of work are the bytes of the file.
[[nodiscard]] OPENFITS_API std::vector<diagnostic> verify(const reader& file, const verify_options& options = {});

/// The conformance report of the file at path, as the other verify() makes it of a reader that opens the file with
/// open, whose strict member is not used. A file that is not a FITS file, whose primary header is beyond the limits of
/// open, or whose gzip compression is corrupt, has a report of that finding alone, of severity error.
/// @throws usage_error when path is empty or not valid UTF-8, and io_error when the file cannot be read; and the
///         exceptions of the other verify().
[[nodiscard]] OPENFITS_API std::vector<diagnostic> verify(std::string_view path, const read_options& open = {},
                                                          const verify_options& options = {});

} // namespace openfits
