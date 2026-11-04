// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/checksum.h>
#include <openfits/error.h>
#include <openfits/export.h>
#include <openfits/hdu.h>
#include <openfits/image.h>
#include <openfits/io.h>
#include <openfits/limits.h>
#include <openfits/table.h>
#include <openfits/types.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// @file
/// Reading FITS files: the header and data units, walked as they are asked for, and the values of their images,
/// tables and random groups.

namespace openfits {

#if defined(_MSC_VER)
// The reader holds a standard-library member, which cl reports for an exported class. A C++ interface requires the
// same standard library on both sides of the DLL boundary anyway.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

/// Options of a reader.
struct read_options
{
    /// The safety limits of the file, which is untrusted input.
    openfits::limits limits{};
    /// Walk every unit at the open, and fail there with the exception of the first problem that would make part of the
    /// file unavailable, which a diagnostic of severity error reports otherwise; then verify the checksums of every
    /// unit that has them, as reader::verify() does, reading the whole unit, and fail with integrity_error
    /// (errc::checksum_mismatch) at the first that does not match its unit.
    bool strict = false;
    /// Where the content of a gzip-compressed file goes once inflated: into memory within limits::max_inflated_bytes,
    /// or into a temporary file in the directory that it gives.
    inflate_options inflate{};
};

namespace detail {
struct tile_cache_access;
struct reader_access;
} // namespace detail

/// Decoded tiles of compressed images (standard §10.1), kept for the reads that are given the cache, so that an image
/// read row by row, or in many small regions, decodes each of its tiles once.
///
/// A read that is given a cache looks in it for each tile it needs, and puts in it each tile it decodes, its stored
/// values as an uncompressed unit holds them: the cache holds at most capacity() bytes of them, and drops the tiles
/// used least recently to make room. A tile is the tile of one unit of one reader: the tiles of a reader that no longer
/// exists are never used again, and leave the cache as others take their room. The reader itself keeps no tile.
///
/// Thread safety: every member function can be called from any number of threads at once, and the reads of several
/// threads, of one reader or of several, can share a cache. It is neither copied nor moved.
class OPENFITS_API tile_cache
{
public:
    /// A cache of at most capacity bytes of decoded tiles; 0 keeps none.
    explicit tile_cache(std::uint64_t capacity);
    tile_cache(const tile_cache&) = delete;
    tile_cache& operator=(const tile_cache&) = delete;
    tile_cache(tile_cache&&) = delete;
    tile_cache& operator=(tile_cache&&) = delete;
    ~tile_cache();

    /// The most bytes of decoded tiles that the cache holds.
    [[nodiscard]] std::uint64_t capacity() const noexcept;

    /// The bytes of the decoded tiles that it holds.
    [[nodiscard]] std::uint64_t size() const;

    /// The number of tiles that reads found in it, and that they decoded instead, since it was made or cleared.
    [[nodiscard]] std::uint64_t hits() const noexcept;
    [[nodiscard]] std::uint64_t misses() const noexcept;

    /// Drops every tile, and sets the counts to 0.
    void clear();

private:
    friend struct detail::tile_cache_access;

