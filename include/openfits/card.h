// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/export.h>

#include <array>
#include <chrono>
#include <complex>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

/// @file
/// Keyword records and their values (standard §4.1, §4.2), dates (standard §4.2.7, §9.1.1) and display formats
/// (standard §7.3.4).

namespace openfits {

/// The undefined value of a keyword: a value indicator followed by blanks (standard §4.1.2.3).
struct undefined_value
{
    friend bool operator==(undefined_value, undefined_value) = default;
};

/// A complex integer value (standard §4.2.5).
struct complex_integer
{
    std::int64_t real = 0;
    std::int64_t imag = 0;

    friend bool operator==(const complex_integer&, const complex_integer&) = default;
};

/// The kinds of keyword values (standard §4.2).
enum class value_kind : std::uint8_t
{
    none,            ///< No value: a commentary keyword, or a value field that holds none of the forms of the standard.
    undefined,       ///< A value indicator followed by blanks.
    logical,         ///< T or F, held in a bool.
    integer,         ///< Held in a std::int64_t, or in a std::uint64_t from 2^63 to 2^64 - 1.
    real,            ///< Held in a double.
    complex_integer, ///< Held in a complex_integer.
    complex_real,    ///< Held in a std::complex<double>.
    string,          ///< Held in a std::string, without its quotes and its trailing spaces.
};

// After value_kind, whose enumerator of the same name GCC's -Wshadow would otherwise take for a shadow of it.
/// The undefined value, for card_value and the setters of headers.
inline constexpr undefined_value undefined{};

#if defined(_MSC_VER)
// card_value and card hold standard-library members, which cl reports for an exported class. A C++ interface requires
// the same standard library on both sides of the DLL boundary anyway.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

/// The value of a keyword (standard §4.2): one of the kinds of value_kind, in the C++ type that holds it.
///
/// An integer is held in a std::int64_t, and in a std::uint64_t only from 2^63 to 2^64 - 1, which the 64-bit unsigned
/// convention needs for BZERO and TZEROn (standard §5.2.5): the same integer is always held the same way. The
/// constructors take the C++ type of a value: bool makes a logical, an integer type an integer, float and double a
/// real, text a string. The character types are refused, so that 'T' cannot make the integer 84, and a float is held
/// as the double of its shortest decimal form, so that 0.1F makes the value 0.1, which reads back as the same float. A
/// default-constructed value has no value (value_kind::none).
class OPENFITS_API card_value
{
public:
    /// Every value, as the C++ type that holds it.
    using storage = std::variant<std::monostate, undefined_value, bool, std::int64_t, std::uint64_t, double,
                                 complex_integer, std::complex<double>, std::string>;

    card_value() = default;
    card_value(const card_value& other);
    card_value(card_value&&) noexcept = default;

    /// Copies other. When the copy throws, this value is left as it was.
    card_value& operator=(const card_value& other)
    {
        card_value copy(other);
        *this = std::move(copy);
        return *this;
    }

    card_value& operator=(card_value&&) noexcept = default;
    ~card_value() = default;

    card_value(undefined_value value) noexcept;
    card_value(bool value) noexcept;
    card_value(std::int64_t value) noexcept;
    card_value(std::uint64_t value) noexcept;

    /// An integer of any other integer type, such as int or unsigned long long: held as the std::int64_t or
    /// std::uint64_t of its value.
    template <typename T>
        requires std::integral<T> && (!std::same_as<T, bool>) && (!std::same_as<T, char>) &&
                 (!std::same_as<T, wchar_t>) && (!std::same_as<T, char8_t>) && (!std::same_as<T, char16_t>) &&
                 (!std::same_as<T, char32_t>) && (!std::same_as<T, std::int64_t>) && (!std::same_as<T, std::uint64_t>)
    card_value(T value) noexcept : card_value(from_integer(value))
    {}

    card_value(char) = delete;
    card_value(wchar_t) = delete;
    card_value(char8_t) = delete;
    card_value(char16_t) = delete;
    card_value(char32_t) = delete;

    /// A real, held as the double of the shortest decimal form of value. A NaN or an infinity is held as it is, and
    /// refused when it is written, since no keyword value can hold it (standard §4.2.4).
    card_value(float value);
    card_value(double value) noexcept;

    card_value(complex_integer value) noexcept;
    card_value(std::complex<float> value);
    card_value(std::complex<double> value) noexcept;

    /// A string, as it is: its trailing spaces are not significant (standard §4.2.1.1), and are not written.
    card_value(std::string value) noexcept;
    card_value(std::string_view value);
    card_value(const char* value);
    card_value(std::nullptr_t) = delete;

