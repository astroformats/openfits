// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/error.h>
#include <openfits/export.h>
#include <openfits/hdu.h>
#include <openfits/image.h>
#include <openfits/io.h>
#include <openfits/limits.h>
#include <openfits/types.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

/// @file
/// Reading FITS files: the header and data units, walked as they are asked for, and the values of their images.

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
    /// file unavailable, which a diagnostic of severity error reports otherwise.
    bool strict = false;
};

/// Options of the reads of the values of an image.
///
/// The values are converted as standard §4.4.2.5 scales them: physical = BZERO + BSCALE x stored. They are exact for
/// integer data with BSCALE = 1 and an integer BZERO, the unsigned conventions of standard §5.2.5 among them, and
/// computed in double otherwise, then converted to the type of the destination: truncated toward zero, or rounded,
/// into an integer type, and checked against its range. Floating-point data read without scaling into their own type
/// keep their bits, NaN payloads and signed zeros included.
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
    /// Called as the values are read, and able to cancel the read. Its units of work are the bytes of the stored values
    /// of the region.
    progress_function progress{};
};

/// What a read of values found.
struct pixel_read_result
{
    /// The number of values beyond the range of the destination that overflow_policy::clamp clamped.
    std::uint64_t clamped = 0;

    friend bool operator==(const pixel_read_result&, const pixel_read_result&) = default;
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
    /// Opens the file at path, which is UTF-8 on every platform and always the name of a file, brackets included.
    /// @throws usage_error when path is empty or not valid UTF-8, and the exceptions of file_source.
    /// @throws invalid_data_error (errc::not_a_fits_file) when the file does not start with the SIMPLE keyword of a
    ///         primary header (standard §4.4.1.1), and the exceptions of a strict open.
    /// @throws limit_error when the primary header is beyond the limits.
    explicit reader(std::string_view path, read_options options = {});

    /// Opens the file that source holds, which the reader then owns.
    /// @throws usage_error when source is null, or gives bytes in memory of another size than its own.
    /// @throws invalid_data_error and limit_error as the open of a path, and the exceptions of the source.
    explicit reader(std::unique_ptr<input_source> source, read_options options = {});

    /// Opens the file of a file specification, and selects its unit and its image section, which selected_hdu() and
    /// section() give: a section without a unit selector applies to the first unit that holds an image.
    /// @throws usage_error (errc::hdu_not_found) when the file has no such unit, and (errc::invalid_file_specification)
    ///         when the section does not fit the axes of its image or the unit holds none.
    /// @throws the exceptions of the open of a path.
    explicit reader(const file_specification& specification, read_options options = {});

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

    /// The index of the first unit that holds an image: the primary HDU, an IMAGE extension or a compressed image,
    /// with NAXIS above 0 and no NAXISn of 0; nothing when there is none.
    /// @throws the exceptions of hdu(index) for a walk that stops first.
    [[nodiscard]] std::optional<std::size_t> first_image() const;

    /// The index of the first table, ASCII, binary or compressed; nothing when there is none.
    /// @throws the exceptions of hdu(index) for a walk that stops first.
    [[nodiscard]] std::optional<std::size_t> first_table() const;

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

    /// The image of the unit at index, the primary HDU or an IMAGE extension (standard §3.3.2, §7.1): its BITPIX, its
    /// axes, its scaling, its BLANK and its header. An image of NAXIS = 0 has no axes and no values.
    /// @throws usage_error (errc::not_an_image) for a unit that holds no image: a table, random groups or an extension
    ///         of an unknown type.
    /// @throws unsupported_error (errc::unsupported_compression) for a tiled compressed image (standard §10.1), which
    ///         this version does not decompress.
    /// @throws invalid_data_error when BITPIX, NAXIS, an NAXISn, BSCALE, BZERO or BLANK does not describe an image, and
    ///         the exceptions of hdu(index).
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
    /// @throws cancelled_error when options.progress returns false.
    template <numeric_value T>
    pixel_read_result read_pixels(std::size_t index, std::span<T> destination,
                                  const pixel_read_options& options = {}) const
    {
        return read_values(index, numeric_type_of<T>(), std::as_writable_bytes(destination), options);
    }

private:
    // The number of values of T that a read of options into memory of the library allocates, within
    // limits::max_allocation.
    [[nodiscard]] std::size_t destination_size(std::size_t index, const pixel_read_options& options,
                                               std::size_t value_size) const;

    // The read of the values of the image at index into destination, as values of type.
    pixel_read_result read_values(std::size_t index, numeric_type type, std::span<std::byte> destination,
                                  const pixel_read_options& options) const;

    struct state;
    std::unique_ptr<state> state_;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace openfits
