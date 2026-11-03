// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/card.h>
#include <openfits/compression.h>
#include <openfits/export.h>
#include <openfits/header.h>
#include <openfits/image.h>
#include <openfits/io.h>
#include <openfits/table.h>
#include <openfits/types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

/// @file
/// Writing FITS files: headers, images and tables.

namespace openfits {

/// What a save to a path does when the file exists.
enum class if_exists : std::uint8_t
{
    replace, ///< Replaces it, once the new file is complete.
    fail,    ///< Fails with io_error (errc::file_exists), and leaves it as it is.
};

/// Options of a save. With the defaults, the file holds the structures of the standard alone, which every FITS reader
/// reads: no keyword of the HIERARCH convention, no LONGSTRN, and long strings over CONTINUE records (standard
/// §4.2.1.2).
struct write_options
{
    /// Write a keyword of more than eight characters, or with spaces, with the HIERARCH convention, which is outside
    /// the standard, rather than refuse it.
    bool hierarch = false;
    /// Write LONGSTRN = 'OGIP 1.0' before the cards of a header that holds a long string, the convention of OGIP that
    /// announces CONTINUE records to readers older than FITS 4.0.
    bool longstrn = false;
    /// Cut a comment that does not fit its record, rather than refuse it.
    bool truncate_comments = false;
    /// Continue the long string of a reserved keyword, such as OBJECT or DATE-OBS, over CONTINUE records, rather than
    /// refuse it: standard §4.2.1.2 forbids it, but CFITSIO writes such strings and readers join them, as the reader
    /// of this library does with a warning. The structural keywords are refused still.
    bool continue_reserved_keywords = false;
    /// What a save to a path does when the file exists.
    openfits::if_exists if_exists = openfits::if_exists::replace;
    /// Flush the file to the storage device before it replaces the target, so that it survives a power failure.
    /// Slower.
    bool flush_to_disk = false;
    /// Write DATASUM and CHECKSUM in every unit, after the cards of its header (standard §4.4.2.7): the sum of its data
    /// records, and the string of standard Appendix J that makes the sum of the whole unit negative zero, which
    /// readers verify the unit against. The data are summed as they are written, and each header completed in place
    /// once its data are written; a sink that cannot rewrite has them summed in a pass of their own first.
    bool checksums = false;
    /// The time of the checksums, written as the comment of DATASUM and CHECKSUM in the form of format_date(), as
    /// standard §4.4.2.7 recommends; no comment without one, since the library reads no clock.
    std::optional<date_time> checksum_time{};
    /// The compression of the images and the binary tables whose options give none (standard §10); none when empty, so
    /// that every reader reads the file. A first image that it compresses is preceded by a primary HDU without data,
    /// which the standard requires, so that it and every unit after it lie one index further in the file than their
    /// writer gave them.
    std::optional<compression_options> compression{};
    /// Called as the file is written, and able to cancel the save. Its units of work are the bytes of the file, and
    /// with checksums over a sink that cannot rewrite, the bytes of the data once more, summed before the file is
    /// written.
    progress_function progress{};
};

/// Options of an image of the writer.
struct image_write_options
{
    /// Blank cards before END: room that later changes of the header take in place (standard §4.4.2.4).
    std::size_t reserved_cards = 0;
    /// What a value beyond the range of the stored type does.
    openfits::overflow_policy overflow = openfits::overflow_policy::refuse;
    /// What the values are: physical values, which the scaling of the image converts into stored ones
    /// (scaling_policy::apply); the stored values themselves, written as they are (scaling_policy::raw); or values that
    /// override_scaling converts into stored ones (scaling_policy::override), such as the codes of std::uint16_t with
    /// the offset of standard §5.2.5, a scale of 1 and a zero of 32768. The header gives the scaling of the image in
    /// every case.
    openfits::scaling_policy scaling = openfits::scaling_policy::apply;
    /// The scaling of scaling_policy::override.
    openfits::scaling override_scaling{};
    /// The compression of the image (standard §10.1), or empty for that of write_options::compression; a compression
    /// of type compression::none keeps it uncompressed whatever the file asks.
    std::optional<compression_options> compression{};
};

/// Options of a table of the writer.
struct table_write_options
{
    /// Blank cards before END: room that later changes of the header take in place (standard §4.4.2.4).
    std::size_t reserved_cards = 0;
    /// What a value beyond the range of the stored type, or of the field of an ASCII table, does.
    openfits::overflow_policy overflow = openfits::overflow_policy::refuse;
    /// What the values of numbers are: physical values, which the TSCALn and TZEROn of their columns convert into
    /// stored ones (scaling_policy::apply), or the stored values themselves, written as they are under the TSCALn and
    /// TZEROn that the header gives (scaling_policy::raw).
    openfits::scaling_policy scaling = openfits::scaling_policy::apply;
    /// The compression of a binary table (standard §10.3), or empty for that of write_options::compression; a
    /// compression of type compression::none keeps it uncompressed whatever the file asks. An ASCII table is never
    /// compressed.
    std::optional<compression_options> compression{};
};

#if defined(_MSC_VER)
// The writer holds a standard-library member, which cl reports for an exported class. A C++ interface requires the
// same standard library on both sides of the DLL boundary anyway.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

/// Writes FITS files: a primary HDU, the first unit added, and IMAGE, TABLE and BINTABLE extensions after it (standard
/// §3.3, §7.1, §7.2, §7.3).
///
/// Each unit gets the structural keywords that its model gives, which the writer makes itself: SIMPLE, BITPIX, NAXIS,
/// NAXISn and EXTEND = T for the primary HDU, XTENSION = 'IMAGE', BITPIX, NAXIS, NAXISn, PCOUNT and GCOUNT for an
/// extension, then BSCALE, BZERO and BLANK when they are not the defaults; for a table, its XTENSION, BITPIX = 8,
/// NAXIS = 2, NAXIS1, NAXIS2, PCOUNT, GCOUNT = 1 and TFIELDS, and for each column TTYPEn, TBCOLn, TFORMn, TUNITn,
/// TSCALn, TZEROn, TNULLn, TDISPn and TDIMn, those that it has; and the cards of its header after them, as they are,
/// none of them structural (is_structural()), then CHECKSUM and DATASUM when write_options::checksums asks for them.
/// The data are written in big-endian order through a staging chunk, and the last block of each unit is filled with
/// zeros, or with spaces for an ASCII table (standard §3.3.2, §7.2.3).
///
/// The values of an image or a table are borrowed: they must stay valid and unchanged until the last save(), which
/// never modifies them. save() formats every header before it writes anything, and throws validation_error, naming the
/// unit and the keyword, for a header that the standard forbids. A writer is thread-compatible, like a standard
/// container: save() can run on several threads at once, and a change needs exclusive access. It can be moved, and a
/// moved-from writer can only be destroyed or assigned to.
class OPENFITS_API writer
{
public:
    writer();
    writer(writer&& other) noexcept;
    writer& operator=(writer&& other) noexcept;
    writer(const writer&) = delete;
    writer& operator=(const writer&) = delete;
    ~writer();

