// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/export.h>
#include <openfits/header.h>
#include <openfits/types.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/// @file
/// Images: the arrays of primary HDUs and IMAGE extensions (standard §3.3.2, §7.1), and random groups (standard §6).

namespace openfits {

#if defined(_MSC_VER)
// The members hold standard-library types, which cl reports for an exported class. A C++ interface requires the same
// standard library on both sides of the DLL boundary anyway.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

/// An image: the array of a primary HDU or of an IMAGE extension (standard §3.3.2, §7.1), as a reader describes it and
/// as the writer takes it.
///
/// A reader gives every member: the BITPIX of the data, the axes, the scaling of BSCALE and BZERO, BLANK for integer
/// data, and the header of the unit, its structural keywords included. A model for the writer gives the axes and the
/// cards of the header, none of them structural (without_structural() takes them out of a header read from a file);
/// bitpix, scaling and blank say how the values are stored when they are not stored in the type of the data given
/// (writer::add_image()).
///
/// A value: copies are independent.
struct image_info
{
    /// The type of the stored values. A reader always gives it; a model for the writer leaves it empty to store the
    /// values in the type of its data.
    std::optional<openfits::bitpix> bitpix{};
    openfits::geometry geometry{};
    /// BSCALE and BZERO (standard §4.4.2.5): the identity when the header has neither.
    openfits::scaling scaling{};
    /// BLANK: the stored value of the undefined values of integer data, before scaling (standard §4.4.2.5).
    std::optional<std::int64_t> blank{};
    openfits::header header{};

    /// The type that holds every physical value of the image, which a read gives without a conversion that loses
    /// anything:
    /// - float or double, for floating-point data;
    /// - the stored type, for integer data without scaling;
    /// - with BSCALE and BZERO both integers, the narrowest integer type that holds every value they give: the
    ///   conventions of standard §5.2.5 among them, BITPIX 16 with BZERO 32768 std::uint16_t, BITPIX 32 with
    ///   BZERO 2147483648 std::uint32_t, BITPIX 64 with BZERO 2^63 std::uint64_t, and BITPIX 8 with BZERO -128
    ///   std::int8_t; double when no integer type holds them all;
    /// - otherwise float for 8-bit and 16-bit data, and double for 32-bit and 64-bit data.
    /// @throws usage_error (errc::invalid_argument) when bitpix is empty.
    [[nodiscard]] OPENFITS_API numeric_type value_type() const;

    friend bool operator==(const image_info&, const image_info&) = default;
};

/// A parameter of random groups (standard §6.1.2): its name and the scaling of its values.
struct group_parameter
{
    /// PTYPEn, without trailing spaces; empty without it.
    std::string name{};
    /// PSCALn and PZEROn (standard §6.1.2, Eq. 5): the identity when the header has neither.
    openfits::scaling scaling{};

    friend bool operator==(const group_parameter&, const group_parameter&) = default;
};

/// Random groups (standard §6): GCOUNT groups in the primary HDU, each of PCOUNT parameters and an array, as a reader
/// describes them. The standard deprecates them for anything but the existing use in radio interferometry, and the
/// writer does not write them.
///
/// A value: copies are independent.
struct groups_info
{
    /// The type of the stored parameters and array elements, which share it (standard §6.3).
    openfits::bitpix bitpix = openfits::bitpix::uint8;
    /// The axes of the array of each group: NAXIS2 to NAXISm, after NAXIS1 = 0 (standard §6.1.1). With NAXIS = 1 there
    /// are none, and each group holds one element, the empty product of Eq. 6, which a read takes for an axis of one.
    openfits::geometry geometry{};
    /// BSCALE and BZERO of the array elements (standard §4.4.2.5).
    openfits::scaling scaling{};
    /// BLANK of integer array elements.
    std::optional<std::int64_t> blank{};
    /// GCOUNT, the number of groups.
    std::uint64_t count = 0;
    /// The PCOUNT parameters of each group, in their order.
    std::vector<group_parameter> parameters{};
    openfits::header header{};

    friend bool operator==(const groups_info&, const groups_info&) = default;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/// The model of an image read from a file, for the writer: image with the structural cards of its header taken out
/// (without_structural()), its BITPIX, axes, scaling and BLANK kept, so that an image read is written again in one
/// line: writer.add_image(without_structural(file.image(index)), values).
[[nodiscard]] OPENFITS_API image_info without_structural(image_info image);

} // namespace openfits
