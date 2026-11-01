// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/export.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

/// @file
/// Exceptions, error codes and diagnostics.

namespace openfits {

/// Why an operation failed. The exception type says what kind of failure it is, and this code says why.
/// Callers branch on either, never on the message text. Zero is not a code.
enum class errc
{
    invalid_argument = 1, ///< A precondition of the API was violated.
    cancelled,            ///< A progress callback asked to stop.
    arithmetic_overflow,  ///< A size or offset computation does not fit its type.
    allocation_too_large, ///< Data that need a single allocation larger than limits::max_allocation.
    entropy_unavailable,  ///< The operating system provided no random data.
    invalid_utf8,         ///< Text is not well-formed UTF-8, or holds U+0000, such as a path.

    open_failed,        ///< A file could not be opened or created.
    not_a_regular_file, ///< A path names a directory, a device or anything else that is not a regular file.
    not_seekable,       ///< A stream cannot change its position, which a source needs.
    read_failed,        ///< Reading from a source or a device failed.
    end_of_data,        ///< A read extends beyond the end of a source.
    write_failed,       ///< Writing to a sink or a device, or committing what was written, failed.
    file_exists,        ///< The file that a save would create exists, and the save may not replace it.

    invalid_character, ///< A keyword record holds a byte outside the printable ASCII characters, 0x20 to 0x7E
                       ///< (standard §4.1.2.3).
    invalid_keyword,   ///< A keyword name that is not of standard §4.1.2.1: lowercase letters, embedded spaces, other
                       ///< characters, or more than eight characters without the HIERARCH convention; or a keyword
                       ///< that cannot have the value or the record it is given.
    invalid_value_indicator, ///< A value indicator other than '= ' in bytes 9 and 10 (standard §4.1.2.2).
    invalid_value,           ///< A value field that holds none of the forms of standard §4.2.
    invalid_string,          ///< A character string without its closing quote (standard §4.2.1.1).
    invalid_logical,         ///< A logical value in lowercase, t or f (standard §4.2.2).
    invalid_integer,         ///< Text that is not an integer of standard §4.2.3.
    invalid_real,    ///< Text that is not a real floating-point number of standard §4.2.4, such as one with a lowercase
                     ///< exponent letter; or a NaN or an infinity to write, which no keyword value can hold.
    invalid_complex, ///< Text that is not a complex number of standard §4.2.5 or §4.2.6.
    value_out_of_range, ///< A well-formed value that its type cannot represent, such as an integer beyond -2^63 to
                        ///< 2^64 - 1 or a real beyond the range of double.
    invalid_comment,    ///< Text after the value of a keyword record that does not start with the slash of a comment
                        ///< (standard §4.1.2.3).
    value_too_long,     ///< A value that does not fit in its keyword record.
    comment_too_long,   ///< A comment, or the text of a commentary keyword, that does not fit in its keyword record.

    invalid_date,           ///< Text that is not a date of standard §4.2.7 and §9.1.1; or a date_time to write that
                            ///< is not a valid date and time.
    invalid_display_format, ///< Text that is not a display format of standard §7.3.4 (TDISPn, Tables 16 and 20); or a
                            ///< format that cannot display a value of the type given.

    keyword_not_found, ///< No card of the header has the keyword asked for.
    keyword_undefined, ///< The value of the keyword is undefined: its value field is blank (standard §4.1.2.3).
    keyword_type,      ///< The value of the keyword does not convert to the type asked for, or the card has none.

    not_a_fits_file,   ///< A source does not start with the SIMPLE keyword of a primary header (standard §4.4.1.1).
    missing_end,       ///< A header has no END record before the end of the file (standard §4.4.1).
    header_fill,       ///< The last block of a header holds other bytes than spaces after END (standard §3.3.1).
    truncated_file,    ///< The file ends within a block, or before the data that a header describes (standard §3.1).
    special_records,   ///< Bytes after the last unit that are not a header: special records or else (standard §3.5).
    missing_keyword,   ///< A mandatory keyword is missing (standard §4.4.1).
    keyword_order,     ///< A mandatory keyword out of its place, or another keyword among them (standard §4.4.1).
    free_format,       ///< The value of a mandatory keyword in free format, where the fixed format is required
                       ///< (standard §4.2).
    invalid_structure, ///< A mandatory keyword with a value that the unit cannot have: a BITPIX outside its values, a
                       ///< NAXIS above 999, a negative NAXISn, PCOUNT or GCOUNT, or a size beyond 64 bits (standard
                       ///< §4.4.1).
    reserved_keyword,  ///< A reserved keyword with a value of the wrong type or form, or that the unit cannot have
                       ///< (standard §4.4.2).
    repeated_keyword,  ///< A keyword with a value appears more than once in a header.
    orphaned_continue, ///< A CONTINUE record that continues no string, read as commentary (standard §4.2.1.2).
    continued_reserved_keyword, ///< A CONTINUE record that continues the string of a mandatory or reserved keyword
                                ///< (standard §4.2.1.2).
    too_many_hdus,              ///< A file has more units than limits::max_hdus.
    header_too_large,           ///< A header larger than limits::max_header_bytes, or headers of a file beyond
                                ///< limits::max_total_header_bytes.
    hdu_not_found,              ///< No unit of the file has the index, or the name and the version, asked for.
    invalid_file_specification, ///< A file specification that does not parse, or whose section does not fit its unit.
    structural_keyword,         ///< A keyword that the library writes itself, given in a header to write.

