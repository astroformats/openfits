// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/error.h>
#include <openfits/types.h>

#include "core/endian.h"
#include "data/conversion.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

// The kernels of the conversions of conversion.h, for a host that swaps the bytes of stored values, Swap true, as a
// little-endian host does, or does not, as a big-endian one. The library instantiates those of its host, and the tests
// also those of the other byte order, so that both paths are tested on every host.

namespace openfits::detail::kernels {

/// value converted to To, without a cast where it is of that type already, which GCC reports as useless.
template <typename To, typename From> [[nodiscard]] constexpr To cast(From value) noexcept
{
    if constexpr (std::same_as<To, From>) {
        return value;
    } else {
        return static_cast<To>(value);
    }
}

template <std::size_t Size> struct unsigned_of_size;
template <> struct unsigned_of_size<1>
{
    using type = std::uint8_t;
};
template <> struct unsigned_of_size<2>
{
    using type = std::uint16_t;
};
template <> struct unsigned_of_size<4>
{
    using type = std::uint32_t;
};
template <> struct unsigned_of_size<8>
{
    using type = std::uint64_t;
};

/// The unsigned integer type of the size of T, which holds its bits.
template <typename T> using bits_of = typename unsigned_of_size<sizeof(T)>::type;

/// A stored value, big-endian, at in.
template <bool Swap, typename T> [[nodiscard]] T load_stored(const std::byte* in) noexcept
{
    bits_of<T> bits{};
    std::memcpy(&bits, in, sizeof(T));
    if constexpr (Swap && sizeof(T) > 1) {
        bits = byte_swap(bits);
    }
    return std::bit_cast<T>(bits);
}

/// Stores value at out, big-endian.
template <bool Swap, typename T> void store_stored(std::byte* out, T value) noexcept
{
    auto bits = std::bit_cast<bits_of<T>>(value);
    if constexpr (Swap && sizeof(T) > 1) {
        bits = byte_swap(bits);
    }
    std::memcpy(out, &bits, sizeof(T));
}

/// A value of the application, in the byte order of the host, at in.
template <typename T> [[nodiscard]] T load_native(const std::byte* in) noexcept
{
    T value{};
    std::memcpy(&value, in, sizeof(T));
    return value;
}

template <typename T> void store_native(std::byte* out, T value) noexcept
{
    std::memcpy(out, &value, sizeof(T));
}

/// The values that the kernels convert at once through blocks on the stack: 4 KiB of the widest of them.
inline constexpr std::size_t conversion_block = 512;

/// A stored value, copied as it lies in a file, as the host holds it: its bytes swapped when Swap.
template <bool Swap, typename T> [[nodiscard]] T to_native(T stored) noexcept
{
    if constexpr (Swap && sizeof(T) > 1) {
        return std::bit_cast<T>(byte_swap(std::bit_cast<bits_of<T>>(stored)));
    } else {
        return stored;
    }
}

/// A value of the host as a file stores it.
template <bool Swap, typename T> [[nodiscard]] T to_stored(T value) noexcept
{
    return to_native<Swap>(value);
}

/// The C++ type of the values that a BITPIX stores.
template <bitpix B> struct stored_of;
template <> struct stored_of<bitpix::uint8>
{
    using type = std::uint8_t;
};
template <> struct stored_of<bitpix::int16>
{
    using type = std::int16_t;
};
template <> struct stored_of<bitpix::int32>
{
    using type = std::int32_t;
};
template <> struct stored_of<bitpix::int64>
{
    using type = std::int64_t;
};
template <> struct stored_of<bitpix::float32>
{
    using type = float;
};
template <> struct stored_of<bitpix::float64>
{
    using type = double;
};

/// The C++ type of a numeric type.
template <numeric_type N> struct type_of;
template <> struct type_of<numeric_type::int8>
{
    using type = std::int8_t;
};
template <> struct type_of<numeric_type::uint8>
{
    using type = std::uint8_t;
};
template <> struct type_of<numeric_type::int16>
{
    using type = std::int16_t;
};
template <> struct type_of<numeric_type::uint16>
{
    using type = std::uint16_t;
};
template <> struct type_of<numeric_type::int32>
{
    using type = std::int32_t;
};
template <> struct type_of<numeric_type::uint32>
{
    using type = std::uint32_t;
};
template <> struct type_of<numeric_type::int64>
{
    using type = std::int64_t;
};
template <> struct type_of<numeric_type::uint64>
{
    using type = std::uint64_t;
};
template <> struct type_of<numeric_type::float32>
{
    using type = float;
};
template <> struct type_of<numeric_type::float64>
{
    using type = double;
};

/// The name of a type in messages.
template <typename T> [[nodiscard]] constexpr std::string_view type_name() noexcept
{
    if constexpr (std::same_as<T, float>) {
        return "float";
    } else if constexpr (std::same_as<T, double>) {
        return "double";
    } else if constexpr (sizeof(T) == 1) {
        return std::is_signed_v<T> ? "int8" : "uint8";
    } else if constexpr (sizeof(T) == 2) {
        return std::is_signed_v<T> ? "int16" : "uint16";
    } else if constexpr (sizeof(T) == 4) {
        return std::is_signed_v<T> ? "int32" : "uint32";
    } else {
        return std::is_signed_v<T> ? "int64" : "uint64";
    }
}

/// True when the real value converts into the integer type T with the rounding: when the integer it gives lies within
/// the range of T. The bounds are min - 1 and max + 1 for a truncation, min - 0.5 and max + 0.5 for a rounding, which
/// are exact or round to the right bound for every type: below -2^63 the nearest double is -2^63 itself, a valid value.
template <std::integral T> [[nodiscard]] bool real_fits(double value, rounding mode) noexcept
{
    const auto min = static_cast<double>(std::numeric_limits<T>::min());
    const auto max = static_cast<double>(std::numeric_limits<T>::max());
    const double reach = mode == rounding::truncate ? 1.0 : 0.5;
    return (value > min - reach || value >= min) && value < max + reach;
}

/// The largest double below 0.5.
inline constexpr double below_half = 0.5 - 0x1p-54;

/// The signed integer type that a real value truncates into on its way to T: 32 bits where they hold every value of T,
/// which vectorizers convert, and 64 bits otherwise.
template <typename T>
using truncation_of = std::conditional_t<sizeof(T) < 4 || std::same_as<T, std::int32_t>, std::int32_t, std::int64_t>;

/// The integer of a real value that real_fits() accepts, truncated or rounded half away from zero. The rounding adds
/// below_half in the direction of the sign before the truncation: the sum, rounded to the nearest double, lies on the
/// side of the next integer that the value rounds to, for every double. It costs neither the call of std::round nor a
/// branch, and loops of it vectorize.
template <std::integral T> [[nodiscard]] T real_to_integer(double value, rounding mode) noexcept
{
    if (mode == rounding::nearest) {
        value += value < 0.0 ? -below_half : below_half;
    }
    if constexpr (std::same_as<T, std::uint64_t>) {
        // Beyond 2^63 every double is an integer, which the signed conversion below could not hold.
        if (value >= 9223372036854775808.0) {
            return static_cast<std::uint64_t>(value);
        }
    }
    return cast<T>(static_cast<truncation_of<T>>(value));
}

/// The value of T nearest to a real one beyond its range, or to NaN, which has none: a clamp.
template <typename T> [[nodiscard]] T clamp_real(double value) noexcept
{
    if constexpr (std::floating_point<T>) {
        return value < 0.0 ? std::numeric_limits<T>::lowest() : std::numeric_limits<T>::max();
    } else {
        return value < 0.0 ? std::numeric_limits<T>::min() : std::numeric_limits<T>::max();
    }
}

/// The text of a value, for messages.
template <typename T> [[nodiscard]] std::string text_of(T value)
{
    if constexpr (std::floating_point<T>) {
        return std::to_string(cast<double>(value));
    } else if constexpr (std::is_signed_v<T>) {
        return std::to_string(cast<std::int64_t>(value));
    } else {
        return std::to_string(cast<std::uint64_t>(value));
    }
}

// The errors of the kernels. Each builds its message itself, out of the loops that call it: a loop that held the
// strings of a message would need a frame for them, and the function of each value would be too large to be inlined.

/// A value of a read beyond the range of the destination type, with the stored value that it comes from.
template <typename Value, typename Stored>
[[noreturn]] void throw_out_of_range(Value value, Stored stored, std::string_view type, std::uint64_t hdu)
{
    throw invalid_data_error(errc::value_out_of_range,
                             "the value " + text_of(value) + " (stored as " + text_of(stored) +
                                 ") is beyond the range of " + std::string(type),
                             {.hdu = hdu});
}

/// A value of a write beyond the range of the stored type.
template <typename Value> [[noreturn]] void throw_out_of_range(Value value, std::string_view type, std::uint64_t hdu)
{
    throw invalid_data_error(
        errc::value_out_of_range,
        "the value " + text_of(value) + " is beyond the range of the stored type " + std::string(type), {.hdu = hdu});
}

[[noreturn]] inline void throw_undefined(std::string_view type, std::uint64_t hdu)
{
    throw invalid_data_error(errc::undefined_value,
                             "an undefined value, NaN, cannot be held in " + std::string(type) +
                                 " without a policy for undefined values",
                             {.hdu = hdu});
}

/// A NaN of a write into integer data without BLANK.
[[noreturn]] inline void throw_without_blank(std::uint64_t hdu)
{
    throw invalid_data_error(errc::undefined_value, "a NaN cannot be written into integer data without BLANK",
                             {.hdu = hdu});
}

/// The conversion of the values of a read from Stored into Target, the types of the stored and the destination values.
template <bool Swap, typename Stored, typename Target> class read_values
{
public:
    /// True when the values convert in place: their stored and destination types have one size.
    static constexpr bool same_size = sizeof(Stored) == sizeof(Target);

    explicit read_values(const read_conversion& conversion) noexcept : c_(conversion)
    {
        if (c_.policy == blank_policy::value) {
            // make_read_conversion() checked that it converts.
            blank_ = cast<Target>(c_.blank_value);
        }
    }

    std::uint64_t run(const std::byte* in, std::ptrdiff_t stride, std::byte* out, std::uint8_t* mask, std::size_t count)
    {
        if (direct()) {
            if (mask != nullptr) {
                std::memset(mask, 0, count);
            }
            with_arithmetic([&]<arithmetic A>() { direct_run<A>(in, stride, out, count); });
            return 0;
        }
        for (std::size_t i = 0; i < count; ++i) {
            checked(load_stored<Swap, Stored>(at(in, stride, i)), out + (i * sizeof(Target)),
                    mask == nullptr ? nullptr : mask + i);
        }
        return clamped_;
    }

    std::uint64_t run_in_place(std::byte* data, std::uint8_t* mask, std::size_t count)
    {
        static_assert(sizeof(Stored) == sizeof(Target));
        if (direct()) {
            if (mask != nullptr) {
                std::memset(mask, 0, count);
            }
            // Each value read before it is written, at the same place, which a loop of one pointer vectorizes.
            with_arithmetic([&]<arithmetic A>() {
                if constexpr (bitwise<A>()) {
                    bitwise_in_place<A>(data, count);
                } else {
                    for (std::size_t i = 0; i < count; ++i) {
                        std::byte* place = data + (i * sizeof(Target));
                        store_native(place, direct_value<A>(load_stored<Swap, Stored>(place)));
                    }
                }
            });
            return 0;
        }
        for (std::size_t i = 0; i < count; ++i) {
            std::byte* place = data + (i * sizeof(Target));
            checked(load_stored<Swap, Stored>(place), place, mask == nullptr ? nullptr : mask + i);
        }
        return clamped_;
    }

private:
    // No value needs a check: each converts without leaving the range of Target, and none is undefined.
    [[nodiscard]] bool direct() const noexcept
    {
        return !c_.checks_range && !c_.checks_undefined;
    }

    // The value i of a run from in, stride values apart: the offset of each computed alone, so that no pointer goes
    // beyond the values of the run.
    [[nodiscard]] static const std::byte* at(const std::byte* in, std::ptrdiff_t stride, std::size_t i) noexcept
    {
        return in + (static_cast<std::ptrdiff_t>(i) * stride * static_cast<std::ptrdiff_t>(sizeof(Stored)));
    }

    // Calls function with the arithmetic of the conversion as a template argument, so that the loops it runs have no
    // branch on it: the vectorizers of compilers take loops without one.
    template <typename Function> void with_arithmetic(const Function& function) const
    {
        switch (c_.arithmetic) {
        case arithmetic::identity:
            function.template operator()<arithmetic::identity>();
            return;
        case arithmetic::sign_flip:
            function.template operator()<arithmetic::sign_flip>();
            return;
        case arithmetic::signed_offset:
            function.template operator()<arithmetic::signed_offset>();
            return;
        case arithmetic::unsigned_offset:
            function.template operator()<arithmetic::unsigned_offset>();
            return;
        case arithmetic::scaled:
            function.template operator()<arithmetic::scaled>();
            return;
        }
    }

    // True when the conversion of A is a matter of bits: the stored type itself, or the type of an offset of Table 11,
    // which a flip of the sign bit gives.
    template <arithmetic A> static constexpr bool bitwise() noexcept
    {
        return (A == arithmetic::identity && std::same_as<Stored, Target>) ||
               (A == arithmetic::sign_flip && std::integral<Target> && sizeof(Stored) == sizeof(Target));
    }

    // The values of a bitwise conversion, as unsigned integers: their bytes swapped, and the sign bit flipped for an
    // offset, which loops of unsigned integers vectorize best.
    template <arithmetic A> static void bitwise_run(const std::byte* in, std::byte* out, std::size_t count) noexcept
    {
        using bits = bits_of<Stored>;
        constexpr bits sign = A == arithmetic::sign_flip ? cast<bits>(bits{1} << (sizeof(Stored) * 8 - 1)) : bits{0};
        for (std::size_t i = 0; i < count; ++i) {
            bits value{};
            std::memcpy(&value, in + (i * sizeof(bits)), sizeof(bits));
            if constexpr (Swap && sizeof(bits) > 1) {
                value = byte_swap(value);
            }
            value = cast<bits>(value ^ sign);
            std::memcpy(out + (i * sizeof(bits)), &value, sizeof(bits));
        }
    }

    // bitwise_run() over one buffer, through one pointer: the vectorizer of MSVC takes two for buffers that may
    // overlap.
    template <arithmetic A> static void bitwise_in_place(std::byte* data, std::size_t count) noexcept
    {
        using bits = bits_of<Stored>;
        constexpr bits sign = A == arithmetic::sign_flip ? cast<bits>(bits{1} << (sizeof(Stored) * 8 - 1)) : bits{0};
        for (std::size_t i = 0; i < count; ++i) {
            bits value{};
            std::memcpy(&value, data + (i * sizeof(bits)), sizeof(bits));
            if constexpr (Swap && sizeof(bits) > 1) {
                value = byte_swap(value);
            }
            value = cast<bits>(value ^ sign);
            std::memcpy(data + (i * sizeof(bits)), &value, sizeof(bits));
        }
    }

    template <arithmetic A>
    void direct_run(const std::byte* in, std::ptrdiff_t stride, std::byte* out, std::size_t count)
    {
        if constexpr (bitwise<A>()) {
            if (stride == 1) {
                bitwise_run<A>(in, out, count);
                return;
            }
        }
        if (stride == 1) {
            // Through blocks on the stack, which cannot overlap: the vectorizer of MSVC refuses loops between two
            // pointers that might.
            // Written before they are read: zeros would cost a pass over them at each call, a run of a row of a region.
            // NOLINTBEGIN(cppcoreguidelines-pro-type-member-init)
            std::array<Stored, conversion_block> stored;
            std::array<Target, conversion_block> converted;
            // NOLINTEND(cppcoreguidelines-pro-type-member-init)
            for (std::size_t first = 0; first < count; first += conversion_block) {
                const std::size_t values = std::min(conversion_block, count - first);
                std::memcpy(stored.data(), in + (first * sizeof(Stored)), values * sizeof(Stored));
                for (std::size_t i = 0; i < values; ++i) {
                    converted[i] = direct_value<A>(to_native<Swap>(stored[i]));
                }
                std::memcpy(out + (first * sizeof(Target)), converted.data(), values * sizeof(Target));
            }
        } else {
            for (std::size_t i = 0; i < count; ++i) {
                store_native(out + (i * sizeof(Target)), direct_value<A>(load_stored<Swap, Stored>(at(in, stride, i))));
            }
        }
    }

    // The conversion of a value that needs no check, by the arithmetic A.
    template <arithmetic A> [[nodiscard]] Target direct_value(Stored stored) const noexcept
    {
        if constexpr (A == arithmetic::identity) {
            return cast<Target>(stored);
        } else if constexpr (A == arithmetic::scaled || std::floating_point<Stored>) {
            return from_real(scaled(stored));
        } else if constexpr (A == arithmetic::sign_flip) {
            return cast<Target>(flipped(stored));
        } else if constexpr (A == arithmetic::signed_offset) {
            return cast<Target>(cast<std::int64_t>(stored) + c_.offset);
        } else {
            return cast<Target>(signed_bits(stored) + c_.unsigned_offset);
        }
    }

    // The physical value of an offset of standard §5.2.5: the stored value with its most significant bit flipped, in
    // the type that holds it, std::int8_t for bytes and the unsigned type of their size for the others.
    [[nodiscard]] static auto flipped(Stored stored) noexcept
    {
        using bits = bits_of<Stored>;
        constexpr bits sign = cast<bits>(bits{1} << (sizeof(Stored) * 8 - 1));
        const bits value = cast<bits>(std::bit_cast<bits>(stored) ^ sign);
        if constexpr (std::same_as<Stored, std::uint8_t>) {
            return std::bit_cast<std::int8_t>(value);
        } else {
            return value;
        }
    }

    // A real value within the range of Target, converted.
    [[nodiscard]] Target from_real(double value) const noexcept
    {
        if constexpr (std::floating_point<Target>) {
            return cast<Target>(value);
        } else {
            return real_to_integer<Target>(value, c_.rounding);
        }
    }

    // zero + scale x stored, the product rounded before the sum: two statements, which no compiler fuses into one
    // multiply-add, so that every host gives the same value.
    [[nodiscard]] double scaled(Stored stored) const noexcept
    {
        const double product = cast<double>(stored) * c_.scale;
        return product + c_.zero;
    }

    // The bits of a stored integer as an unsigned 64-bit integer, sign-extended, for an unsigned offset that wraps.
    [[nodiscard]] static std::uint64_t signed_bits(Stored stored) noexcept
    {
        return static_cast<std::uint64_t>(cast<std::int64_t>(stored));
    }

    [[nodiscard]] bool undefined(Stored stored) const noexcept
    {
        if constexpr (std::floating_point<Stored>) {
            return std::isnan(stored);
        } else {
            return c_.blank && cast<std::int64_t>(stored) == *c_.blank;
        }
    }

    void checked(Stored stored, std::byte* out, std::uint8_t* mask)
    {
        if (c_.checks_undefined && undefined(stored)) {
            store_native(out, undefined_value());
            if (mask != nullptr) {
                *mask = 1;
            }
            return;
        }
        if (mask != nullptr) {
            *mask = 0;
        }
        store_native(out, checked_value(stored));
    }

    [[nodiscard]] Target undefined_value() const
    {
        switch (c_.policy) {
        case blank_policy::value:
            return blank_;
        case blank_policy::mask:
            if constexpr (std::floating_point<Target>) {
                return std::numeric_limits<Target>::quiet_NaN();
            } else {
                return Target{};
            }
        case blank_policy::nan:
        case blank_policy::ignore:
            break;
        }
        if constexpr (std::floating_point<Target>) {
            return std::numeric_limits<Target>::quiet_NaN();
        } else {
            throw_undefined(type_name<Target>(), c_.hdu);
        }
    }

    [[nodiscard]] Target checked_value(Stored stored)
    {
        if constexpr (std::floating_point<Stored>) {
            if (c_.arithmetic == arithmetic::identity) {
                return checked_real(cast<double>(stored), stored);
            }
            return checked_real(scaled(stored), stored);
        } else {
            switch (c_.arithmetic) {
            case arithmetic::identity:
                return checked_integer(cast<std::int64_t>(stored), stored);
            case arithmetic::sign_flip:
                return checked_integer(flipped(stored), stored);
            case arithmetic::signed_offset:
                return checked_integer(cast<std::int64_t>(stored) + c_.offset, stored);
            case arithmetic::unsigned_offset:
                return checked_integer(signed_bits(stored) + c_.unsigned_offset, stored);
            case arithmetic::scaled:
                break;
            }
            return checked_real(scaled(stored), stored);
        }
    }

    // An exact integer value, signed or unsigned, checked against the range of Target.
    template <std::integral Exact> [[nodiscard]] Target checked_integer(Exact value, Stored stored)
    {
        if constexpr (std::floating_point<Target>) {
            return cast<Target>(value);
        } else {
            if (std::in_range<Target>(value)) {
                return cast<Target>(value);
            }
            if (c_.overflow == overflow_policy::refuse) {
                throw_out_of_range(value, stored, type_name<Target>(), c_.hdu);
            }
            ++clamped_;
            return std::cmp_less(value, 0) ? std::numeric_limits<Target>::min() : std::numeric_limits<Target>::max();
        }
    }

    // A real value checked against the range of Target: an integer type, or float, which a finite value beyond its
    // range overflows. Infinities and NaN are values of floating-point types, and overflow integer ones.
    [[nodiscard]] Target checked_real(double value, Stored stored)
    {
        if constexpr (std::floating_point<Target>) {
            if constexpr (std::same_as<Target, float>) {
                if (std::isfinite(value) && std::abs(value) > static_cast<double>(std::numeric_limits<float>::max())) {
                    return overflow(value, stored);
                }
            }
            if constexpr (std::same_as<Target, Stored>) {
                // The bits of the stored value, NaN payloads and signed zeros, when nothing scales it.
                if (c_.arithmetic == arithmetic::identity) {
                    return stored;
                }
            }
            return cast<Target>(value);
        } else {
            if (std::isnan(value)) {
                throw_undefined(type_name<Target>(), c_.hdu);
            }
            if (!real_fits<Target>(value, c_.rounding)) {
                return overflow(value, stored);
            }
            return real_to_integer<Target>(value, c_.rounding);
        }
    }

    [[nodiscard]] Target overflow(double value, Stored stored)
    {
        if (c_.overflow == overflow_policy::refuse) {
            throw_out_of_range(value, stored, type_name<Target>(), c_.hdu);
        }
        ++clamped_;
        return clamp_real<Target>(value);
    }

    const read_conversion& c_;
    Target blank_{};
    std::uint64_t clamped_ = 0;
};

/// The conversion of the values of a write from Source, the type of the application, into Stored.
template <bool Swap, typename Source, typename Stored> class write_values
{
public:
    explicit write_values(const write_conversion& conversion) noexcept : c_(conversion) {}

    std::uint64_t run(const std::byte* in, std::byte* out, std::size_t count)
    {
        if (!c_.checks_range && !c_.checks_undefined) {
            // The arithmetic as a template argument, so that the loop has no branch on it, which vectorizers take.
            switch (c_.arithmetic) {
            case arithmetic::identity:
                direct_run<arithmetic::identity>(in, out, count);
                break;
            case arithmetic::sign_flip:
                direct_run<arithmetic::sign_flip>(in, out, count);
                break;
            case arithmetic::signed_offset:
                direct_run<arithmetic::signed_offset>(in, out, count);
                break;
            case arithmetic::unsigned_offset:
                direct_run<arithmetic::unsigned_offset>(in, out, count);
                break;
            case arithmetic::scaled:
                direct_run<arithmetic::scaled>(in, out, count);
                break;
            }
            return 0;
        }
        if constexpr (std::floating_point<Source> && std::integral<Stored>) {
            // Real values have no arithmetic but the scaling.
            if (c_.arithmetic == arithmetic::identity) {
                screened_run<arithmetic::identity>(in, out, count);
            } else {
                screened_run<arithmetic::scaled>(in, out, count);
            }
        } else {
            for (std::size_t i = 0; i < count; ++i) {
                store_stored<Swap>(out + (i * sizeof(Stored)), checked(load_native<Source>(in + (i * sizeof(Source)))));
            }
        }
        return clamped_;
    }

private:
    // Real values into integer data, through blocks on the stack: the real values of a block computed in one loop, then
    // converted without a check in another when each of them lies in the range of Stored, in loops without a branch on
    // the values; the values of a block with a NaN or one beyond the range convert one by one, with their checks.
    template <arithmetic A> void screened_run(const std::byte* in, std::byte* out, std::size_t count)
    {
        // The reals that round half away from zero into values of Stored, NaN none of them. The upper bound of 64-bit
        // integers rounds to 2^63, which is beyond them and excluded.
        constexpr auto low = static_cast<double>(std::numeric_limits<Stored>::min());
        constexpr double high = static_cast<double>(std::numeric_limits<Stored>::max()) + 0.5;
        const double zero = c_.zero;
        const double scale = c_.scale;
        // Written before they are read: zeros would cost a pass over them at each call.
        // NOLINTBEGIN(cppcoreguidelines-pro-type-member-init)
        std::array<Source, conversion_block> values;
        std::array<double, conversion_block> reals;
        std::array<Stored, conversion_block> stored;
        // NOLINTEND(cppcoreguidelines-pro-type-member-init)
        for (std::size_t first = 0; first < count; first += conversion_block) {
            const std::size_t block = std::min(conversion_block, count - first);
            std::memcpy(values.data(), in + (first * sizeof(Source)), block * sizeof(Source));
            for (std::size_t i = 0; i < block; ++i) {
                if constexpr (A == arithmetic::identity) {
                    reals[i] = cast<double>(values[i]);
                } else {
                    reals[i] = unscaled(values[i], zero, scale);
                }
            }
            // Counted without a branch, which the vectorizer of MSVC refuses: NaN fails both comparisons.
            std::size_t within = 0;
            for (std::size_t i = 0; i < block; ++i) {
                within += static_cast<std::size_t>(reals[i] >= low) & static_cast<std::size_t>(reals[i] < high);
            }
            if (within == block) {
                for (std::size_t i = 0; i < block; ++i) {
                    stored[i] = real_to_integer<Stored>(reals[i], rounding::nearest);
                }
            } else {
                for (std::size_t i = 0; i < block; ++i) {
                    stored[i] = checked(values[i]);
                }
            }
            if constexpr (Swap && sizeof(Stored) > 1) {
                // In a loop of its own: the vectorizer of MSVC refuses a loop of the conversion that also swaps.
                for (std::size_t i = 0; i < block; ++i) {
                    stored[i] = to_stored<Swap>(stored[i]);
                }
            }
            std::memcpy(out + (first * sizeof(Stored)), stored.data(), block * sizeof(Stored));
        }
    }

    // Through blocks on the stack, which cannot overlap: the vectorizer of MSVC refuses loops between two pointers that
    // might.
    template <arithmetic A> void direct_run(const std::byte* in, std::byte* out, std::size_t count) const noexcept
    {
        // Written before they are read: zeros would cost a pass over them at each call.
        // NOLINTBEGIN(cppcoreguidelines-pro-type-member-init)
        std::array<Source, conversion_block> values;
        std::array<Stored, conversion_block> stored;
        // NOLINTEND(cppcoreguidelines-pro-type-member-init)
        for (std::size_t first = 0; first < count; first += conversion_block) {
            const std::size_t block = std::min(conversion_block, count - first);
            std::memcpy(values.data(), in + (first * sizeof(Source)), block * sizeof(Source));
            for (std::size_t i = 0; i < block; ++i) {
                stored[i] = to_stored<Swap>(direct_value<A>(values[i]));
            }
            std::memcpy(out + (first * sizeof(Stored)), stored.data(), block * sizeof(Stored));
        }
    }

    // The stored value of a value that needs no check, by the arithmetic A.
    template <arithmetic A> [[nodiscard]] Stored direct_value(Source value) const noexcept
    {
        if constexpr (std::floating_point<Source>) {
            if constexpr (A == arithmetic::identity) {
                if constexpr (std::floating_point<Stored>) {
                    return cast<Stored>(value);
                } else {
                    return from_real(cast<double>(value));
                }
            } else {
                return from_real(unscaled(value));
            }
        } else if constexpr (A == arithmetic::identity) {
            return cast<Stored>(value);
        } else if constexpr (A == arithmetic::sign_flip) {
            return flipped(value);
        } else if constexpr (A == arithmetic::signed_offset) {
            return cast<Stored>(cast<std::int64_t>(value) - c_.offset);
        } else if constexpr (A == arithmetic::unsigned_offset) {
            return cast<Stored>(static_cast<std::int64_t>(signed_bits(value) - c_.unsigned_offset));
        } else {
            return from_real(unscaled(value));
        }
    }

    // The stored value of an offset of standard §5.2.5, for a value of the type that it holds, of the size of Stored:
    // its most significant bit flipped.
    [[nodiscard]] static Stored flipped(Source value) noexcept
    {
        if constexpr (sizeof(Source) == sizeof(Stored) && std::integral<Stored>) {
            using bits = bits_of<Stored>;
            constexpr bits sign = cast<bits>(bits{1} << (sizeof(Stored) * 8 - 1));
            return std::bit_cast<Stored>(cast<bits>(std::bit_cast<bits>(value) ^ sign));
        } else {
            // make_write_conversion() flips only types of the size of the stored ones.
            return Stored{};
        }
    }
    // (value - zero) / scale, as the inverse of the scaling of the read.
    [[nodiscard]] static double unscaled(Source value, double zero, double scale) noexcept
    {
        const double difference = cast<double>(value) - zero;
        return difference / scale;
    }

    [[nodiscard]] double unscaled(Source value) const noexcept
    {
        return unscaled(value, c_.zero, c_.scale);
    }

    // The bits of an integer as an unsigned 64-bit integer, sign-extended, for an unsigned offset that wraps.
    [[nodiscard]] static std::uint64_t signed_bits(Source value) noexcept
    {
        if constexpr (std::is_signed_v<Source>) {
            return static_cast<std::uint64_t>(cast<std::int64_t>(value));
        } else {
            return cast<std::uint64_t>(value);
        }
    }

    // A real value within the range of Stored, converted: rounded half away from zero into an integer.
    [[nodiscard]] static Stored from_real(double value) noexcept
    {
        if constexpr (std::floating_point<Stored>) {
            return cast<Stored>(value);
        } else {
            return real_to_integer<Stored>(value, rounding::nearest);
        }
    }

    [[nodiscard]] Stored checked(Source value)
    {
        if constexpr (std::floating_point<Source>) {
            if (std::isnan(value)) {
                return undefined();
            }
            const double real = c_.arithmetic == arithmetic::identity ? cast<double>(value) : unscaled(value);
            return checked_real(real, value);
        } else {
            switch (c_.arithmetic) {
            case arithmetic::identity:
                return checked_integer(value, value);
            case arithmetic::sign_flip:
                // Every value of the type that the offset stores lies in the range of the stored type.
                return flipped(value);
            case arithmetic::signed_offset:
                return checked_integer(cast<std::int64_t>(value) - c_.offset, value);
            case arithmetic::unsigned_offset:
                // A signed value below 0 is below the range that BZERO = 2^63 gives.
                if (std::cmp_less(value, 0)) {
                    return overflow_integer(value);
                }
                return cast<Stored>(static_cast<std::int64_t>(signed_bits(value) - c_.unsigned_offset));
            case arithmetic::scaled:
                break;
            }
            return checked_real(unscaled(value), value);
        }
    }

    [[nodiscard]] Stored undefined() const
    {
        if constexpr (std::floating_point<Stored>) {
            return std::numeric_limits<Stored>::quiet_NaN();
        } else {
            if (!c_.blank) {
                throw_without_blank(c_.hdu);
            }
            // make_write_conversion() checked that BLANK is a value of Stored.
            return cast<Stored>(*c_.blank);
        }
    }

    template <std::integral Exact> [[nodiscard]] Stored checked_integer(Exact stored, Source value)
    {
        if constexpr (std::floating_point<Stored>) {
            return cast<Stored>(stored);
        } else {
            if (std::in_range<Stored>(stored)) {
                return cast<Stored>(stored);
            }
            return overflow_integer(value, std::cmp_less(stored, 0));
        }
    }

    [[nodiscard]] Stored overflow_integer(Source value, bool below = true)
    {
        if (c_.overflow == overflow_policy::refuse) {
            throw_out_of_range(value, type_name<Stored>(), c_.hdu);
        }
        ++clamped_;
        if constexpr (std::floating_point<Stored>) {
            return below ? std::numeric_limits<Stored>::lowest() : std::numeric_limits<Stored>::max();
        } else {
            return below ? std::numeric_limits<Stored>::min() : std::numeric_limits<Stored>::max();
        }
    }

    [[nodiscard]] Stored checked_real(double real, Source value)
    {
        if constexpr (std::floating_point<Stored>) {
            if constexpr (std::same_as<Stored, float>) {
                if (std::isfinite(real) && std::abs(real) > static_cast<double>(std::numeric_limits<float>::max())) {
                    return overflow_real(real, value);
                }
            }
            if constexpr (std::same_as<Source, Stored>) {
                // The bits of the value, NaN payloads and signed zeros, when nothing scales it.
                if (c_.arithmetic == arithmetic::identity) {
                    return value;
                }
            }
            return cast<Stored>(real);
        } else {
            if (!real_fits<Stored>(real, rounding::nearest)) {
                return overflow_real(real, value);
            }
            return real_to_integer<Stored>(real, rounding::nearest);
        }
    }

    [[nodiscard]] Stored overflow_real(double real, Source value)
    {
        if (c_.overflow == overflow_policy::refuse) {
            throw_out_of_range(value, type_name<Stored>(), c_.hdu);
        }
        ++clamped_;
        return clamp_real<Stored>(real);
    }

    const write_conversion& c_;
    std::uint64_t clamped_ = 0;
};

/// Converts the values of a read, with the kernel of the stored and the destination types of the conversion.
template <bool Swap, typename Function>
decltype(auto) with_read_types(const read_conversion& conversion, const Function& function)
{
    const auto with_target = [&]<typename Stored>(std::type_identity<Stored>) -> decltype(auto) {
        switch (conversion.to) {
        case numeric_type::int8:
            return function(read_values<Swap, Stored, std::int8_t>(conversion));
        case numeric_type::uint8:
            return function(read_values<Swap, Stored, std::uint8_t>(conversion));
        case numeric_type::int16:
            return function(read_values<Swap, Stored, std::int16_t>(conversion));
        case numeric_type::uint16:
            return function(read_values<Swap, Stored, std::uint16_t>(conversion));
        case numeric_type::int32:
            return function(read_values<Swap, Stored, std::int32_t>(conversion));
        case numeric_type::uint32:
            return function(read_values<Swap, Stored, std::uint32_t>(conversion));
        case numeric_type::int64:
            return function(read_values<Swap, Stored, std::int64_t>(conversion));
        case numeric_type::uint64:
            return function(read_values<Swap, Stored, std::uint64_t>(conversion));
        case numeric_type::float32:
            return function(read_values<Swap, Stored, float>(conversion));
        case numeric_type::float64:
            break;
        }
        return function(read_values<Swap, Stored, double>(conversion));
    };
    switch (conversion.from) {
    case bitpix::uint8:
        return with_target(std::type_identity<std::uint8_t>{});
    case bitpix::int16:
        return with_target(std::type_identity<std::int16_t>{});
    case bitpix::int32:
        return with_target(std::type_identity<std::int32_t>{});
    case bitpix::int64:
        return with_target(std::type_identity<std::int64_t>{});
    case bitpix::float32:
        return with_target(std::type_identity<float>{});
    case bitpix::float64:
        break;
    }
    return with_target(std::type_identity<double>{});
}

/// convert_read() for a host of the byte order of Swap.
template <bool Swap>
std::uint64_t read(const read_conversion& conversion, const std::byte* in, std::ptrdiff_t stride, std::byte* out,
                   std::uint8_t* mask, std::size_t count)
{
    return with_read_types<Swap>(conversion, [&](auto values) { return values.run(in, stride, out, mask, count); });
}

/// convert_read_in_place() for a host of the byte order of Swap.
template <bool Swap>
std::uint64_t read_in_place(const read_conversion& conversion, std::byte* data, std::uint8_t* mask, std::size_t count)
{
    return with_read_types<Swap>(conversion, [&]<typename Values>(Values values) -> std::uint64_t {
        if constexpr (Values::same_size) {
            return values.run_in_place(data, mask, count);
        } else {
            return 0;
        }
    });
}

/// convert_write() for a host of the byte order of Swap.
template <bool Swap>
std::uint64_t write(const write_conversion& conversion, const std::byte* in, std::byte* out, std::size_t count)
{
    const auto with_stored = [&]<typename Source>(std::type_identity<Source>) -> std::uint64_t {
        switch (conversion.to) {
        case bitpix::uint8:
            return write_values<Swap, Source, std::uint8_t>(conversion).run(in, out, count);
        case bitpix::int16:
            return write_values<Swap, Source, std::int16_t>(conversion).run(in, out, count);
        case bitpix::int32:
            return write_values<Swap, Source, std::int32_t>(conversion).run(in, out, count);
        case bitpix::int64:
            return write_values<Swap, Source, std::int64_t>(conversion).run(in, out, count);
        case bitpix::float32:
            return write_values<Swap, Source, float>(conversion).run(in, out, count);
        case bitpix::float64:
            break;
        }
        return write_values<Swap, Source, double>(conversion).run(in, out, count);
    };
    switch (conversion.from) {
    case numeric_type::int8:
        return with_stored(std::type_identity<std::int8_t>{});
    case numeric_type::uint8:
        return with_stored(std::type_identity<std::uint8_t>{});
    case numeric_type::int16:
        return with_stored(std::type_identity<std::int16_t>{});
    case numeric_type::uint16:
        return with_stored(std::type_identity<std::uint16_t>{});
    case numeric_type::int32:
        return with_stored(std::type_identity<std::int32_t>{});
    case numeric_type::uint32:
        return with_stored(std::type_identity<std::uint32_t>{});
    case numeric_type::int64:
        return with_stored(std::type_identity<std::int64_t>{});
    case numeric_type::uint64:
        return with_stored(std::type_identity<std::uint64_t>{});
    case numeric_type::float32:
        return with_stored(std::type_identity<float>{});
    case numeric_type::float64:
        break;
    }
    return with_stored(std::type_identity<double>{});
}

} // namespace openfits::detail::kernels
