// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/export.h>
#include <openfits/header.h>
#include <openfits/image.h>
#include <openfits/io.h>
#include <openfits/types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

/// @file
/// Writing FITS files: headers and images.

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
    /// What a save to a path does when the file exists.
    openfits::if_exists if_exists = openfits::if_exists::replace;
    /// Flush the file to the storage device before it replaces the target, so that it survives a power failure.
    /// Slower.
    bool flush_to_disk = false;
    /// Called as the file is written, and able to cancel the save. Its units of work are the bytes of the file.
    progress_function progress{};
};

/// Options of an image of the writer.
struct image_write_options
{
    /// Blank cards before END: room that later changes of the header take in place (standard §4.4.2.4).
    std::size_t reserved_cards = 0;
    /// What a value beyond the range of the stored type does.
    openfits::overflow_policy overflow = openfits::overflow_policy::refuse;
};

#if defined(_MSC_VER)
// The writer holds a standard-library member, which cl reports for an exported class. A C++ interface requires the
// same standard library on both sides of the DLL boundary anyway.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

/// Writes FITS files: a primary HDU, the first unit added, and IMAGE extensions after it (standard §3.3, §7.1).
///
/// Each unit gets the structural keywords that its model gives, which the writer makes itself: SIMPLE, BITPIX, NAXIS,
/// NAXISn and EXTEND = T for the primary HDU, XTENSION = 'IMAGE', BITPIX, NAXIS, NAXISn, PCOUNT and GCOUNT for an
/// extension, then BSCALE, BZERO and BLANK when they are not the defaults; and the cards of its header after them, as
/// they are, none of them structural (is_structural()). The data are written in big-endian order through a staging
/// chunk, and the last block of each unit is filled with zeros (standard §3.3.2).
///
/// The values of an image are borrowed: they must stay valid and unchanged until the last save(), which never modifies
/// them. save() formats every header before it writes anything, and throws validation_error, naming the unit and the
/// keyword, for a header that the standard forbids. A writer is thread-compatible, like a standard container: save()
/// can run on several threads at once, and a change needs exclusive access. It can be moved, and a moved-from writer
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
    /// NaN is stored as info.blank in integer data.
    /// @throws usage_error (errc::invalid_argument) when data does not hold the values of the axes, or a scaling is
    ///         given without info.bitpix; (errc::arithmetic_overflow) when the data do not fit in 64 bits.
    /// @throws validation_error for an image that the standard forbids: more than 999 axes, a BSCALE of 0 or a scaling
    ///         that is not finite, or a BLANK with floating-point data or beyond the range of the stored type.
    template <numeric_value T>
    std::size_t add_image(image_info info, std::span<const T> data, image_write_options options = {})
    {
        return add_values(std::move(info), numeric_type_of<T>(), std::as_bytes(data), options);
    }

    /// Adds an image with values that are not const, such as std::span(values) of a std::vector that is not const, as
    /// the overload for const values does.
    template <numeric_value T>
    std::size_t add_image(image_info info, std::span<T> data, image_write_options options = {})
    {
        return add_image(std::move(info), std::span<const T>(data), options);
    }

    /// Adds a unit of a header alone, without data, after the units added so far: the primary HDU with BITPIX = 8 and
    /// NAXIS = 0 when it is the first, as files of extensions begin, and an IMAGE extension of NAXIS = 0 otherwise.
    /// reserved_cards blank cards come before END. Returns the index of the unit.
    std::size_t add_header(openfits::header cards, std::size_t reserved_cards = 0);

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
    /// the sink, which pass through unchanged. The sink needs no rewrite(). A failure may leave part of the file in it.
    void save(output_sink& sink, const write_options& options = {}) const;

private:
    std::size_t add_values(image_info info, numeric_type type, std::span<const std::byte> data,
                           const image_write_options& options);

    struct state;
    std::unique_ptr<state> state_;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace openfits
