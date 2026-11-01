// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/export.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

/// @file
/// Plain types that the rest of the API shares: the kinds of header and data units, the types of array values and their
/// scaling, the shapes and regions of arrays, and the policies of conversions.

namespace openfits {

/// The kind of a header and data unit.
enum class hdu_kind : std::uint8_t
{
    primary,          ///< The primary HDU, with or without a data array (standard §3.3).
    image,            ///< An IMAGE extension (standard §7.1).
    table,            ///< An ASCII-table extension, TABLE (standard §7.2).
    bintable,         ///< A binary-table extension, BINTABLE (standard §7.3).
    compressed_image, ///< A binary table that holds a tiled compressed image, ZIMAGE = T (standard §10.1).
    compressed_table, ///< A binary table that holds a tiled compressed table, ZTABLE = T (standard §10.3).
    random_groups,    ///< A primary HDU of random groups, GROUPS = T with NAXIS1 = 0 (standard §6).
    unknown,          ///< A conforming extension of another type, such as those of Appendix F, read as bytes.
};

/// A region of an array: a box of elements on every axis, with a step on each, in the order of the axes of FITS, the
/// first varying fastest.
///
/// On each axis the box holds size elements from the index start, counted from 0. A step of n takes every n-th of
/// them: from the first when n is positive, and from the last, the axis traversed in reverse, when n is negative. An
/// empty step means 1 on every axis; a step of 0 is invalid.
struct region
{
    std::vector<std::uint64_t> start{};
    std::vector<std::uint64_t> size{};
    std::vector<std::int64_t> step{};

    friend bool operator==(const region&, const region&) = default;
};

/// The type of the values of an array as a file stores them, which the BITPIX keyword gives (standard §4.4.1.1, §5):
/// the enumerators have the values of the keyword.
enum class bitpix : std::int8_t
{
    uint8 = 8,     ///< 8-bit unsigned integers (standard §5.2.1).
    int16 = 16,    ///< 16-bit two's complement integers (standard §5.2.2).
    int32 = 32,    ///< 32-bit two's complement integers (standard §5.2.3).
    int64 = 64,    ///< 64-bit two's complement integers (standard §5.2.4).
    float32 = -32, ///< IEEE 754 32-bit floating-point numbers (standard §5.3).
    float64 = -64, ///< IEEE 754 64-bit floating-point numbers (standard §5.3).
};

/// The C++ types that the values of arrays are read into and written from.
enum class numeric_type : std::uint8_t
{
    int8,    ///< std::int8_t
    uint8,   ///< std::uint8_t
    int16,   ///< std::int16_t
    uint16,  ///< std::uint16_t
    int32,   ///< std::int32_t
    uint32,  ///< std::uint32_t
    int64,   ///< std::int64_t
    uint64,  ///< std::uint64_t
    float32, ///< float
    float64, ///< double
};

/// The types that numeric_type names: the integer types of 8, 16, 32 and 64 bits, signed and unsigned, but bool and the
/// character types, such as std::uint16_t, long or unsigned long long; float; and double. Each integer type stands for
/// the numeric_type of its size and signedness.
template <typename T>
concept numeric_value =
    std::same_as<T, std::remove_cv_t<T>> &&
    (std::same_as<T, float> || std::same_as<T, double> ||
     (std::integral<T> && !std::same_as<T, bool> && !std::same_as<T, char> && !std::same_as<T, wchar_t> &&
      !std::same_as<T, char8_t> && !std::same_as<T, char16_t> && !std::same_as<T, char32_t> &&
      (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8)));

/// The numeric_type of T.
template <numeric_value T> [[nodiscard]] constexpr numeric_type numeric_type_of() noexcept
{
    if constexpr (std::same_as<T, float>) {
        return numeric_type::float32;
    } else if constexpr (std::same_as<T, double>) {
        return numeric_type::float64;
    } else if constexpr (sizeof(T) == 1) {
        return std::is_signed_v<T> ? numeric_type::int8 : numeric_type::uint8;
    } else if constexpr (sizeof(T) == 2) {
        return std::is_signed_v<T> ? numeric_type::int16 : numeric_type::uint16;
    } else if constexpr (sizeof(T) == 4) {
        return std::is_signed_v<T> ? numeric_type::int32 : numeric_type::uint32;
    } else {
        return std::is_signed_v<T> ? numeric_type::int64 : numeric_type::uint64;
    }
}

/// The size of the values of a numeric type, in bytes.
[[nodiscard]] constexpr std::size_t size_of(numeric_type type) noexcept
{
    switch (type) {
    case numeric_type::int8:
    case numeric_type::uint8:
        return 1;
    case numeric_type::int16:
    case numeric_type::uint16:
        return 2;
    case numeric_type::int32:
    case numeric_type::uint32:
    case numeric_type::float32:
        return 4;
    case numeric_type::int64:
    case numeric_type::uint64:
    case numeric_type::float64:
        return 8;
    }
    return 0;
}

/// The shape of an array: the length of each axis, NAXIS1 first, the axis whose index varies fastest in the data
/// (standard §3.3.2). An array without axes, NAXIS = 0, holds no data.
struct geometry
{
    std::vector<std::uint64_t> axes{};

    /// The number of elements: the product of the lengths, and 0 without axes.
    /// @throws usage_error (errc::arithmetic_overflow) when it does not fit in 64 bits.
    [[nodiscard]] OPENFITS_API std::uint64_t element_count() const;

    friend bool operator==(const geometry&, const geometry&) = default;
};

/// The linear scaling of stored values into the physical values that they represent: physical = zero + scale x stored
/// (standard §4.4.2.5, Eq. 3), BZERO and BSCALE for an array.
struct scaling
{
    double scale = 1.0;
    double zero = 0.0;

    friend bool operator==(const scaling&, const scaling&) = default;
};

/// What a read does with the scaling of the values it reads.
enum class scaling_policy : std::uint8_t
{
    apply,    ///< The physical values, from the scaling of the file.
    raw,      ///< The stored values, as the file holds them.
    override, ///< The values that a scaling of the caller's gives.
};

/// What a read gives for undefined values: those of integer data equal to BLANK, and NaN in floating-point data
/// (standard §4.4.2.5, §5.3).
enum class blank_policy : std::uint8_t
{
    /// NaN in a floating-point destination. An integer destination has no undefined value: it gets BLANK converted as
    /// any stored value, and a NaN of floating-point data is refused there with invalid_data_error
    /// (errc::undefined_value).
    nan,
    /// A value of the caller's, converted to the type of the destination.
    value,
    /// A mask alongside the destination marks them with 1, and the other values with 0; the destination holds NaN for
    /// them, or 0 when it holds integers.
    mask,
};

/// What a conversion does with a value beyond the range of its destination.
enum class overflow_policy : std::uint8_t
{
    refuse, ///< Fails with invalid_data_error (errc::value_out_of_range).
    clamp,  ///< Gives the nearest value of the range, and counts it.
};

/// How a conversion of a real value into an integer rounds.
enum class rounding : std::uint8_t
{
    truncate, ///< Toward zero.
    nearest,  ///< To the nearest integer, and half away from zero.
};

} // namespace openfits