    /// Adds an image after the units added so far, with its values, as many as its axes have, the first axis varying
    /// fastest. Returns the index of its unit: 0 for the first, the primary HDU.
    ///
    /// Without info.bitpix, the values are stored in their own type, with the conventions of standard §5.2.5 for the
    /// types that FITS stores with an offset: std::uint16_t as BITPIX 16 with BZERO 32768, std::uint32_t and
    /// std::uint64_t alike, and std::int8_t as BITPIX 8 with BZERO -128; info.scaling must then be the identity. With
    /// info.bitpix, the values are converted into that type: info.scaling, BSCALE and BZERO, gives the physical value
    /// that each is, the stored value being (value - BZERO) / BSCALE, rounded half away from zero into integers, and a
    /// NaN is stored as info.blank in integer data. With options.scaling raw, the values are the stored values of
    /// info.bitpix, which must be given, converted into its type without the scaling, which the header gives as it is;
    /// with override, they convert into the type of info.bitpix, which must be given, by options.override_scaling, and
    /// the header gives info.scaling as it is.
    /// @throws usage_error (errc::invalid_argument) when data does not hold the values of the axes, a scaling is given
    ///         without info.bitpix, options.scaling is raw or override without info.bitpix, or options.override_scaling
    ///         of override has a scale of 0 or a value that is not finite; (errc::arithmetic_overflow) when the data do
    ///         not fit in 64 bits.
    /// @throws validation_error for an image that the standard forbids: more than 999 axes, a BSCALE of 0 or a scaling
    ///         that is not finite, or a BLANK with floating-point data or beyond the range of the stored type.
    template <numeric_value T>
    std::size_t add_image(image_info info, std::span<const T> data, image_write_options options = {})
    {
        return add_values(std::move(info), numeric_type_of<T>(), std::as_bytes(data), std::move(options));
    }

    /// Adds an image with values that are not const, such as std::span(values) of a std::vector that is not const, as
    /// the overload for const values does.
    template <numeric_value T>
    std::size_t add_image(image_info info, std::span<T> data, image_write_options options = {})
    {
        return add_image(std::move(info), std::span<const T>(data), std::move(options));
    }

    /// Adds an image of values of type, given as their bytes in the byte order of the host, as add_image() adds typed
    /// values, for a caller that knows the type at run time only. The bytes are borrowed, as typed values are, and lie
    /// at an address that is a multiple of the size of the values.
    /// @throws usage_error (errc::invalid_argument) when the size or the address of data is not a multiple of the size
    ///         of the values of type; and the exceptions of the other add_image().
    std::size_t add_image(image_info info, numeric_type type, std::span<const std::byte> data,
                          image_write_options options = {});

