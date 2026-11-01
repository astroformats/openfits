// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/error.h>
#include <openfits/export.h>
#include <openfits/header.h>
#include <openfits/types.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// @file
/// Header and data units as a reader walks them, and the specification of a file with a unit and an image section.

namespace openfits {

namespace detail {
struct hdu_access;
} // namespace detail

#if defined(_MSC_VER)
// The members hold standard-library types, which cl reports for an exported class. A C++ interface requires the same
// standard library on both sides of the DLL boundary anyway.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

/// A header and data unit of a file, as a reader walks it: its kind, its header, where its header and its data lie, and
/// the problems found in its header. A reader keeps it unchanged, at the same address, as long as the reader exists.
class OPENFITS_API hdu_info
{
public:
    /// The kind of the unit, from its first keywords: SIMPLE or XTENSION, and for a binary table ZIMAGE and ZTABLE.
    [[nodiscard]] hdu_kind kind() const noexcept
    {
        return kind_;
    }

    /// The index of the unit in the file, 0 for the primary HDU.
    [[nodiscard]] std::size_t index() const noexcept
    {
        return index_;
    }

    [[nodiscard]] const openfits::header& header() const noexcept
    {
        return header_;
    }

    /// The offset of the first block of the header in the file.
    [[nodiscard]] std::uint64_t header_offset() const noexcept
    {
        return header_offset_;
    }

    /// The bytes of the blocks of the header, the block of END included: a multiple of 2880, but where the file ends
    /// before.
    [[nodiscard]] std::uint64_t header_size() const noexcept
    {
        return header_size_;
    }

    /// The offset of the data, which follow the header.
    [[nodiscard]] std::uint64_t data_offset() const noexcept
    {
        return header_offset_ + header_size_;
    }

    /// The bytes of the data that the header describes (standard §4.4.1), without the fill of their last block; nothing
    /// when the header gives no size, which stops the walk of the file at this unit with an error among its
    /// diagnostics.
    [[nodiscard]] std::optional<std::uint64_t> data_size() const noexcept
    {
        return data_size_;
    }

    /// The name of the unit: its EXTNAME, or else its HDUNAME, without trailing spaces; empty without either.
    [[nodiscard]] const std::string& name() const noexcept
    {
        return name_;
    }

    /// The version of the unit: its EXTVER, or 1 without it (standard §4.4.2.6).
    [[nodiscard]] std::int64_t version() const noexcept
    {
        return version_;
    }

    /// The problems found in the header and in the size of the unit, in the order of the records.
    [[nodiscard]] const std::vector<diagnostic>& diagnostics() const noexcept
    {
        return diagnostics_;
    }

private:
    friend struct detail::hdu_access;

    hdu_kind kind_ = hdu_kind::primary;
    std::size_t index_ = 0;
    openfits::header header_{};
    std::uint64_t header_offset_ = 0;
    std::uint64_t header_size_ = 0;
    std::optional<std::uint64_t> data_size_{};
    std::string name_{};
    std::int64_t version_ = 1;
    std::vector<diagnostic> diagnostics_{};
};

/// One axis of the image section of a file specification: the pixels from first to last, counted from 1, every step-th
/// of them, in reverse when first is beyond last. Nothing in first or last stands for the last pixel of the axis, so
/// that the default, from 1 to nothing, is the whole axis, written `*`, and `-*`, from nothing to 1, is the whole axis
/// in reverse.
struct section_range
{
    std::optional<std::uint64_t> first = 1;
    std::optional<std::uint64_t> last{};
    std::uint64_t step = 1;

    friend bool operator==(const section_range&, const section_range&) = default;
};

/// A file, a unit of it and an image section of that unit, as the extended file names of CFITSIO select them, a
/// convention outside the standard: `path[3]`, `path[SCI]` and `path[SCI,2]` select a unit, and a second bracket, or
/// the first, a section, `path[1][1:512,1:512]`, `path[*,-*]` or `path[SCI][100:1:2,*]`.
///
/// A path given to a reader as a string is always taken literally, brackets included, so that `M31 [Ha].fits` opens as
/// itself: a file specification is made only by parse().
struct file_specification
{
    /// The path of the file, everything before the first bracket.
    std::string path{};
    /// The index of the unit, 0 for the primary HDU, from [n].
    std::optional<std::size_t> hdu_index{};
    /// The name of the unit, from [name] and [name, version], as reader::hdu() finds it; empty for none.
    std::string hdu_name{};
    /// The version of the named unit, 0 for any.
    std::int64_t hdu_version = 0;
    /// The image section, a range for each of the first axes, the others whole; empty for none.
    std::vector<section_range> section{};

    /// Parses text, a path followed by a unit selector, a section, or both. A section without a unit selector applies
    /// to the first unit that holds an image. Ranges are `*`, `-*`, `first:last` and either with `:step` after it, the
    /// pixels counted from 1, and `:step` alone for the whole axis.
    /// @throws usage_error (errc::invalid_file_specification) for text that is not of this form, such as an empty path,
    ///         a bracket that is not closed, a range of pixel 0 or of step 0, and the other filters of CFITSIO's
    ///         syntax, which this library does not apply: the selection of columns, rows, pixels and histograms, a
    ///         unit selected by its type, an image in a table cell, compression and output options. Its message names
    ///         the filter.
    [[nodiscard]] OPENFITS_API static file_specification parse(std::string_view text);

    friend bool operator==(const file_specification&, const file_specification&) = default;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/// The header of an image section: the header of the image, with each NAXISn the length of the section along its axis,
/// and the WCS keywords that a section moves changed as CFITSIO changes them when it copies a section, in each
/// description, the primary one and the alternates A to Z: CRPIXn to the pixel that the reference point becomes, where
/// the section does not start at the first pixel or has a step; and where it has a step or runs in reverse, CDELTn and
/// the CDi_n of the axis multiplied by the step, negated in reverse, where CRPIXn of the same description exists. The
/// changed values are rounded to 16 significant digits, as CFITSIO writes them. section is a region of the image.
/// @throws usage_error (errc::invalid_argument) when section does not have the axes of the image or lies outside it.
/// @throws invalid_data_error as header::value() does when NAXIS or an NAXISn is not an integer.
[[nodiscard]] OPENFITS_API openfits::header adjust_for_section(const openfits::header& image, const region& section);

} // namespace openfits