    /// The kind of the value.
    [[nodiscard]] value_kind kind() const noexcept
    {
        // The alternatives of storage, in their order. The variant is never valueless: the copy of an alternative is
        // made before it replaces the old one, and every alternative moves without throwing.
        static_assert(std::variant_size_v<storage> == 9);
        switch (value_.index()) {
        case 0:
            return value_kind::none;
        case 1:
            return value_kind::undefined;
        case 2:
            return value_kind::logical;
        case 3:
        case 4:
            return value_kind::integer;
        case 5:
            return value_kind::real;
        case 6:
            return value_kind::complex_integer;
        case 7:
            return value_kind::complex_real;
        default:
            return value_kind::string;
        }
    }

    /// The value, which must be held in T: get<double>() for a real, get<std::string>() for a string,
    /// get<std::int64_t>() or get<std::uint64_t>() for an integer, by its range.
    /// @throws usage_error when the value is held in another type.
    template <typename T> [[nodiscard]] const T& get() const
    {
        if (const T* value = std::get_if<T>(&value_)) {
            return *value;
        }
        throw_type_mismatch();
    }

    /// The value, as the C++ type that holds it, for std::visit.
    [[nodiscard]] const storage& data() const noexcept
    {
        return value_;
    }

    friend bool operator==(const card_value&, const card_value&) = default;

private:
    template <typename T> static card_value from_integer(T value) noexcept
    {
        if constexpr (std::is_signed_v<T>) {
            return {static_cast<std::int64_t>(value)};
        } else {
            return {static_cast<std::uint64_t>(value)};
        }
    }

    [[noreturn]] static void throw_type_mismatch();

    storage value_{};
};

namespace detail {
struct card_access;
} // namespace detail

/// One keyword record of a header (standard §4.1): a keyword, a value, and a comment, or the text of a commentary
/// keyword.
///
/// A card read from a file keeps the record it was read from, which text() gives, so that a header is written back
/// unchanged; a card built by the application has none, and the writer formats it under the options of the save, a card
/// of with_value_text() with its value written as the text it was given. Values and copies own their data.
class OPENFITS_API card
{
public:
    /// A blank card: the blank keyword, without value or text.
    card() = default;

    /// A keyword with a value and a comment. The keyword is kept in its canonical form, the form a lookup uses: in
    /// upper case, without the HIERARCH prefix of the convention, with every run of spaces made one space. The
    /// keyword, the value and the comment are checked when the card is written.
    card(std::string_view keyword, card_value value, std::string_view comment = {});

    /// A commentary card (standard §4.4.2.4): COMMENT, HISTORY or the blank keyword, or any keyword without a value
    /// indicator, with its text, bytes 9 through 80 of the record.
    [[nodiscard]] static card commentary(std::string_view keyword, std::string_view text);

    /// Parses one keyword record (standard §4.1, Appendix A), padded with spaces to 80 bytes when it is shorter, as a
    /// reader reads it: a departure from the standard that leaves the meaning clear is read as intended, and a value
    /// field that holds none of the forms of the standard gives no value, its text kept in value_text().
    /// @throws usage_error when record is longer than 80 bytes.
    [[nodiscard]] static card parse(std::string_view record);

    /// A keyword with its value given as the text of a value field, which its record holds as it is ("1.50E+03", a
    /// string with its quotes), and a comment. The card has no record: the writer makes one under the options of the
    /// save, as for any card that the application builds, with the text from byte 11, right-justified to byte 30 when
    /// it is not a string and fits there; a name that is not of standard §4.1.2.1 written with the HIERARCH convention
    /// and a comment that does not fit cut, when the options allow it, and a string that does not fit continued over
    /// CONTINUE records, as the string that value() holds. value() is the value that the text gives, as a reader reads
    /// it, and none for a text of none of the forms of standard §4.2; the writer refuses such a text, and one with a
    /// departure that a reader tolerates, such as a lowercase exponent letter. value_text() is the text without its
    /// leading and trailing spaces.
    /// @throws usage_error (errc::invalid_keyword) for a name that has no value: COMMENT, HISTORY, the blank keyword,
    ///         CONTINUE and END.
    [[nodiscard]] static card with_value_text(std::string_view keyword, std::string_view value_text,
                                              std::string_view comment = {});

    /// The keyword, in its canonical form: in upper case, with the spaces of a name of the HIERARCH convention made
    /// single.
    [[nodiscard]] std::string_view keyword() const noexcept
    {
        return keyword_;
    }

    /// True when the record has a value indicator, so that the keyword has a value, maybe undefined.
    [[nodiscard]] bool has_value_indicator() const noexcept
    {
        return value_indicator_;
    }