    struct state;
    std::unique_ptr<state> state_;
};

/// Options of the reads of the values of an image.
///
/// The values are converted as standard §4.4.2.5 scales them: physical = BZERO + BSCALE x stored. They are exact for
/// integer data with BSCALE = 1 and an integer BZERO, the unsigned conventions of standard §5.2.5 among them, and
/// computed in double otherwise, then converted to the type of the destination: truncated toward zero, or rounded,
/// into an integer type, and checked against its range. Floating-point data read without scaling into their own type
/// keep their bits, NaN payloads and signed zeros included.
///
/// A compressed image (standard §10.1) is read tile by tile: the tiles that the region meets are read from the file in
/// few large reads, decompressed, in parallel in a build with oneTBB, and their values converted as those of an
/// uncompressed image; a quantized floating-point image gives the values that its quantization restores (standard
/// §10.2).
struct pixel_read_options
{
    /// The region of the image to read: its elements alone, in the order of their axes, the first varying fastest, an
    /// axis of a negative step in reverse. The whole image when empty.
    std::optional<openfits::region> region{};
    /// The elements from the start of one row of the destination to the start of the next, at least the length of a
    /// row of the region, so that the region lands in a larger buffer; 0 for rows that follow each other. A row holds
    /// the elements along the first axis, and the rows follow in the order of the other axes. The elements between the
    /// rows are left as they are.
    std::uint64_t pitch = 0;
    /// The scaling of the values: that of the file, none, or override_scaling.
    openfits::scaling_policy scaling = openfits::scaling_policy::apply;
    /// The scaling of scaling_policy::override.
    openfits::scaling override_scaling{};
    /// What undefined values become: BLANK in integer data, and NaN in floating-point data.
    openfits::blank_policy blank = openfits::blank_policy::nan;
    /// The value of blank_policy::value.
    double blank_value = 0.0;
    /// The mask of blank_policy::mask: a byte for each element of the destination, in the same layout; with a pitch,
    /// the bytes between the rows are left as they are. Not used by the other policies.
    std::span<std::uint8_t> mask{};
    /// What a value beyond the range of the destination does.
    openfits::overflow_policy overflow = openfits::overflow_policy::refuse;
    /// How a real value converts into an integer destination.
    openfits::rounding rounding = openfits::rounding::truncate;
    /// Verify the DATASUM of the unit (standard §4.4.2.7) when the read is of the whole image, without a region: the
    /// sum of the stored bytes is taken as they are read, in the same pass, then that of the fill of the last block,
    /// and a mismatch throws once the destination is filled. A region is not verified, nor a unit without DATASUM or
    /// with a blank one. For a compressed image, the sum is that of the data of its table, read in a pass of their own
    /// once the values are decompressed. false is the choice of an application that reads a damaged unit for what it
    /// still holds.
    bool verify_checksum = true;
    /// The decoded tiles of a compressed image that the read takes from the cache, and that it adds to it; none
    /// without one. Not used for other images.
    tile_cache* cache = nullptr;
    /// Called as the values are read, and able to cancel the read. Its units of work are the bytes of the stored values
    /// of the region, for a compressed image those of the tiles that the region meets once they are decompressed. It
    /// is called on the calling thread, also while tiles are decompressed in parallel.
    progress_function progress{};
};

/// What a read of values found.
struct pixel_read_result
{
    /// The number of values beyond the range of the destination that overflow_policy::clamp clamped.
    std::uint64_t clamped = 0;

    friend bool operator==(const pixel_read_result&, const pixel_read_result&) = default;
};

/// Options of the reads of the values of table columns (standard §7.2, §7.3).
///
/// Numbers convert as those of images do (pixel_read_options), TSCALn and TZEROn being their scaling (standard §7.3.2,
/// Eq. 7) and TNULLn the stored value of the undefined elements of integer columns; NaN is the undefined value of
/// floating-point and complex columns, and a 0 byte that of logical columns (standard §7.3.3.1). Logical columns read
/// as 1 for T and 0 for F, and bit columns as 1 and 0 for each bit. The fields of an ASCII table are read as standard
/// §7.2.5 reads them, a blank field being 0 and a field equal to TNULLn undefined.
struct column_read_options
{
    /// The rows to read; every row when empty.
    std::optional<row_range> rows{};
    /// The scaling of the values: that of the file, none, or override_scaling.
    openfits::scaling_policy scaling = openfits::scaling_policy::apply;
    /// The scaling of scaling_policy::override.
    openfits::scaling override_scaling{};
    /// What undefined elements become.
    openfits::blank_policy blank = openfits::blank_policy::nan;
    /// The value of blank_policy::value.
    double blank_value = 0.0;
    /// The mask of blank_policy::mask: a byte for each element of the destination, 1 for an undefined one; for strings,
    /// a byte for each string, of which an undefined one is empty.
    std::span<std::uint8_t> mask{};
    /// What a value beyond the range of the destination does.
    openfits::overflow_policy overflow = openfits::overflow_policy::refuse;
    /// How a real value converts into an integer destination.
    openfits::rounding rounding = openfits::rounding::truncate;
    /// Takes the trailing spaces off strings, which a field of characters keeps otherwise.
    bool trim_strings = false;
    /// Called as the values are read, and able to cancel the read. Its units of work are the bytes of the rows, and of
    /// the arrays in the heap that the rows point to.
    progress_function progress{};
};

/// What a read of the values of columns found.
struct column_read_result
{
    /// The number of values beyond the range of the destination that overflow_policy::clamp clamped.
    std::uint64_t clamped = 0;

