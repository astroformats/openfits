// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/card.h>
#include <openfits/export.h>
#include <openfits/header.h>
#include <openfits/types.h>

#include <complex>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// @file
/// Tables: the ASCII tables and binary tables of standard §7.2 and §7.3, their columns, the formats of their fields,
/// and the values of their cells.

namespace openfits {

/// The types of the elements of the fields of tables (standard §7.2.1, Table 15; §7.3.1, Table 18).
enum class column_type : std::uint8_t
{
    logical,         ///< L: T for true, F for false, and a 0 byte for an undefined value (binary tables).
    bit,             ///< X: bits, the most significant first (binary tables).
    uint8,           ///< B: 8-bit unsigned integers (binary tables).
    int16,           ///< I: 16-bit integers (binary tables).
    int32,           ///< J: 32-bit integers (binary tables).
    int64,           ///< K: 64-bit integers (binary tables); Iw, decimal integers (ASCII tables).
    character,       ///< A: characters (binary tables); Aw (ASCII tables).
    float32,         ///< E: 32-bit floating point (binary tables).
    float64,         ///< D: 64-bit floating point (binary tables); Fw.d, Ew.d and Dw.d, decimal reals (ASCII tables).
    complex_float32, ///< C: pairs of 32-bit floating point, the real part first (binary tables).
    complex_float64, ///< M: pairs of 64-bit floating point, the real part first (binary tables).
};

/// Where the elements of a field of a binary table lie (standard §7.3.5).
enum class column_storage : std::uint8_t
{
    fixed,      ///< In the row: the elements of the repeat count.
    variable_p, ///< rPt(emax): an array in the heap, which a descriptor of two 32-bit integers in the row points to.
    variable_q, ///< rQt(emax): an array in the heap, which a descriptor of two 64-bit integers points to.
};

#if defined(_MSC_VER)
// The members hold standard-library types, which cl reports for an exported class. A C++ interface requires the same
// standard library on both sides of the DLL boundary anyway.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

/// The format of the fields of a column: a TFORMn value (standard §7.2.1, Table 15; §7.3.1, Table 18; §7.3.5).
///
/// In a binary table, rTa: the repeat count r and the type T of Table 18, and for the descriptors P and Q the type of
/// the arrays they point to, with emax, the most elements that any of them holds. The form rAw of the character type, a
/// convention outside the standard, gives a field of r / w strings of w characters, as TDIMn = '(w,r/w)' would. In an
/// ASCII table, a Fortran format of Table 15: Aw, a field of w characters; Iw, an integer; and Fw.d, Ew.d and Dw.d,
/// reals, which field holds.
///
/// A value: copies are independent.
struct column_format
{
    column_type type = column_type::uint8;
    /// The elements of a field: r of a binary table, its characters for type character, and its bits for type bit;
    /// for P and Q, 0 or 1, the descriptors. In an ASCII table, w for Aw and 1 for the numbers.
    std::uint64_t repeat = 1;
    column_storage storage = column_storage::fixed;
    /// emax of rPt(emax) and rQt(emax), when given.
    std::optional<std::uint64_t> max_elements{};
    /// w of rAw, when given: the characters of each string of a field of type character.
    std::optional<std::uint64_t> substring_width{};
    /// The Fortran format of a field of an ASCII table: character for Aw, integer for Iw, fixed for Fw.d, exponential
    /// for Ew.d and double_exponential for Dw.d; nothing in a binary table.
    std::optional<display_format> field{};

    /// Parses the TFORMn value of a binary table: a repeat count of digits, 1 when absent; a type of Table 18 in upper
    /// case; for P and Q the type of the array, but P and Q, and (emax); for type A digits, the width of the rAw
    /// convention. Spaces before and after are ignored, and so are the characters after the form, which the standard
    /// leaves to software (rTa). A repeat count of 0 is an empty field.
    /// @throws invalid_data_error (errc::invalid_column_format) for text of no such form, a repeat count beyond 64
    ///         bits, a descriptor whose repeat count is above 1, or an rAw whose w is 0 or above r.
    [[nodiscard]] OPENFITS_API static column_format parse_binary(std::string_view text);