    /// Adds a unit of a header alone, without data, after the units added so far: the primary HDU with BITPIX = 8 and
    /// NAXIS = 0 when it is the first, as files of extensions begin, and an IMAGE extension of NAXIS = 0 otherwise.
    /// reserved_cards blank cards come before END. Returns the index of the unit.
    std::size_t add_header(openfits::header cards, std::size_t reserved_cards = 0);

    /// Adds a table after the units added so far, an ASCII table when info.kind is hdu_kind::table and a binary table
    /// when it is hdu_kind::bintable, with the values of each column of info.columns in values, in the same order. A
    /// table is an extension: when the writer has no unit yet, a primary HDU of NAXIS = 0 comes first, as add_header()
    /// adds it. Returns the index of the unit of the table.
    ///
    /// Each column takes info.rows fields. A column without a format takes it from its values, as an image without
    /// BITPIX does: numbers in their own type, std::uint16_t as I with TZEROn 32768 and the other types that FITS
    /// stores with an offset alike (standard §5.2.5), complex numbers as C or M, strings as A of the longest of them,
    /// logicals as L and bits as X, each field with one element, or the elements of its dimensions; in an ASCII table,
    /// A of the longest string, I of the widest integer of the type, E16.9 for float and D25.17 for double, which read
    /// back as the values written. Variable-length arrays are written in the heap, which the writer builds compact, in
    /// the order of the rows: P descriptors, or Q ones when an offset or a count of the heap needs more than 31 bits,
    /// with emax. A column of characters with dimensions (w, ...) holds substrings of w characters, written with TDIMn,
    /// or as rAw, a convention outside the standard, when its format gives a substring_width. An ASCII table puts one
    /// space between its fields, numbers right-justified in theirs and strings left-justified, and an undefined value,
    /// a NaN or a value whose mask byte is set, as its TNULLn.
    /// With options.scaling raw, the numbers of each column are the stored values of its format, which must be given,
    /// converted into its type without the scaling, which the header gives as it is.
    /// @throws usage_error (errc::invalid_argument) when info.kind is not a kind of table, values do not have a column
    ///         for each column of info, the values of a column are not of a kind its format holds or do not have the
    ///         elements of info.rows fields, a scaling is given without a format, or options.scaling is raw for a
    ///         column of numbers without a format or is override, which the writes of images alone take.
    /// @throws validation_error for a table that the standard forbids: more than 999 columns, a format of the other
    ///         kind of table, a TSCALn or TZEROn of characters, logicals or bits, a TNULLn of a type that has none, a
    ///         TDIMn larger than its field, a TSCALn of 0 or a scaling that is not finite.
    std::size_t add_table(table_info info, std::span<const column_values> values, table_write_options options = {});

    /// The number of units added.
    [[nodiscard]] std::size_t hdu_count() const noexcept;

    /// Writes the file to path, which is UTF-8 on every platform and the name of the file, brackets included, through
    /// file_sink: the file is replaced, or created, only once it is complete, so that a failed or cancelled save
    /// leaves the old one, or none; options.if_exists refuses an existing file instead.
    /// @throws usage_error when path is empty or not valid UTF-8, or the writer has no unit.
    /// @throws validation_error when a header cannot be written; nothing is written then.
    /// @throws invalid_data_error with errc::value_out_of_range for a value beyond the stored type when its options do
    ///         not clamp it, and errc::undefined_value for a NaN in integer data without BLANK.
    /// @throws io_error when the file cannot be written, or exists and options.if_exists refuses it
    ///         (errc::file_exists), and the other exceptions of file_sink.
    /// @throws cancelled_error when options.progress returns false.
    void save(std::string_view path, const write_options& options = {}) const;

    /// Writes the file to sink, in order, then calls its finish(), with the exceptions of the other save() and those of
    /// the sink, which pass through unchanged. The sink needs no rewrite(): with checksums, the writer rewrites the
    /// checksum cards of each header once its data are written when the sink can, and sums the data before it writes
    /// anything otherwise. A failure may leave part of the file in it.
    void save(output_sink& sink, const write_options& options = {}) const;

    /// The bytes of the file that save() with options writes, its headers formatted as save() formats them: room to
    /// reserve before a save, as memory_sink::reserve() takes it.
    /// @throws usage_error when the writer has no unit, and validation_error when a header cannot be written.
    [[nodiscard]] std::uint64_t size(const write_options& options = {}) const;

    /// The records of the header of the unit at index as save() with options writes it, END included and the fill of
    /// its last block left out: the cards that a reader of the file finds in it.
    /// @throws usage_error (errc::hdu_not_found) when the writer has no unit at index, and validation_error when the
    ///         header cannot be written.
    [[nodiscard]] std::size_t header_records(std::size_t index, const write_options& options = {}) const;

private:
    std::size_t add_values(image_info info, numeric_type type, std::span<const std::byte> data,
                           image_write_options options);

    struct state;
    std::unique_ptr<state> state_;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace openfits