    friend bool operator==(const column_read_result&, const column_read_result&) = default;
};

/// A destination of reader::read_columns(): a column, and the memory of the caller that its values go to, as the span
/// form of reader::read_column() takes them; with blank_policy::mask, the mask of that column.
struct column_destination
{
    std::size_t column = 0;
    element_type type = element_type::float64;
    std::span<std::byte> values{};
    std::span<std::uint8_t> mask{};

    /// The destination of the column at index column in values, as values of T.
    template <column_element T>
    [[nodiscard]] static column_destination of(std::size_t column, std::span<T> values,
                                               std::span<std::uint8_t> mask = {}) noexcept
    {
        return {.column = column, .type = element_type_of<T>(), .values = std::as_writable_bytes(values), .mask = mask};
    }
};

/// Options of the reads of the parameters of random groups (standard §6.1.2). The values convert as those of images
/// do, PSCALn and PZEROn being the scaling of each parameter.
struct parameter_read_options
{
    /// The groups to read, counted from 0; every group when empty.
    std::optional<row_range> groups{};
    /// The scaling of the values: that of each parameter, none, or override_scaling for every one.
    openfits::scaling_policy scaling = openfits::scaling_policy::apply;
    openfits::scaling override_scaling{};
    openfits::overflow_policy overflow = openfits::overflow_policy::refuse;
    openfits::rounding rounding = openfits::rounding::truncate;
};

/// Options of reader::verify(), and of verify() of a whole file (verify.h).
struct verify_options
{
    /// Called as the unit is read, and able to cancel the verification. Its units of work are the bytes of the unit
    /// that the file holds, its header and its data records; for verify() of a file, the bytes of the file.
    progress_function progress{};
};

/// A FITS file opened for reading (standard §3): its header and data units, which the reader walks when they are
/// first asked for.
///
/// The open reads the primary header and no data. A later unit is walked when a function asks for it, or for a unit
/// after it: its header read in as few reads as possible, and the data of each unit before it skipped by their size.
/// hdu_count() and hdus() walk every unit, and so does a strict open. A unit whose header gives no size stops the walk
/// there, with an error among its diagnostics, and asking for a unit after it throws that error; bytes after the last
/// unit that are not a header end the walk with a warning (standard §3.5).
///
/// Thread safety: the const member functions can be called from any number of threads at once. The walk is the only
/// state that changes after the open, under a lock, and a walked unit never changes: the references that hdu() returns
/// stay valid as long as the reader. The source is read by one thread at a time unless it supports concurrent reads.
/// A reader can be moved, after which the moved-from reader can only be destroyed or assigned.
class OPENFITS_API reader
{
public:
    /// Opens the file at path, which is UTF-8 on every platform and always the name of a file, brackets included. A
    /// file compressed whole with gzip, whatever its name, is inflated first, as gzip_source inflates it.
    /// @throws usage_error when path is empty or not valid UTF-8, and the exceptions of file_source.
    /// @throws invalid_data_error (errc::not_a_fits_file) when the file does not start with the SIMPLE keyword of a
    ///         primary header (standard §4.4.1.1), and the exceptions of a strict open.
    /// @throws limit_error when the primary header is beyond the limits; and the exceptions of gzip_source for a
    ///         gzip-compressed file.
    explicit reader(std::string_view path, const read_options& options = {});

    /// Opens the file that source holds, which the reader then owns. A source that starts with the bytes 1f 8b of a
    /// gzip member is inflated first, as gzip_source inflates it, and the reader reads the content.
    /// @throws usage_error when source is null, or gives bytes in memory of another size than its own.
    /// @throws invalid_data_error and limit_error as the open of a path, and the exceptions of the source.
    explicit reader(std::unique_ptr<input_source> source, const read_options& options = {});

    /// Opens the file of a file specification, and selects its unit and its image section, which selected_hdu() and
    /// section() give: a section without a unit selector applies to the first unit that holds an image.
    /// @throws usage_error (errc::hdu_not_found) when the file has no such unit, and (errc::invalid_file_specification)
    ///         when the section does not fit the axes of its image or the unit holds none.
    /// @throws the exceptions of the open of a path.
    explicit reader(const file_specification& specification, const read_options& options = {});

    reader(reader&& other) noexcept;
    reader& operator=(reader&& other) noexcept;
    reader(const reader&) = delete;
    reader& operator=(const reader&) = delete;
    ~reader();