    not_an_image,            ///< A unit that holds no image is asked for one: a table, random groups or an unknown
                             ///< extension.
    undefined_value,         ///< An undefined value, NaN or BLANK, that the destination of a conversion cannot hold.
    unsupported_compression, ///< Data compressed in a way that this build does not decompress.
};

/// Where in a file a problem was found. Empty or absent members are unknown or do not apply.
struct error_context
{
    std::optional<std::uint64_t> hdu{};    ///< Index of the header and data unit, 0 for the primary.
    std::string keyword{};                 ///< Name of the keyword.
    std::optional<std::uint64_t> column{}; ///< Number of the table column, from 1 as in TFORMn.
    std::optional<std::uint64_t> offset{}; ///< Byte offset in the source.

    friend bool operator==(const error_context&, const error_context&) = default;
};

#if defined(_MSC_VER)
// The exceptions derive from std::runtime_error and hold standard-library members, which cl reports for an exported
// class. A C++ interface requires the same standard library on both sides of the DLL boundary anyway.
#pragma warning(push)
#pragma warning(disable : 4251 4275)
#endif

// Each exception class declares its copies, since it declares its destructor: the implicit copies of such a class
// are deprecated, which -Wdeprecated reports in applications. It declares no moves, so that a move copies: a
// defaulted one would leave the error without its context.
// NOLINTBEGIN(cppcoreguidelines-special-member-functions)

/// Base class of the exceptions that OpenFITS throws for its failures. Copying an error never throws.
///
/// what() returns the message followed by the context, if there is one.
///
/// Besides them, a function that allocates memory can throw std::bad_alloc, and the exceptions of the application's
/// own sources, sinks, devices and progress functions pass through unchanged.
class OPENFITS_API error : public std::runtime_error
{
public:
    /// The message describes the failure without its context, which what() appends.
    error(errc code, std::string_view message, error_context context = {});
    error(const error&) = default;
    error& operator=(const error&) = default;
    ~error() override;

    /// The reason of the failure.
    [[nodiscard]] errc code() const noexcept;
    /// Where the failure happened.
    [[nodiscard]] const error_context& context() const noexcept;

private:
    errc code_;
    std::shared_ptr<const error_context> context_;
};

/// An operating-system call, a source, a sink or a device failed.
class OPENFITS_API io_error : public error
{
public:
    io_error(errc code, std::string_view message, std::error_code system_code = {}, error_context context = {});
    io_error(const io_error&) = default;
    io_error& operator=(const io_error&) = default;
    ~io_error() override;

    /// The operating-system error, when there is one.
    [[nodiscard]] std::error_code system_code() const noexcept;

private:
    std::error_code system_code_;
};

/// The input violates the FITS standard, or a value does not convert to the type asked for.
class OPENFITS_API invalid_data_error : public error
{
public:
    using error::error;
    invalid_data_error(const invalid_data_error&) = default;
    invalid_data_error& operator=(const invalid_data_error&) = default;
    ~invalid_data_error() override;
};

/// Data failed an integrity check: a checksum mismatch, a wrong decompressed size or corrupt compressed data.
class OPENFITS_API integrity_error : public error
{
public:
    using error::error;
    integrity_error(const integrity_error&) = default;
    integrity_error& operator=(const integrity_error&) = default;
    ~integrity_error() override;
};

/// Valid input that this build cannot handle.
class OPENFITS_API unsupported_error : public error
{
public:
    using error::error;
    unsupported_error(const unsupported_error&) = default;
    unsupported_error& operator=(const unsupported_error&) = default;
    ~unsupported_error() override;
};

/// A safety limit was exceeded (see openfits::limits).
class OPENFITS_API limit_error : public error
{
public:
    using error::error;
    limit_error(const limit_error&) = default;
    limit_error& operator=(const limit_error&) = default;
    ~limit_error() override;
};

/// A model given to the writer violates the FITS standard. Its context names the unit and the keyword.
class OPENFITS_API validation_error : public error
{
public:
    using error::error;
    validation_error(const validation_error&) = default;
    validation_error& operator=(const validation_error&) = default;
    ~validation_error() override;
};

/// A precondition of the API was violated, such as an index out of range or a path that is not valid UTF-8.
class OPENFITS_API usage_error : public error
{
public:
    using error::error;
    usage_error(const usage_error&) = default;
    usage_error& operator=(const usage_error&) = default;
    ~usage_error() override;
};

/// A progress callback asked to stop. A save leaves no file, and a read into memory of the caller may leave part of
/// the data there.
class OPENFITS_API cancelled_error : public error
{
public:
    using error::error;
    cancelled_error(const cancelled_error&) = default;
    cancelled_error& operator=(const cancelled_error&) = default;
    ~cancelled_error() override;
};

// NOLINTEND(cppcoreguidelines-special-member-functions)

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/// How serious a diagnostic is.
enum class severity
{
    info,    ///< Something the standard allows was noted, or a departure that changes nothing was read.
    warning, ///< A harmless departure from the standard was tolerated, and the data are read as intended.
    error,   ///< Part of the file is unavailable. Strict reading turns this into a failure.
};

/// A problem found while reading a file that did not stop the reading.
struct diagnostic
{
    openfits::severity severity = openfits::severity::info;
    errc code = errc::invalid_argument;
    std::string message{};
    error_context context{};

    friend bool operator==(const diagnostic&, const diagnostic&) = default;
};

} // namespace openfits