    /// Parses the TFORMn value of an ASCII table: Aw, Iw, Fw.d, Ew.d or Dw.d in upper case, w from 1 to
    /// display_format::max_width and d below w. Spaces before and after are ignored.
    /// @throws invalid_data_error (errc::invalid_column_format) for text of no such form.
    [[nodiscard]] OPENFITS_API static column_format parse_ascii(std::string_view text);

    /// The TFORMn value that the parse functions read back as this format: rT, rAw, rPt(emax), or the Fortran format of
    /// an ASCII table.
    /// @throws usage_error (errc::invalid_argument) for a format that no TFORMn value writes: a field of another type
    ///         than character, integer, fixed, exponential and double_exponential, or a type that its table does not
    ///         have.
    [[nodiscard]] OPENFITS_API std::string to_string() const;

    /// The bytes of a field in a row: repeat x the size of the type, the bits rounded up to bytes, 8 for each P and 16
    /// for each Q descriptor (standard Table 18); the width of the field of an ASCII table.
    /// @throws usage_error (errc::arithmetic_overflow) when the size does not fit in 64 bits.
    [[nodiscard]] OPENFITS_API std::uint64_t field_size() const;

    friend bool operator==(const column_format&, const column_format&) = default;
};

/// A column of a table (standard §7.2.2, §7.3.2), as a reader describes it and as the writer takes it.
///
/// A reader gives every member that the header gives, and the offset of the field in the row. A model for the writer
/// gives the name and the other members that it wants written, the writer writes TTYPEn, TFORMn, TUNITn, TSCALn,
/// TZEROn, TNULLn, TDISPn and TDIMn from them, and computes the offset; format may be empty, to store the values in
/// their own type, with the conventions of standard §5.2.5 for the types that FITS stores with an offset, as images
/// are.
///
/// A value: copies are independent.
struct column_info
{
    /// TTYPEn, without trailing spaces; empty without it.
    std::string name{};
    /// TFORMn. A reader always gives it.
    std::optional<column_format> format{};
    /// TDIMn: the dimensions of the array of a field, the first varying fastest (standard §7.3.2), and for the rAw
    /// convention (w, r / w); empty without them.
    std::vector<std::uint64_t> dimensions{};
    /// TSCALn and TZEROn (standard §7.3.2, Eq. 7): the identity when the header has neither.
    openfits::scaling scaling{};
    /// TNULLn of an integer column of a binary table, or of the integer arrays of a P or Q column: the stored value of
    /// undefined elements, before scaling.
    std::optional<std::int64_t> null{};
    /// TNULLn of an ASCII table: the text of undefined fields, implicitly filled with spaces to the width of the field.
    std::optional<std::string> null_string{};
    /// TUNITn, without trailing spaces.
    std::string unit{};
    /// TDISPn, the recommended display format (standard §7.3.4).
    std::optional<display_format> display{};
    /// The offset of the field in a row, in bytes: TBCOLn - 1 in an ASCII table. The writer computes it.
    std::uint64_t offset = 0;

    /// The type that holds every physical value of a numeric column without a loss, which a read gives without a
    /// conversion that loses anything: the stored type for floating-point data and unscaled integers, for integers
    /// whose TSCALn and TZEROn are both integers the narrowest integer type that holds every value they give (the
    /// conventions of standard §5.2.5: I with TZEROn 32768 is std::uint16_t, B with TZEROn -128 std::int8_t), and
    /// otherwise float for B and I and double for J and K, as image_info::value_type() gives; std::uint8_t for logical
    /// and bit columns, which read as 0 and 1; and int64 or float64 for the fields of an ASCII table. The parts of
    /// complex columns take float and double.
    /// @throws usage_error (errc::invalid_argument) when format is empty, or the column holds characters.
    [[nodiscard]] OPENFITS_API numeric_type value_type() const;

    friend bool operator==(const column_info&, const column_info&) = default;
};

/// A table: an ASCII table or a binary table (standard §7.2, §7.3), as a reader describes it and as the writer takes
/// it. A reader gives every member; a model for the writer gives the kind, the rows, the columns and the cards of the
/// header, none of them structural, and the writer computes the others.
///
/// A value: copies are independent.
struct table_info
{
    /// hdu_kind::table for an ASCII table, hdu_kind::bintable for a binary table.
    hdu_kind kind = hdu_kind::bintable;
    /// NAXIS2.
    std::uint64_t rows = 0;
    std::vector<column_info> columns{};
    openfits::header header{};
    /// NAXIS1: the bytes of a row.
    std::uint64_t row_size = 0;
    /// The offset of the heap from the start of the data: THEAP, or NAXIS1 x NAXIS2 without it (standard §7.3.5).
    std::uint64_t heap_offset = 0;
    /// The bytes of the heap: PCOUNT less the gap between the rows and the heap.
    std::uint64_t heap_size = 0;