    /// The number of units of the file, which walks them all.
    /// @throws the exceptions of the source.
    [[nodiscard]] std::size_t hdu_count() const;

    /// Every unit of the file, in order, which walks them all.
    /// @throws the exceptions of the source.
    [[nodiscard]] std::vector<std::reference_wrapper<const hdu_info>> hdus() const;

    /// The unit at index, 0 for the primary HDU, which walks the file as far as it.
    /// @throws usage_error (errc::hdu_not_found) when the file has fewer units.
    /// @throws invalid_data_error or limit_error, with the code of the problem that stopped the walk, when a unit
    ///         before it gives no size or is beyond the limits.
    /// @throws the exceptions of the source.
    [[nodiscard]] const hdu_info& hdu(std::size_t index) const;

    /// The first unit whose EXTNAME, or else HDUNAME, is name, compared without regard to case, and whose EXTVER, 1
    /// without it, is version, or any for 0; the primary HDU included (standard §4.4.2.6).
    /// @throws usage_error (errc::hdu_not_found) when no unit has the name and the version, and the exceptions of
    ///         hdu(index) for a walk that stops first.
    [[nodiscard]] const hdu_info& hdu(std::string_view name, std::int64_t version = 0) const;

    /// The index of the unit that hdu(name, version) gives, or nothing.
    /// @throws the exceptions of hdu(index) for a walk that stops first.
    [[nodiscard]] std::optional<std::size_t> find_hdu(std::string_view name, std::int64_t version = 0) const;

    /// The index of the first unit at or after index from that holds an image: the primary HDU, an IMAGE extension or
    /// a compressed image, with NAXIS above 0 and no NAXISn of 0; nothing when there is none. The walk goes no further
    /// than that unit, so that first_image(previous + 1) gives the next image.
    /// @throws the exceptions of hdu(index) for a walk that stops first.
    [[nodiscard]] std::optional<std::size_t> first_image(std::size_t from = 0) const;

    /// The index of the first table at or after index from, ASCII, binary or compressed; nothing when there is none.
    /// The walk goes no further than that unit.
    /// @throws the exceptions of hdu(index) for a walk that stops first.
    [[nodiscard]] std::optional<std::size_t> first_table(std::size_t from = 0) const;

    /// The units that hold an image, as first_image() takes them, by index in their order, which walks every unit.
    /// @throws the exceptions of hdu(index) for a walk that stops first.
    [[nodiscard]] std::vector<std::size_t> images() const;

    /// The indices of the tables, as first_table() takes them, in their order, which walks every unit.
    /// @throws the exceptions of hdu(index) for a walk that stops first.
    [[nodiscard]] std::vector<std::size_t> tables() const;

    /// The problems that the walk found outside the headers of the units: the ends of the file, special records, and
    /// what stopped the walk. Each unit has the diagnostics of its own header.
    [[nodiscard]] std::vector<diagnostic> diagnostics() const;

    /// The unit that a file specification selected, and 0 without one.
    [[nodiscard]] std::size_t selected_hdu() const noexcept;

    /// The image section that a file specification selected, as a region of the image of selected_hdu(); nothing
    /// without one.
    [[nodiscard]] const std::optional<region>& section() const noexcept;

    /// The size of the file, in bytes.
    [[nodiscard]] std::uint64_t size() const noexcept;

    /// Fills destination with the bytes of the data of the unit at index that start at offset, counted from the start
    /// of its data: the bytes as they are stored, big-endian, for any kind of unit, an unknown one included.
    /// @throws usage_error (errc::invalid_argument) when the bytes are not within data_size(), and the exceptions of
    ///         hdu(index); io_error (errc::end_of_data) when the file ends before them, as a truncated file does.
    void read_raw(std::size_t index, std::uint64_t offset, std::span<std::byte> destination) const;

    /// Verifies the checksums of the unit at index (standard §4.4.2.7): reads the whole unit, any kind of unit, and
    /// gives the sum of its data records, the fill of their last block included, against DATASUM, and that of the
    /// whole unit, which CHECKSUM makes negative zero, each sum valid, invalid or absent. A blank or undefined DATASUM
    /// or CHECKSUM asserts nothing and is absent, never a mismatch. A truncated file has the sums of the bytes it
    /// holds. Nothing is kept: each call reads the unit again.
    /// @throws invalid_data_error when the header of the unit gives no size for its data, and the exceptions of
    ///         hdu(index) and of the source.
    /// @throws cancelled_error when options.progress returns false.
    [[nodiscard]] checksum_report verify(std::size_t index, const verify_options& options = {}) const;

