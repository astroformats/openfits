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

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

/// @file
/// Writing FITS files: headers, images and tables.

namespace openfits {

class reader;

/// What a save to a path does when the file exists.
enum class if_exists : std::uint8_t
{
    replace, ///< Replaces it, once the new file is complete.
    fail,    ///< Fails with io_error (errc::file_exists), and leaves it as it is.
};

/// What a save does with the bytes outside printable ASCII, 0x20 to 0x7E, in the cards of a header: in strings,
/// comments and the text of commentary cards, and anywhere in the records of cards read from a file. The standard
/// allows none (standard §4.1.2.3), and a keyword name never takes them.
enum class non_ascii : std::uint8_t
{
    refuse,  ///< Fails with validation_error (errc::invalid_character), before anything is written.
    replace, ///< Writes a question mark in place of each such byte.
    keep,    ///< Writes them as they are, which the standard forbids: to copy cards as they were read.
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
    /// What the bytes outside printable ASCII of the cards of a header become.
    openfits::non_ascii non_ascii = openfits::non_ascii::refuse;
    /// What a save to a path does when the file exists.
    openfits::if_exists if_exists = openfits::if_exists::replace;
    /// Flush the file to the storage device before it replaces the target, so that it survives a power failure.
    /// Slower.
    bool flush_to_disk = false;
    /// Write DATASUM and CHECKSUM in every unit, after the cards of its header (standard §4.4.2.7): the sum of its data
    /// records, and the string of standard Appendix J that makes the sum of the whole unit negative zero, which
    /// readers verify the unit against. The data are summed as they are written, and each header completed in place
    /// once its data are written; a sink that cannot rewrite has them summed in a pass of their own first, which the
    /// values of a stream cannot take (writer::add_image() and writer::add_table() of a function): such a sink is
    /// refused then. A unit copied as it is keeps its own (writer::add_hdu()).
    bool checksums = false;
    /// The time of the checksums, written as the comment of DATASUM and CHECKSUM in the form of format_date(), as
    /// standard §4.4.2.7 recommends; no comment without one, since the library reads no clock.
    std::optional<date_time> checksum_time{};
    /// The compression of the images and the binary tables whose options give none (standard §10); none when empty, so
    /// that every reader reads the file. A first image that it compresses is preceded by a primary HDU without data,
    /// which the standard requires, so that it and every unit after it lie one index further in the file than their
    /// writer gave them.
    std::optional<compression_options> compression{};
    /// Compress the whole file with gzip (RFC 1952) at this level of zlib, from 1, the fastest, to 9, the smallest, 0
    /// for none or -1 for its default, 6, as gzip_sink compresses it: a `.fits.gz` file, a convention outside the
    /// standard, which readers that inflate gzip read. Not compressed when empty. A gzip sink cannot rewrite, so that
    /// the checksums of streamed units, and a streamed table whose header is completed once its rows are written, are
    /// refused, as over any sink that cannot rewrite.
    std::optional<int> gzip_level{};
    /// Called as the file is written, and able to cancel the save. Its units of work are the bytes of the file, before
    /// a gzip compression, and with checksums over a sink that cannot rewrite, the bytes of the data once more, summed
    /// before the file is written. With a table of streamed rows whose number is not given, the total grows as they
    /// are written.
    progress_function progress{};
};

/// What the data of a reserved image hold until an editor fills them (writer::add_reserved_image()).
enum class reserved_fill : std::uint8_t
{
    zeros,            ///< Stored values of 0, which a file holds without writing them where its sink allows it.
    undefined_values, ///< BLANK in integer data, which the image must give, and NaN in floating-point data (standard
                      ///< §4.4.2.5, §5.3).
};

/// Options of a reserved image of the writer.
struct reserved_image_options
{
    /// Blank cards before END: room that later changes of the header take in place (standard §4.4.2.4).
    std::size_t reserved_cards = 0;
    /// What the data hold.
    reserved_fill fill = reserved_fill::zeros;
};

/// Gives the values of an image that the writer streams (writer::add_image() of a function): fills values, which hold
/// values of the type the image was added with, in the byte order of the host, with the elements of the image from the
/// element at index first on, in the order of the data, the first axis varying fastest. A save calls it in order, for
/// consecutive runs of elements from the first to the last: whole rows, the elements along the first axis, as many as
/// fit in about 256 KiB of values, or part of a row when one row is larger; for a compressed image, the rows of as many
/// whole bands of tiles as fit in 64 MiB of stored values, one band at least. Each save calls it again from the first
/// element. Its exceptions pass through unchanged, and end the save.
using image_fill = std::function<void(std::uint64_t first, std::span<std::byte> values)>;

/// Gives the rows of a table that the writer streams (writer::add_table() of a function): the values of each column of
/// the table for the rows from the row at index first on, as many rows as it chooses, in a column_values for each
/// column as add_table() takes them; its values stay valid until it is called again or the save ends. No columns, or
/// columns of no row, end the table. A save calls it in order, the first row after the rows it gave so far, and each
/// save again from row 0. Its exceptions pass through unchanged, and end the save.
using table_rows = std::function<std::vector<column_values>(std::uint64_t first)>;

/// Options of a unit copied by the writer (writer::add_hdu()).
struct hdu_copy_options
{
    /// The cards of the header of the copy but its structural ones, which the writer takes from the unit: a header such
    /// as without_structural() of the unit's, edited. Empty to copy the cards of the unit as they are.
    std::optional<openfits::header> header{};
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
/// never modifies them. They can also be streamed, given by a function that a save calls for each chunk in order, so
/// that a unit larger than memory is written with the memory of a chunk (image_fill, table_rows); reserved, an image
/// whose data an editor fills later, region by region (add_reserved_image()); or copied, a unit of a file as it is,
/// read when the file is saved (add_hdu()). save() formats every header before it writes anything, and throws
/// validation_error, naming the unit and the keyword, for a model that the standard forbids: a keyword, a value or a
/// comment that no record can hold, a structural keyword in a header, a scaling, a BLANK, a TNULLn or a compression
/// that the data cannot have. The headers of streamed units are completed once their data are written: their checksums,
/// and the NAXIS2 or the heap of a table whose rows or arrays are known only then, which a sink that can rewrite takes.
/// A writer is thread-compatible, like a standard container: save() can run on several threads at once, and calls the
/// functions of streamed units from each, and a change needs exclusive access. It can be moved, and a moved-from writer
/// can only be destroyed or assigned to.
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
    ///         of override has a scale of 0 or a value that is not finite.
    /// @throws validation_error for an image that the standard forbids: more than 999 axes, data of more than 2^64
    ///         bytes, a BSCALE of 0 or a scaling that is not finite, or a BLANK with floating-point data or beyond the
    ///         range of the stored type; and for a compression that options ask for, the errors of compression_options.
    template <numeric_value T>
    std::size_t add_image(image_info info, std::span<const T> data, image_write_options options = {})
    {
        return add_values(std::move(info), numeric_type_of<T>(), std::as_bytes(data), {}, std::move(options));
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

    /// Adds an image whose values a save takes from fill, chunk after chunk, as values of type in the byte order of the
    /// host, and writes as add_image() writes values in memory: the same bytes, with the memory of a chunk.
    /// Uncompressed, each chunk is written as it is filled, and with checksums the header completed once the data are
    /// written, which a sink that can rewrite takes. Compressed, the tiles of each band of rows are compressed as its
    /// chunks are filled, and the compressed data held, as for values in memory, until the unit is written: size() and
    /// header_records() then call fill too.
    /// @throws usage_error (errc::invalid_argument) when fill is empty, and the exceptions of the other add_image().
    std::size_t add_image(image_info info, numeric_type type, image_fill fill, image_write_options options = {});

    /// Adds an image whose values a save takes from fill, called with a std::span<T> of each chunk to fill, as the
    /// add_image() of an image_fill does, such as add_image<float>(info, fill).
    template <numeric_value T, typename Function>
        requires std::invocable<Function&, std::uint64_t, std::span<T>>
    std::size_t add_image(image_info info, Function fill, image_write_options options = {})
    {
        image_fill typed = [function = std::move(fill)](std::uint64_t first, std::span<std::byte> values) mutable {
            // The chunks of a save are allocated for values of the largest type, so that they are aligned for T.
            void* chunk = values.data();
            function(first, std::span<T>(static_cast<T*>(chunk), values.size() / sizeof(T)));
        };
        return add_image(std::move(info), numeric_type_of<T>(), std::move(typed), std::move(options));
    }

    /// Adds an image without values whose data are reserved, uncompressed whatever the options of the save say: zeros
    /// or undefined values, as options.fill says, which an editor then fills in place, region by region, so that an
    /// output such as a stacked frame or a mosaic is written once without being held in memory whole. The zeros are
    /// written in large chunks, or as a file made longer where the sink allows it (output_sink::write_zeros()). info
    /// gives the BITPIX of the data, and the scaling and BLANK that the header gives.
    /// @throws usage_error (errc::invalid_argument) when info has no BITPIX, or no BLANK for options.fill
    ///         undefined_values of integer data; and validation_error as add_image() does.
    std::size_t add_reserved_image(image_info info, reserved_image_options options = {});

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

    /// Adds a table whose rows a save takes from rows, chunk after chunk, and writes as add_table() writes values in
    /// memory, with the memory of a chunk: the rows of each chunk are written as they come. info gives the format of
    /// every column, since the header is written before the rows, and the number of rows when it is not 0: rows then
    /// gives exactly as many; with 0, the table ends when rows gives none, and its NAXIS2 is written once they are
    /// written. A table with variable-length arrays needs its number of rows: its rows are written as zeros, then each
    /// chunk's rows once their arrays are in the heap, and the size of the heap and the emax of each column once they
    /// are all written. A streamed table is never compressed, as an ASCII table is not.
    /// @throws usage_error (errc::invalid_argument) when rows is empty, a column has no format, or a table with
    ///         variable-length arrays has no number of rows; and the exceptions of the other add_table().
    std::size_t add_table(table_info info, table_rows rows, const table_write_options& options = {});

    /// Adds a copy of the unit at index of source after the units added so far, which a save reads from source as it
    /// writes it: its header and its data as they are, byte for byte, compressed tiles and extensions of unknown types
    /// included, so that its checksums stay what they were. The reader is borrowed: it must stay valid until the last
    /// save().
    ///
    /// A unit placed where its kind cannot be is converted as it is written: the primary HDU of an image after the
    /// first unit becomes an IMAGE extension, its XTENSION, PCOUNT and GCOUNT in place of SIMPLE and EXTEND, and an
    /// IMAGE extension added first the primary HDU, its SIMPLE and EXTEND in place of XTENSION, PCOUNT and GCOUNT; any
    /// other extension added first comes after a primary HDU of NAXIS = 0, as add_table() adds one. Its header is then
    /// written anew, its cards in their order, as it is when the save asks for checksums or replaces bytes outside
    /// printable ASCII; options.header replaces its cards but the structural ones, which come first, as the unit has
    /// them. A header written anew gets CHECKSUM and DATASUM computed again when the unit had them or the save asks for
    /// them. Returns the index of the copy.
    /// @throws usage_error (errc::invalid_argument) for random groups placed after the first unit, which only a primary
    ///         HDU holds (standard §6), and the exceptions of reader::hdu(); invalid_data_error when the header of the
    ///         unit gives no size for its data.
    std::size_t add_hdu(const reader& source, std::size_t index, hdu_copy_options options = {});

    /// The number of units added.
    [[nodiscard]] std::size_t hdu_count() const noexcept;

    /// Writes the file to path, which is UTF-8 on every platform and the name of the file, brackets included, through
    /// file_sink: the file is replaced, or created, only once it is complete, so that a failed or cancelled save
    /// leaves the old one, or none; options.if_exists refuses an existing file instead.
    /// @throws usage_error when path is empty or not valid UTF-8, or the writer has no unit; (errc::invalid_argument)
    ///         when a streamed table gives rows of other columns than its model, or another number of rows than it
    ///         says, or options.gzip_level is not from -1 to 9.
    /// @throws validation_error when a header cannot be written; nothing is written then.
    /// @throws invalid_data_error with errc::value_out_of_range for a value beyond the stored type when its options do
    ///         not clamp it, and errc::undefined_value for a NaN in integer data without BLANK.
    /// @throws io_error when the file cannot be written, or exists and options.if_exists refuses it
    ///         (errc::file_exists), and the other exceptions of file_sink.
    /// @throws cancelled_error when options.progress returns false.
    void save(std::string_view path, const write_options& options = {}) const;

    /// Writes the file to sink, in order, then calls its finish(), with the exceptions of the other save() and those of
    /// the sink, which pass through unchanged; with options.gzip_level, through a gzip_sink over it. With checksums,
    /// the writer rewrites the checksum cards of each header once its data are written when the sink can, and sums the
    /// data before it writes anything otherwise. A failure may leave part of the file in it.
    /// @throws usage_error (errc::invalid_argument), before anything is written, when the sink cannot rewrite and a
    ///         header needs it: a streamed unit with checksums, a streamed table whose rows are not counted in its
    ///         model, or that has variable-length arrays.
    void save(output_sink& sink, const write_options& options = {}) const;

    /// The bytes of the file that save() with options writes, its headers formatted as save() formats them: room to
    /// reserve before a save, as memory_sink::reserve() takes it. With options.gzip_level, the bytes before their
    /// compression.
    /// @throws usage_error when the writer has no unit, or (errc::invalid_argument) a streamed table that does not say
    ///         its number of rows, whose size is known once it is written; and validation_error when a header cannot be
    ///         written.
    [[nodiscard]] std::uint64_t size(const write_options& options = {}) const;

    /// The records of the header of the unit at index as save() with options writes it, END included and the fill of
    /// its last block left out: the cards that a reader of the file finds in it.
    /// @throws usage_error (errc::hdu_not_found) when the writer has no unit at index, and validation_error when the
    ///         header cannot be written.
    [[nodiscard]] std::size_t header_records(std::size_t index, const write_options& options = {}) const;

private:
    std::size_t add_values(image_info info, numeric_type type, std::span<const std::byte> data, image_fill fill,
                           image_write_options options);

    struct state;
    std::unique_ptr<state> state_;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace openfits