    /// The index of the first column whose name is name, compared without regard to case and to trailing spaces, as
    /// standard §7.2.2 and §7.3.2 ask, or exactly; nothing when no column has it.
    [[nodiscard]] OPENFITS_API std::optional<std::size_t> find_column(std::string_view name, bool exact = false) const;

    /// The indices of the columns whose names match pattern, in their order: ? matches one character, * any run of
    /// characters, and # a run of digits; letters in either case, unless exact. A name equal to pattern matches it.
    [[nodiscard]] OPENFITS_API std::vector<std::size_t> matching_columns(std::string_view pattern,
                                                                         bool exact = false) const;

    friend bool operator==(const table_info&, const table_info&) = default;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/// The model of a table read from a file, for the writer: table with the structural cards of its header taken out
/// (without_structural()), its kind, rows and columns kept, the format, dimensions, scaling, null and display of each,
/// so that a table read is written again in one line: writer.add_table(without_structural(file.table(index)), values).
[[nodiscard]] OPENFITS_API table_info without_structural(table_info table);

/// The rows of a table that a read takes: count rows from the row first, counted from 0; the rest of the table when
/// count is empty.
struct row_range
{
    std::uint64_t first = 0;
    std::optional<std::uint64_t> count{};

    friend bool operator==(const row_range&, const row_range&) = default;
};

/// The types that the elements of columns are read into: those of numeric_value, and the complex numbers of float and
/// double. Logical and bit columns read as numbers, 0 and 1.
template <typename T>
concept column_element =
    numeric_value<T> || std::same_as<T, std::complex<float>> || std::same_as<T, std::complex<double>>;

/// The types that column_element names, for the functions that take any of them.
enum class element_type : std::uint8_t
{
    int8,
    uint8,
    int16,
    uint16,
    int32,
    uint32,
    int64,
    uint64,
    float32,
    float64,
    complex_float32, ///< std::complex<float>
    complex_float64, ///< std::complex<double>
};

/// The element_type of T.
template <column_element T> [[nodiscard]] constexpr element_type element_type_of() noexcept
{
    if constexpr (std::same_as<T, std::complex<float>>) {
        return element_type::complex_float32;
    } else if constexpr (std::same_as<T, std::complex<double>>) {
        return element_type::complex_float64;
    } else {
        return static_cast<element_type>(numeric_type_of<T>());
    }
}

/// The arrays of the cells of a column, as a read of variable-length arrays (standard §7.3.5) gives them: the elements
/// of every row one after the other, and where those of each row start.
///
/// A value: copies are independent.
template <column_element T> struct column_arrays
{
    std::vector<T> values{};
    /// The index in values of the first element of each row, and after them the number of values: one more than the
    /// rows.
    std::vector<std::uint64_t> offsets{};
    /// With blank_policy::mask, a byte for each value, 1 for an undefined one; empty otherwise.
    std::vector<std::uint8_t> mask{};

    /// The number of rows.
    [[nodiscard]] std::size_t size() const noexcept
    {
        return offsets.empty() ? 0 : offsets.size() - 1;
    }

    /// The elements of the row at index, which must be below size().
    [[nodiscard]] std::span<const T> operator[](std::size_t index) const noexcept
    {
        return std::span<const T>(values).subspan(offsets[index], offsets[index + 1] - offsets[index]);
    }
};

namespace detail {
struct column_values_access;
} // namespace detail

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

/// The values of a column given to the writer, borrowed until its last save(), which never modifies them: numbers of
/// any of the types of numeric_value, complex numbers, strings, logical values or bits, each row with the elements of
/// its field, or, for variable-length arrays (standard §7.3.5), each row with as many as it has.
///
/// Fixed fields take the elements of every row one after the other: rows x the elements of a field. A string fills
/// the characters of a field, or one of its substrings when its column has dimensions, padded with spaces. A
/// variable-length column takes the elements of every row and offsets, as column_arrays holds them.
class OPENFITS_API column_values
{
public:
    /// Numbers, stored in their own type unless the format of the column says otherwise.
    template <numeric_value T> column_values(std::span<const T> values) noexcept : column_values(numbers_of(values)) {}