    /// The image of the unit at index, the primary HDU, an IMAGE extension or a compressed image (standard §3.3.2,
    /// §7.1, §10.1): its BITPIX, its axes, its scaling, its BLANK and its header. An image of NAXIS = 0 has no axes and
    /// no values.
    ///
    /// A compressed image is the image that it holds: ZBITPIX, ZNAXIS and ZNAXISn give its BITPIX and its axes, and its
    /// header is the header of that image as the decompression restores it (standard §10.1.2): SIMPLE, BITPIX, NAXIS,
    /// NAXISn and EXTEND, or XTENSION, BITPIX, NAXIS, NAXISn, PCOUNT and GCOUNT, from their Z keywords, then CHECKSUM
    /// and DATASUM from ZHECKSUM and ZDATASUM, then the other cards in their order, without the keywords of the table
    /// and of the compression, and without the EXTNAME = 'COMPRESSED_IMAGE' that names a compressed primary HDU.
    /// hdu(index) gives the header of the table.
    /// @throws usage_error (errc::not_an_image) for a unit that holds no image: a table, random groups or an extension
    ///         of an unknown type.
    /// @throws invalid_data_error when BITPIX, NAXIS, an NAXISn, BSCALE, BZERO or BLANK does not describe an image, or
    ///         (errc::invalid_compression) the keywords of a compressed image its compression; unsupported_error
    ///         (errc::unsupported_compression) for an algorithm that is not one of standard Table 36; and the
    ///         exceptions of hdu(index).
    [[nodiscard]] openfits::image_info image(std::size_t index) const;

    /// Reads the values of the image at index, or of the region of options, as values of T, in memory that the call
    /// allocates: the values of the region, or with options.pitch the elements that its rows span, with zeros between
    /// the rows.
    /// @throws limit_error (errc::allocation_too_large) when they take more than limits::max_allocation; and the
    ///         exceptions of the other read_pixels().
    template <numeric_value T>
    [[nodiscard]] std::vector<T> read_pixels(std::size_t index, const pixel_read_options& options = {}) const
    {
        std::vector<T> values(destination_size(index, options, sizeof(T)));
        (void)read_pixels(index, std::span<T>(values), options);
        return values;
    }

    /// Reads the values of the image at index, or of the region of options, into destination as values of T:
    /// exactly as many as the region has, or with options.pitch at least those from the first element of its first
    /// row to the last of its last row, of which it fills the rows alone. The read allocates besides at most a staging
    /// chunk of a few MiB, and reads the stored values in runs: a region whose values are a large part of the bytes
    /// that hold them is read in chunks of those bytes, and one whose rows lie far apart row by row, so that a region
    /// costs reads in proportion to its size. When it throws, destination may hold part of the values.
    /// @throws usage_error (errc::invalid_argument) when the region, the pitch, destination or the mask do not fit the
    ///         image, or options give a value for undefined values that T cannot hold, or a scaling of 0; and the
    ///         exceptions of image(index).
    /// @throws invalid_data_error with errc::value_out_of_range for a value beyond the range of T when options do not
    ///         clamp it, errc::undefined_value for a NaN that an integer T holds only through a policy, and the code of
    ///         the problem of the header when the data of the unit are unavailable, such as BSCALE = 0 for physical
    ///         values.
    /// @throws io_error (errc::end_of_data) when the file ends before the values, as a truncated file does, and the
    ///         exceptions of the source.
    /// @throws integrity_error (errc::checksum_mismatch) when a read of the whole image finds that the data do not
    ///         match DATASUM, once destination holds the values (pixel_read_options::verify_checksum).
    /// @throws cancelled_error when options.progress returns false.
    template <numeric_value T>
    pixel_read_result read_pixels(std::size_t index, std::span<T> destination,
                                  const pixel_read_options& options = {}) const
    {
        return read_values(index, numeric_type_of<T>(), std::as_writable_bytes(destination), options);
    }