    /// The value. A conforming CONTINUE record (standard §4.2.1.2) has the string it continues with, without a value
    /// indicator, and a commentary card has none.
    [[nodiscard]] const card_value& value() const noexcept
    {
        return value_;
    }

    /// The value as the record writes it, a string with its quotes, or as with_value_text() was given it; empty for
    /// another card built by the application.
    [[nodiscard]] std::string_view value_text() const noexcept
    {
        if (!record_) {
            return text_;
        }
        return {record_text_.data() + value_begin_, static_cast<std::size_t>(value_end_ - value_begin_)};
    }

    /// The comment, without the slash that starts it, the space after the slash, and trailing spaces; the text of a
    /// commentary card.
    [[nodiscard]] std::string_view comment() const noexcept
    {
        return comment_;
    }

    /// The units of the value that the comment gives in square brackets at its start (standard §4.3.2), as in
    /// "/ [s] exposure time", without the brackets; nothing when it does not start with them.
    [[nodiscard]] std::optional<std::string_view> unit() const noexcept;

    /// The record that the card was read from, 80 characters; empty for a card built by the application,
    /// with_value_text() included.
    [[nodiscard]] std::string_view text() const noexcept
    {
        return record_ ? std::string_view(record_text_.data(), record_text_.size()) : std::string_view();
    }

    friend bool operator==(const card&, const card&) = default;

private:
    friend struct detail::card_access;

    std::string keyword_{};
    card_value value_{};
    std::string comment_{};
    // The value text of a card of with_value_text().
    std::string text_{};
    // The record that the card was read from, when record_ says that it has one: in the card itself, so that the cards
    // of a header read from a file make no allocation for their records.
    std::array<char, 80> record_text_{};
    std::uint8_t value_begin_ = 0;
    std::uint8_t value_end_ = 0;
    bool value_indicator_ = false;
    bool fixed_format_ = false;
    bool record_ = false;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/// A date, and optionally a time of day, as a datetime string writes it (standard §9.1.1): the values of DATE,
/// DATE-OBS and the other date keywords (standard §4.4.2.1, §4.4.2.2). The calendar is the Gregorian one, proleptic
/// before 1582, with the year 0 that ISO 8601 counts (1 BCE). No time scale is implied.
struct date_time
{
    int year = 2000; ///< -99999 to 99999, written with four digits from 0 to 9999 and with a sign and five otherwise.
    unsigned month = 1;           ///< 1 to 12.
    unsigned day = 1;             ///< 1 to the length of the month.
    bool has_time = false;        ///< False for a date alone, YYYY-MM-DD; the members below are then zero.
    unsigned hour = 0;            ///< 0 to 23.
    unsigned minute = 0;          ///< 0 to 59.
    unsigned second = 0;          ///< 0 to 60, where 60 is a leap second of UTC.
    std::uint32_t nanosecond = 0; ///< 0 to 999,999,999.

    friend bool operator==(const date_time&, const date_time&) = default;
};

/// Parses a datetime string (standard §9.1.1): YYYY-MM-DD or [+-]YYYYY-MM-DD, optionally followed by Thh:mm:ss and a
/// decimal fraction of the second of any length, of which nanoseconds are kept; or the form DD/MM/YY of files written
/// before 2000 (standard §4.4.2.1), a year of 1900 to 1999. Leading zeros cannot be omitted, and a time zone cannot be
/// given.
/// @throws invalid_data_error with errc::invalid_date when text is not such a date, or names a date or time that does
/// not exist.
[[nodiscard]] OPENFITS_API date_time parse_date(std::string_view text);

/// The datetime string of value (standard §9.1.1): YYYY-MM-DD, followed by Thh:mm:ss when value has a time, and by the
/// fraction of the second without trailing zeros when it is not zero. The form DD/MM/YY is never written.
/// @throws usage_error with errc::invalid_date when a member of value is out of its range.
[[nodiscard]] OPENFITS_API std::string format_date(const date_time& value);

/// The date and time of a time point of the system clock, in UTC; the library reads no clock. The system clock counts
/// no leap seconds, so the second is never 60. Give std::chrono::floor<std::chrono::seconds>(time) for a value in
/// whole seconds.
[[nodiscard]] OPENFITS_API date_time to_date_time(std::chrono::sys_time<std::chrono::nanoseconds> time) noexcept;

/// The datetime string of a time point of the system clock, as format_date(to_date_time(time)) gives it: the value of
/// a DATE keyword, which is in UTC (standard §4.4.2.1).
[[nodiscard]] OPENFITS_API std::string format_date(std::chrono::sys_time<std::chrono::nanoseconds> time);

/// The format codes of the display formats of table columns (standard §7.3.4, Tables 16 and 20).
enum class display_code : std::uint8_t
{
    character,          ///< Aw, character data.
    logical,            ///< Lw, logical data.
    integer,            ///< Iw.m, integers in decimal.
    binary,             ///< Bw.m, integers in binary.
    octal,              ///< Ow.m, integers in octal.
    hexadecimal,        ///< Zw.m, integers in hexadecimal.
    fixed,              ///< Fw.d, reals in fixed decimal notation.
    exponential,        ///< Ew.dEe, reals in exponential notation, the fraction from 0.1 to 1.
    engineering,        ///< ENw.d, reals in exponential notation with an exponent that is a multiple of three.
    scientific,         ///< ESw.d, reals in exponential notation, the fraction from 1 to 10.
    general,            ///< Gw.dEe, reals as Fw.d when they fit, as Ew.dEe otherwise; other data as Iw, Lw or Aw.
    double_exponential, ///< Dw.dEe, as Ew.dEe with the exponent letter D.
};

/// A display format of a table column, the value of a TDISPn keyword (standard §7.3.4): a format code, a width, and
/// the digits that the code takes. The codes are interpreted as the output formats of Fortran that they are.
struct display_format
{
    /// The widest field that a display format can have: wider formats are refused, so that rendering a value never
    /// allocates more than this.
    static constexpr std::uint32_t max_width = std::uint32_t{1} << 20U;