    /// Numbers that are not const, such as std::span(values) of a std::vector that is not const.
    template <numeric_value T>
    column_values(std::span<T> values) noexcept : column_values(numbers_of(std::span<const T>(values)))
    {}

    column_values(std::span<const std::complex<float>> values) noexcept;
    column_values(std::span<const std::complex<double>> values) noexcept;

    /// Complex numbers that are not const.
    column_values(std::span<std::complex<float>> values) noexcept
        : column_values(std::span<const std::complex<float>>(values))
    {}

    column_values(std::span<std::complex<double>> values) noexcept
        : column_values(std::span<const std::complex<double>>(values))
    {}

    /// Strings: one for each field, or for each substring of a field of a column with dimensions.
    column_values(std::span<const std::string> values) noexcept;

    /// Strings that are not const.
    column_values(std::span<std::string> values) noexcept : column_values(std::span<const std::string>(values)) {}

    /// Logical values (type L): 0 as F and any other value as T; an element whose byte in undefined_mask is not 0 is
    /// written as undefined, a 0 byte (standard §7.3.3.1). undefined_mask is empty or has a byte for each value.
    [[nodiscard]] static column_values logicals(std::span<const std::uint8_t> values,
                                                std::span<const std::uint8_t> undefined_mask = {}) noexcept;

    /// Bits (type X): one value for each bit, 0 or any other value for 1, the first the most significant bit of its
    /// field.
    [[nodiscard]] static column_values bits(std::span<const std::uint8_t> values) noexcept;

    /// Variable-length arrays of numbers: the elements of row i are values[offsets[i]] to values[offsets[i + 1] - 1],
    /// and offsets has one entry more than the rows.
    template <numeric_value T>
    [[nodiscard]] static column_values arrays(std::span<const T> values,
                                              std::span<const std::uint64_t> offsets) noexcept
    {
        column_values result = numbers_of(values);
        result.offsets_ = offsets;
        result.variable_ = true;
        return result;
    }

    /// Variable-length arrays of numbers that are not const, such as std::span(values) of a std::vector that is not
    /// const.
    template <numeric_value T>
    [[nodiscard]] static column_values arrays(std::span<T> values, std::span<const std::uint64_t> offsets) noexcept
    {
        return arrays(std::span<const T>(values), offsets);
    }

    /// Variable-length arrays of complex numbers, as arrays() of numbers takes them.
    [[nodiscard]] static column_values arrays(std::span<const std::complex<float>> values,
                                              std::span<const std::uint64_t> offsets) noexcept;
    [[nodiscard]] static column_values arrays(std::span<const std::complex<double>> values,
                                              std::span<const std::uint64_t> offsets) noexcept;

    /// Variable-length arrays of characters: a string for each row, an empty one written as one NUL byte, as CFITSIO
    /// writes it, since its string reads refuse an empty array; a read gives it back empty.
    [[nodiscard]] static column_values arrays(std::span<const std::string> values) noexcept;

private:
    friend struct detail::column_values_access;

    // What the values are.
    enum class kind : std::uint8_t
    {
        numbers,
        complex_float32,
        complex_float64,
        strings,
        logicals,
        bits,
    };

    column_values() noexcept = default;

    template <numeric_value T> static column_values numbers_of(std::span<const T> values) noexcept
    {
        column_values result;
        result.kind_ = kind::numbers;
        result.type_ = numeric_type_of<T>();
        result.bytes_ = std::as_bytes(values);
        result.count_ = values.size();
        return result;
    }

    kind kind_ = kind::numbers;
    numeric_type type_ = numeric_type::uint8;
    // The values, for every kind but strings.
    std::span<const std::byte> bytes_{};
    std::span<const std::string> strings_{};
    std::span<const std::uint8_t> undefined_{};
    // The offsets of variable-length arrays; empty for fixed fields.
    std::span<const std::uint64_t> offsets_{};
    bool variable_ = false;
    // The number of values: elements, complex numbers, strings, logicals or bits.
    std::size_t count_ = 0;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace openfits