    /// Reads the values of the image at index into destination as values of type, as read_pixels<T>() reads them, for
    /// a caller that knows the type at run time only: destination holds values of type in the byte order of the host,
    /// at an address that is a multiple of their size.
    /// @throws usage_error (errc::invalid_argument) when the size or the address of destination is not a multiple of
    ///         the size of the values of type; and the exceptions of read_pixels<T>().
    pixel_read_result read_pixels(std::size_t index, numeric_type type, std::span<std::byte> destination,
                                  const pixel_read_options& options = {}) const;

    /// The table of the unit at index, an ASCII or a binary table (standard §7.2, §7.3): its columns, its rows, its
    /// heap and its header.
    ///
    /// A compressed table (standard §10.3) is the binary table that it holds: ZNAXIS1, ZNAXIS2, ZPCOUNT, ZTHEAP and
    /// ZFORMn give its rows, its heap and the formats of its columns, and its header is that of the table as the
    /// decompression restores it, with TFORMn, NAXIS1, NAXIS2, PCOUNT, THEAP, CHECKSUM and DATASUM from their Z
    /// keywords, without the keywords of the compression. Its columns are read as those of any table: the tiles of the
    /// rows read are decompressed first. hdu(index) gives the header of the compressed table.
    /// @throws usage_error (errc::not_a_table) for a unit that holds no table: an image, random groups or an extension
    ///         of an unknown type.
    /// @throws unsupported_error (errc::unsupported_compression) for an algorithm of a compressed table that is not one
    ///         of standard §10.3.5.
    /// @throws invalid_data_error, with the code of the first problem, when the header does not describe a table: a
    ///         TFIELDS or a TFORMn missing or of no form of the standard, fields that do not fit the rows, a THEAP
    ///         before the end of the rows; and the exceptions of hdu(index).
    [[nodiscard]] openfits::table_info table(std::size_t index) const;

    /// Reads the elements of a column of the table at index hdu, in the rows of options, as values of T, in memory that
    /// the call allocates: the elements of the field of each row, one row after the other, those of a bit column one
    /// for each bit.
    /// @throws limit_error (errc::allocation_too_large) when they take more than limits::max_allocation; and the
    ///         exceptions of the other read_column().
    template <column_element T>
    [[nodiscard]] std::vector<T> read_column(std::size_t hdu, std::size_t column,
                                             const column_read_options& options = {}) const
    {
        std::vector<T> values(column_size(hdu, column, options, sizeof(T)));
        (void)read_column(hdu, column, std::span<T>(values), options);
        return values;
    }

    /// Reads the elements of a column into destination as values of T: exactly the elements of the field of each row of
    /// options. The rows are read in chunks, from which the fields are gathered and converted in one pass, and a
    /// column whose fields lie far apart is read field by field. When it throws, destination may hold part of the
    /// values.
    /// @throws usage_error (errc::column_not_found) when the table has no such column, (errc::column_type) when T is
    ///         not a type the column converts to: numbers for every column but characters of binary tables and complex
    ///         columns, std::complex<float> and std::complex<double> for complex columns, and (errc::invalid_argument)
    ///         when the rows lie beyond the table, destination or the mask do not have the elements of the rows, the
    ///         column holds variable-length arrays, which read_arrays() reads, or options give a value for undefined
    ///         values that T cannot hold, or a scaling of 0; and the exceptions of table(hdu).
    /// @throws invalid_data_error with errc::value_out_of_range for a value beyond the range of T when options do not
    ///         clamp it, errc::undefined_value for an undefined value that T holds only through a policy, and
    ///         errc::invalid_field for a field that is not a value of its type.
    /// @throws io_error (errc::end_of_data) when the file ends before the rows, and the exceptions of the source.
    /// @throws cancelled_error when options.progress returns false.
    template <column_element T>
    column_read_result read_column(std::size_t hdu, std::size_t column, std::span<T> destination,
                                   const column_read_options& options = {}) const
    {
        const column_destination target = column_destination::of(column, destination, options.mask);
        return read_columns(hdu, std::span<const column_destination>(&target, 1), options);
    }

    /// read_column() of the first column named name, compared as table_info::find_column() compares names.
    template <column_element T>
    [[nodiscard]] std::vector<T> read_column(std::size_t hdu, std::string_view name,
                                             const column_read_options& options = {}) const
    {
        return read_column<T>(hdu, column_index(hdu, name), options);
    }