    display_code code = display_code::general;
    std::uint32_t width = 1; ///< w, at least 1.
    /// m, the minimum number of digits, for integer, binary, octal and hexadecimal (1 when absent); d, the number of
    /// digits after the decimal point, for the codes of reals (required). Absent for character and logical.
    std::optional<std::uint32_t> digits{};
    /// e, the number of digits of the exponent, for exponential, general and double_exponential (2 when absent).
    std::optional<std::uint32_t> exponent_digits{};

    /// Parses a TDISPn value, such as "F10.3", "I8" or "E15.7E3", ignoring leading and trailing spaces. The codes are
    /// in upper case.
    /// @throws invalid_data_error with errc::invalid_display_format when text is not a display format of standard
    /// §7.3.4, or its width is 0, beyond max_width, or too narrow for its digits or for the e + 2 characters of its
    /// exponent.
    [[nodiscard]] OPENFITS_API static display_format parse(std::string_view text);

    /// The TDISPn value of the format, which parse() reads back as the same format.
    [[nodiscard]] OPENFITS_API std::string to_string() const;

    friend bool operator==(const display_format&, const display_format&) = default;
};

/// A value rendered with a display format (standard §7.3.4), in a field of the width of the format, or of asterisks
/// when the value does not fit. Character data take character or general; logical data logical or general; integers
/// every code but character and logical; reals the codes of reals and general; complex values each part with a code of
/// reals, as "(real,imaginary)" in twice the width and three more characters. Reals are rounded to nearest, half away
/// from zero.
/// @throws invalid_data_error with errc::invalid_display_format when format is not one that display_format::parse()
/// accepts, or its code cannot display a value of that type.
[[nodiscard]] OPENFITS_API std::string format_display(const display_format& format, std::string_view value);
[[nodiscard]] OPENFITS_API std::string format_display(const display_format& format, bool value);
[[nodiscard]] OPENFITS_API std::string format_display(const display_format& format, std::int64_t value);
[[nodiscard]] OPENFITS_API std::string format_display(const display_format& format, std::uint64_t value);
[[nodiscard]] OPENFITS_API std::string format_display(const display_format& format, double value);
[[nodiscard]] OPENFITS_API std::string format_display(const display_format& format, std::complex<double> value);

/// A value of another integer type, rendered as its std::int64_t or std::uint64_t.
template <typename T>
    requires std::integral<T> && (!std::same_as<T, bool>) && (!std::same_as<T, char>) && (!std::same_as<T, wchar_t>) &&
             (!std::same_as<T, char8_t>) && (!std::same_as<T, char16_t>) && (!std::same_as<T, char32_t>) &&
             (!std::same_as<T, std::int64_t>) && (!std::same_as<T, std::uint64_t>)
[[nodiscard]] std::string format_display(const display_format& format, T value)
{
    if constexpr (std::is_signed_v<T>) {
        return format_display(format, static_cast<std::int64_t>(value));
    } else {
        return format_display(format, static_cast<std::uint64_t>(value));
    }
}

/// A float, rendered as the double it converts to.
[[nodiscard]] inline std::string format_display(const display_format& format, float value)
{
    return format_display(format, static_cast<double>(value));
}

/// Text, rendered as the std::string_view it is.
[[nodiscard]] inline std::string format_display(const display_format& format, const char* value)
{
    return format_display(format, std::string_view(value));
}

} // namespace openfits
