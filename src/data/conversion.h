// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/types.h>

#include <cstddef>
#include <cstdint>
#include <optional>

// The conversions of array values between the types that a file stores (standard §5) and those of the application: the
// scaling of standard §4.4.2.5 (Eq. 3), the unsigned conventions of standard §5.2.5, and the undefined values, BLANK
// and NaN. Stored values are big-endian; the values of the application are in the byte order of the host.
//
// Physical values are those of the scaling: exact for integer data with BSCALE = 1 and an integer BZERO, the unsigned
// conventions among them, and computed in double otherwise, as zero + scale x stored with the product rounded before
// the sum, on every host. A read into an integer type truncates toward zero or rounds half away from zero; a write into
// integer data rounds half away from zero.

namespace openfits::detail {

/// The numeric type of the values that a BITPIX stores.
[[nodiscard]] numeric_type stored_type(bitpix value) noexcept;

/// The size of the values that a BITPIX stores, in bytes: |BITPIX| / 8.
[[nodiscard]] std::size_t stored_size(bitpix value) noexcept;

/// The BITPIX of a keyword value, or nothing for a value that is not one of standard §4.4.1.1 (Table 8).
[[nodiscard]] std::optional<bitpix> bitpix_of(std::int64_t value) noexcept;

/// How the physical values of a conversion follow from the stored ones.
enum class arithmetic : std::uint8_t
{
    identity,        ///< They are the stored values.
    sign_flip,       ///< The offsets of standard §5.2.5, Table 11: the stored value with its most significant bit
                     ///< flipped, which adds 2^(n - 1) to n-bit integers and subtracts 128 from bytes.
    signed_offset,   ///< stored + offset, exact in 64-bit signed integers for every stored value.
    unsigned_offset, ///< stored + offset, exact in 64-bit unsigned integers for every stored value: the 64-bit
                     ///< unsigned convention, BZERO = 2^63.
    scaled,          ///< zero + scale x stored, in double.
};

/// A conversion of the values of a read, prepared once for every value of the read.
struct read_conversion
{
    bitpix from = bitpix::uint8;
    numeric_type to = numeric_type::uint8;
    detail::arithmetic arithmetic = detail::arithmetic::identity;
    double scale = 1.0;
    double zero = 0.0;
    /// The offset of the offset arithmetics, as a signed or an unsigned 64-bit integer.
    std::int64_t offset = 0;
    std::uint64_t unsigned_offset = 0;
    /// BLANK, for integer data.
    std::optional<std::int64_t> blank{};
    blank_policy policy = blank_policy::nan;
    /// The value of blank_policy::value, which converts to the destination type.
    double blank_value = 0.0;
    overflow_policy overflow = overflow_policy::refuse;
    openfits::rounding rounding = openfits::rounding::truncate;
    /// True when a value can fall outside the range of the destination, which each value is then checked against.
    bool checks_range = false;
    /// True when undefined values are looked for: BLANK, when integer data have one and the policy or the destination
    /// makes them other than any value, and NaN, when an integer destination or the policy needs it.
    bool checks_undefined = false;
    /// The unit, for the context of errors.
    std::uint64_t hdu = 0;
};

/// Prepares the conversion of the values of data of BITPIX from, with their scaling and BLANK, into values of type to.
/// @throws usage_error (errc::invalid_argument) when policy is blank_policy::value and blank_value does not convert
///         to type to: a value that is not an integer, or beyond the range, for an integer type.
[[nodiscard]] read_conversion make_read_conversion(bitpix from, numeric_type to, const scaling& values,
                                                   std::optional<std::int64_t> blank, blank_policy policy,
                                                   double blank_value, overflow_policy overflow,
                                                   openfits::rounding rounding, std::uint64_t hdu);

/// Converts count stored values, big-endian, the first at in and each next one stride values further, backwards for a
/// negative stride, into count values of the destination type at out, one after the other, in the byte order of the
/// host. With blank_policy::mask, mask gets 1 for each undefined value and 0 for the others. Returns the number of
/// values clamped.
///
/// With a stride of 1 and a destination type wider than the stored one, the stored values may lie at the end of the
/// bytes of the values at out, which they then end with: each value is read before any value written reaches it.
/// @throws invalid_data_error with errc::value_out_of_range for a value beyond the range of the destination when the
///         overflow policy refuses it, and errc::undefined_value for a NaN that an integer destination cannot hold.
std::uint64_t convert_read(const read_conversion& conversion, const std::byte* in, std::ptrdiff_t stride,
                           std::byte* out, std::uint8_t* mask, std::size_t count);

/// convert_read() in place, for a destination type of the size of the stored one: data holds count stored values, and
/// the values of the destination type when it returns.
std::uint64_t convert_read_in_place(const read_conversion& conversion, std::byte* data, std::uint8_t* mask,
                                    std::size_t count);

/// A conversion of the values of an application into the values that a file stores, prepared once for every value.
struct write_conversion
{
    numeric_type from = numeric_type::uint8;
    bitpix to = bitpix::uint8;
    detail::arithmetic arithmetic = detail::arithmetic::identity;
    double scale = 1.0;
    double zero = 0.0;
    /// The offset of the offset arithmetics, subtracted from each value.
    std::int64_t offset = 0;
    std::uint64_t unsigned_offset = 0;
    /// The stored value of NaN in integer data; without one a NaN is refused there.
    std::optional<std::int64_t> blank{};
    overflow_policy overflow = overflow_policy::refuse;
    /// True when a value can fall outside the range of the stored type.
    bool checks_range = false;
    /// True when the values can be NaN, which integer data store as BLANK.
    bool checks_undefined = false;
    std::uint64_t hdu = 0;
};

/// Prepares the conversion of values of type from into data of BITPIX to, with the scaling whose physical values they
/// are and the BLANK of the data.
[[nodiscard]] write_conversion make_write_conversion(numeric_type from, bitpix to, const scaling& values,
                                                     std::optional<std::int64_t> blank, overflow_policy overflow,
                                                     std::uint64_t hdu);

/// Converts count values of the source type, in the byte order of the host, at in into count stored values, big-endian,
/// at out. Returns the number of values clamped.
/// @throws invalid_data_error with errc::value_out_of_range for a value beyond the range of the stored type when the
///         overflow policy refuses it, and errc::undefined_value for a NaN in integer data without BLANK.
std::uint64_t convert_write(const write_conversion& conversion, const std::byte* in, std::byte* out, std::size_t count);

} // namespace openfits::detail