    template <column_element T>
    column_read_result read_column(std::size_t hdu, std::string_view name, std::span<T> destination,
                                   const column_read_options& options = {}) const
    {
        return read_column(hdu, column_index(hdu, name), destination, options);
    }

    /// Reads several columns of the table at index hdu in one pass over the rows of options, each into its destination
    /// as read_column() does, with the mask of each destination for blank_policy::mask.
    /// @throws the exceptions of read_column().
    column_read_result read_columns(std::size_t hdu, std::span<const column_destination> columns,
                                    const column_read_options& options = {}) const;

    /// Reads a column as strings, in the rows of options: for characters, a string for each field up to its first NUL
    /// byte (standard §7.3.3.1), or for each substring of a column with dimensions, or the whole array of a
    /// variable-length column; for other columns, each element as its TDISPn displays it (standard §7.3.4), or by a
    /// default format: I4, I6, I11 and I20 for B, I, J and K, G14.6 and G23.15 for E and D and for scaled integers of
    /// up to 16 bits and beyond, B8.8 for each byte of bits, L1 for logicals, E14.6 and E23.15 for the parts of complex
    /// values, and the TFORMn of an ASCII table. An undefined element is an empty string.
    /// @throws usage_error (errc::column_type) for a column of variable-length arrays of numbers, which read_arrays()
    ///         reads; and the exceptions of read_column().
    [[nodiscard]] std::vector<std::string> read_strings(std::size_t hdu, std::size_t column,
                                                        const column_read_options& options = {}) const;

    [[nodiscard]] std::vector<std::string> read_strings(std::size_t hdu, std::string_view name,
                                                        const column_read_options& options = {}) const
    {
        return read_strings(hdu, column_index(hdu, name), options);
    }

    /// Reads the arrays of the cells of a column in the rows of options as values of T: those of a column of
    /// variable-length arrays (standard §7.3.5), whose descriptors are read first, checked against the heap and
    /// sorted, so that the heap is read in ranges, and the fields of any other column, which have as many elements in
    /// every row. With blank_policy::mask, the mask of the result marks the undefined values, and options.mask is not
    /// used.
    /// @throws invalid_data_error (errc::invalid_descriptor) for a descriptor that holds a negative value or points
    ///         outside the heap; limit_error (errc::allocation_too_large) when the arrays take more than
    ///         limits::max_allocation, and (errc::array_too_large) when the heap is larger than limits::max_heap_bytes
    ///         or an array has more elements than limits::max_vla_elements; and the exceptions of read_column().
    template <column_element T>
    [[nodiscard]] column_arrays<T> read_arrays(std::size_t hdu, std::size_t column,
                                               const column_read_options& options = {}) const
    {
        column_arrays<T> result;
        read_cell_arrays(
            hdu, column, element_type_of<T>(), options,
            [&result](std::uint64_t elements) {
                result.values.resize(elements);
                return std::as_writable_bytes(std::span<T>(result.values));
            },
            result.offsets, result.mask);
        return result;
    }

    template <column_element T>
    [[nodiscard]] column_arrays<T> read_arrays(std::size_t hdu, std::string_view name,
                                               const column_read_options& options = {}) const
    {
        return read_arrays<T>(hdu, column_index(hdu, name), options);
    }

    /// The random groups of the unit at index, the primary HDU (standard §6): their parameters, the axes of their
    /// arrays, their scaling and their number.
    /// @throws usage_error (errc::not_random_groups) for a unit that holds no random groups.
    /// @throws invalid_data_error when BITPIX, NAXIS, an NAXISn, PCOUNT, GCOUNT, BSCALE, BZERO, BLANK, a PSCALn or a
    ///         PZEROn does not describe them; limit_error (errc::allocation_too_large) when the descriptions of the
    ///         PCOUNT parameters take more than limits::max_allocation; and the exceptions of hdu(index).
    [[nodiscard]] openfits::groups_info groups(std::size_t index) const;

    /// Reads the parameter at index parameter, counted from 0, of the groups of options, as values of T: one for each
    /// group, with its PSCALn and PZEROn.
    /// @throws usage_error (errc::invalid_argument) when the groups have no such parameter or lie beyond the groups;
    ///         and the exceptions of groups(hdu) and of read_pixels().
    template <numeric_value T>
    [[nodiscard]] std::vector<T> read_parameter(std::size_t hdu, std::size_t parameter,
                                                const parameter_read_options& options = {}) const
    {
        std::vector<T> values(group_range_size(hdu, options, sizeof(T)));
        read_parameter_values(hdu, parameter, {}, numeric_type_of<T>(), std::as_writable_bytes(std::span<T>(values)),
                              options);
        return values;
    }

    /// Reads the parameter named name, compared without regard to case and to trailing spaces, of the groups of
    /// options as values of T, one for each group: the sum of the physical values of every parameter of that name, as
    /// standard §6.1.2 and §6.3 make a parameter of more precision than the stored type, computed in double.
    /// @throws usage_error (errc::invalid_argument) when no parameter has the name, and the exceptions of the other
    ///         read_parameter().
    template <numeric_value T>
    [[nodiscard]] std::vector<T> read_parameter(std::size_t hdu, std::string_view name,
                                                const parameter_read_options& options = {}) const
    {
        std::vector<T> values(group_range_size(hdu, options, sizeof(T)));
        read_parameter_values(hdu, 0, name, numeric_type_of<T>(), std::as_writable_bytes(std::span<T>(values)),
                              options);
        return values;
    }

    /// Reads the array of the group at index group, counted from 0, or the region of options, as read_pixels() reads
    /// an image, in memory that the call allocates.
    /// @throws usage_error (errc::invalid_argument) when the unit has no such group, and the exceptions of
    ///         read_pixels().
    template <numeric_value T>
    [[nodiscard]] std::vector<T> read_group(std::size_t hdu, std::uint64_t group,
                                            const pixel_read_options& options = {}) const
    {
        std::vector<T> values(group_destination_size(hdu, options, sizeof(T)));
        (void)read_group(hdu, group, std::span<T>(values), options);
        return values;
    }

    /// Reads the array of the group at index group, or the region of options, into destination, as read_pixels() reads
    /// an image.
    template <numeric_value T>
    pixel_read_result read_group(std::size_t hdu, std::uint64_t group, std::span<T> destination,
                                 const pixel_read_options& options = {}) const
    {
        return read_group_values(hdu, group, numeric_type_of<T>(), std::as_writable_bytes(destination), options);
    }

private:
    // The number of values of T that a read of options into memory of the library allocates, within
    // limits::max_allocation.
    [[nodiscard]] std::size_t destination_size(std::size_t index, const pixel_read_options& options,
                                               std::size_t value_size) const;

    // The read of the values of the image at index into destination, as values of type.
    pixel_read_result read_values(std::size_t index, numeric_type type, std::span<std::byte> destination,
                                  const pixel_read_options& options) const;

    // The index of the first column of the table at index hdu named name.
    [[nodiscard]] std::size_t column_index(std::size_t hdu, std::string_view name) const;

    // The number of elements of a read of options of the column, within limits::max_allocation for values of
    // value_size bytes.
    [[nodiscard]] std::size_t column_size(std::size_t hdu, std::size_t column, const column_read_options& options,
                                          std::size_t value_size) const;

    // The read of read_arrays(): allocate gives the bytes of the values of a number of elements, and offsets receives
    // the offsets of the rows.
    void read_cell_arrays(std::size_t hdu, std::size_t column, element_type type, const column_read_options& options,
                          const std::function<std::span<std::byte>(std::uint64_t)>& allocate,
                          std::vector<std::uint64_t>& offsets, std::vector<std::uint8_t>& mask) const;

    // The number of groups of options, within limits::max_allocation for values of value_size bytes.
    [[nodiscard]] std::size_t group_range_size(std::size_t hdu, const parameter_read_options& options,
                                               std::size_t value_size) const;

    // The read of read_parameter(): of the parameter at index parameter, or of those named name when it is not empty.
    void read_parameter_values(std::size_t hdu, std::size_t parameter, std::string_view name, numeric_type type,
                               std::span<std::byte> destination, const parameter_read_options& options) const;

    // The number of values of the region of options of the array of a group, within limits::max_allocation.
    [[nodiscard]] std::size_t group_destination_size(std::size_t hdu, const pixel_read_options& options,
                                                     std::size_t value_size) const;

    pixel_read_result read_group_values(std::size_t hdu, std::uint64_t group, numeric_type type,
                                        std::span<std::byte> destination, const pixel_read_options& options) const;

    friend struct detail::reader_access;

    struct state;
    std::unique_ptr<state> state_;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace openfits
