// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/card.h>
#include <openfits/export.h>

#include <complex>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// @file
/// Headers: the keyword records of a header and data unit in their order, read as typed values and changed.

namespace openfits {

/// The types that header::value(), header::find() and header::values() give the value of a keyword in: bool, an integer
/// type other than the character types, float, double, std::string and std::complex<double>.
template <typename T>
concept header_value =
    std::same_as<T, bool> || std::same_as<T, float> || std::same_as<T, double> || std::same_as<T, std::string> ||
    std::same_as<T, std::complex<double>> ||
    (std::integral<T> && !std::same_as<T, bool> && !std::same_as<T, char> && !std::same_as<T, wchar_t> &&
     !std::same_as<T, char8_t> && !std::same_as<T, char16_t> && !std::same_as<T, char32_t>);

#if defined(_MSC_VER)
// The header holds standard-library members, which cl reports for an exported class. A C++ interface requires the same
// standard library on both sides of the DLL boundary anyway.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

namespace detail {
struct header_access;
struct keyword_table;
} // namespace detail

/// The keyword records of a header (standard §4) in their order, the END record excluded, each a card.
///
/// A card read from a file keeps the record it was read from, so that a header is written back unchanged; a card that
/// a change makes has none, and the writer formats it. A long string continued over CONTINUE records (standard
/// §4.2.1.2) is a card for each record, as in the file, and value() and find() give the string they make together.
///
/// Lookups take a keyword in any case, with or without the HIERARCH prefix, and find the first card of that keyword;
/// a header is indexed by keyword, so that a lookup takes about the same time whatever its size. The blank cards that
/// end a header are its fill space (standard §4.4.2.4), which free_cards() counts and which a card added by append() or
/// set() takes the place of.
///
/// Values convert as the table of value() says. A header is a value: copies are independent, and any number of threads
/// can read one at once, while a change needs exclusive access.
class OPENFITS_API header
{
public:
    using const_iterator = std::vector<card>::const_iterator;

    header() noexcept;

    /// A header of the given cards, in their order.
    explicit header(std::vector<card> cards);

    header(const header& other);
    /// Copies other. When the copy throws, this header is left as it was.
    header& operator=(const header& other);
    header(header&& other) noexcept;
    header& operator=(header&& other) noexcept;
    ~header();

    /// The number of cards.
    [[nodiscard]] std::size_t size() const noexcept
    {
        return cards_.size();
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return cards_.empty();
    }

    /// The card at index, which must be below size().
    [[nodiscard]] const card& operator[](std::size_t index) const noexcept
    {
        return cards_[index];
    }

    /// The card at index.
    /// @throws usage_error (errc::invalid_argument) when index is not below size().
    [[nodiscard]] const card& at(std::size_t index) const;

    [[nodiscard]] const_iterator begin() const noexcept
    {
        return cards_.begin();
    }

    [[nodiscard]] const_iterator end() const noexcept
    {
        return cards_.end();
    }

    /// The cards, in their order.
    [[nodiscard]] std::span<const card> cards() const noexcept
    {
        return cards_;
    }

    /// True when a card has the keyword.
    [[nodiscard]] bool contains(std::string_view keyword) const;

    /// The index of the first card of the keyword, or nothing.
    [[nodiscard]] std::optional<std::size_t> index_of(std::string_view keyword) const;

    /// The number of cards of the keyword, which a commentary keyword such as HISTORY has any number of.
    [[nodiscard]] std::size_t count(std::string_view keyword) const;

    /// The indices of the cards whose keywords match pattern, in their order: ? matches one character, * any run of
    /// characters, and # a run of digits; letters match in either case.
    [[nodiscard]] std::vector<std::size_t> matching(std::string_view pattern) const;

    /// The comment of the first card of the keyword, with those of the CONTINUE records that continue its string
    /// (standard §4.2.1.2); the text of a commentary card; nothing when no card has the keyword.
    [[nodiscard]] std::optional<std::string> comment(std::string_view keyword) const;

    /// The value of the first card of the keyword, converted to T:
    ///
    /// | Value | Integer T | Floating-point T | bool | std::string | std::complex<double> |
    /// |---|---|---|---|---|---|
    /// | Integer | when T holds it | converted, exact up to 2^53 | refused | its text | the real part |
    /// | Real | when integral and T holds it | converted, a float in its range | refused | its text | the real part |
    /// | Logical | refused | refused | the value | T or F | refused |
    /// | String | its text read as a number, then as above | the same | 'T' and 'F' | the string | refused |
    /// | Complex | refused | refused | refused | its text | the value |
    ///
    /// The string of a keyword continued over CONTINUE records is the whole string (standard §4.2.1.2). A value that
    /// does not parse, kept as text with a warning when it was read, is only that text, as a std::string.
    /// @throws invalid_data_error with errc::keyword_not_found when no card has the keyword, errc::keyword_undefined
    ///         when its value is undefined, errc::keyword_type when the value does not convert to T or the card has no
    ///         value, and errc::value_out_of_range when T cannot hold it.
    template <header_value T> [[nodiscard]] T value(std::string_view keyword) const
    {
        std::optional<T> converted = typed<T>(keyword, true);
        if (!converted) {
            throw_not_found(keyword);
        }
        return std::move(*converted);
    }

    /// The value of the first card of the keyword as value() converts it, or nothing when no card has the keyword or
    /// its value is undefined.
    /// @throws invalid_data_error as value() does for a value that does not convert.
    template <header_value T> [[nodiscard]] std::optional<T> find(std::string_view keyword) const
    {
        return typed<T>(keyword, false);
    }

    /// The values of the indexed keywords from root with the indices first to last, as value() converts them:
    /// values<std::int64_t>("NAXIS", 1, 3) gives NAXIS1, NAXIS2 and NAXIS3. Nothing when first is beyond last.
    /// @throws invalid_data_error as value() does.
    template <header_value T>
    [[nodiscard]] std::vector<T> values(std::string_view root, std::uint64_t first, std::uint64_t last) const
    {
        std::vector<T> result;
        for (std::uint64_t index = first; index <= last; ++index) {
            result.push_back(value<T>(std::string(root) + std::to_string(index)));
            if (index == std::numeric_limits<std::uint64_t>::max()) {
                break;
            }
        }
        return result;
    }

    /// Gives the first card of the keyword the value, and keeps its comment; a keyword that no card has is added as
    /// append() adds a card. The CONTINUE records that continued its string are removed: a long string is one card,
    /// which the writer continues over records again.
    /// @throws usage_error (errc::invalid_keyword) for COMMENT, HISTORY, the blank keyword, CONTINUE and END.
    void set(std::string_view keyword, card_value value);

    /// As set(keyword, value), with the comment.
    void set(std::string_view keyword, card_value value, std::string_view comment);

    /// Adds a card after the last one that is not fill, in the place of the first blank card of the fill space when
    /// the header ends with one; a blank card added goes to the end, and is fill.
    void append(card value);

    /// Inserts a card before the card at position, which is not beyond size().
    /// @throws usage_error (errc::invalid_argument) when position is beyond size().
    void insert(std::size_t position, card value);

    /// Removes the first card of the keyword, and the CONTINUE records that continue its string. False when no card has
    /// the keyword.
    bool erase(std::string_view keyword);

    /// Removes the card at index, alone.
    /// @throws usage_error (errc::invalid_argument) when index is not below size().
    void erase_at(std::size_t index);

    /// Gives the first card of the keyword from the keyword to, with its value and comment: a record read from a file
    /// keeps its bytes after the name when both names are of standard §4.1.2.1. False when no card has the keyword
    /// from.
    /// @throws usage_error (errc::invalid_keyword) when to is CONTINUE or END, which the writer makes itself.
    bool rename(std::string_view from, std::string_view to);

    /// Adds the text as COMMENT cards, as append() adds cards: one for each 72 characters or fewer, and one for an
    /// empty text.
    void add_comment(std::string_view text);

    /// Adds the text as HISTORY cards, as add_comment() adds COMMENT cards.
    void add_history(std::string_view text);

    /// The number of blank cards that end the header: the fill space that new keywords may take (standard §4.4.2.4).
    [[nodiscard]] std::size_t free_cards() const noexcept;

    /// The records of the header, 80 characters each, the fill space left out, followed by an END record: with
    /// commentary false, the COMMENT, HISTORY and blank cards are left out too, as a parser of world coordinates wants
    /// the text. A card made by a change is formatted as the writer would, its name with the HIERARCH convention when
    /// it needs it, and its comment cut where it does not fit.
    /// @throws validation_error for a card that no record can hold.
    [[nodiscard]] std::string to_string(bool commentary = true) const;

    /// The number of CONTINUE records after the card at index that continue its string (standard §4.2.1.2): 0 for a
    /// card whose string does not end with an ampersand, or that holds no string.
    /// @throws usage_error (errc::invalid_argument) when index is not below size().
    [[nodiscard]] std::size_t continuation_count(std::size_t index) const;

    /// The string of the card at index with those of the CONTINUE records that continue it, as value() joins the string
    /// of the first card of a keyword, for a keyword that a header holds more than once; nothing when the card holds no
    /// string.
    /// @throws usage_error (errc::invalid_argument) when index is not below size().
    [[nodiscard]] std::optional<std::string> string_at(std::size_t index) const;

    /// The comment of the card at index with those of the CONTINUE records that continue its string, as comment() joins
    /// that of the first card of a keyword.
    /// @throws usage_error (errc::invalid_argument) when index is not below size().
    [[nodiscard]] std::string comment_at(std::size_t index) const;

    friend bool operator==(const header& left, const header& right)
    {
        return left.cards_ == right.cards_;
    }

private:
    friend struct detail::header_access;

    template <typename T> [[nodiscard]] std::optional<T> typed(std::string_view keyword, bool required) const
    {
        if constexpr (std::same_as<T, bool>) {
            return logical_value(keyword, required);
        } else if constexpr (std::same_as<T, std::string>) {
            return string_value(keyword, required);
        } else if constexpr (std::same_as<T, std::complex<double>>) {
            return complex_value(keyword, required);
        } else if constexpr (std::same_as<T, double>) {
            return real_value(keyword, required);
        } else if constexpr (std::same_as<T, float>) {
            const std::optional<double> real = real_value(keyword, required);
            if (!real) {
                return std::nullopt;
            }
            if (*real > static_cast<double>(std::numeric_limits<float>::max()) ||
                *real < static_cast<double>(std::numeric_limits<float>::lowest())) {
                throw_out_of_range(keyword, "float");
            }
            return static_cast<float>(*real);
        } else if constexpr (std::signed_integral<T>) {
            const std::optional<std::int64_t> integer = signed_value(keyword, required);
            if (!integer) {
                return std::nullopt;
            }
            if (!std::in_range<T>(*integer)) {
                throw_out_of_range(keyword, "the integer type");
            }
            return static_cast<T>(*integer);
        } else {
            const std::optional<std::uint64_t> integer = unsigned_value(keyword, required);
            if (!integer) {
                return std::nullopt;
            }
            if (!std::in_range<T>(*integer)) {
                throw_out_of_range(keyword, "the integer type");
            }
            return static_cast<T>(*integer);
        }
    }

    [[nodiscard]] std::optional<bool> logical_value(std::string_view keyword, bool required) const;
    [[nodiscard]] std::optional<std::int64_t> signed_value(std::string_view keyword, bool required) const;
    [[nodiscard]] std::optional<std::uint64_t> unsigned_value(std::string_view keyword, bool required) const;
    [[nodiscard]] std::optional<double> real_value(std::string_view keyword, bool required) const;
    [[nodiscard]] std::optional<std::string> string_value(std::string_view keyword, bool required) const;
    [[nodiscard]] std::optional<std::complex<double>> complex_value(std::string_view keyword, bool required) const;
    [[noreturn]] static void throw_not_found(std::string_view keyword);
    [[noreturn]] static void throw_out_of_range(std::string_view keyword, std::string_view type);

    // The index of the first card of each canonical keyword, made for a header of more cards than a search in order
    // takes as quickly, and kept as cards are added.
    void rebuild_index();

    std::vector<card> cards_{};
    // On the heap, so that a header moves without allocating; empty for a header that is searched in order.
    std::unique_ptr<detail::keyword_table> index_{};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/// True for the keywords that the library writes from the model of a unit, and refuses in a header given to the
/// writer: SIMPLE, BITPIX, NAXIS and NAXISn, EXTEND, XTENSION, PCOUNT, GCOUNT, GROUPS, TFIELDS, THEAP, BSCALE, BZERO,
/// BLANK, the keywords of the columns of tables that the models of columns give (TTYPEn, TFORMn, TBCOLn, TUNITn,
/// TSCALn, TZEROn, TNULLn, TDISPn and TDIMn), the keywords of tiled compression (standard §10), CHECKSUM, DATASUM,
/// CONTINUE and END. The keyword is taken in any case.
[[nodiscard]] OPENFITS_API bool is_structural(std::string_view keyword);

/// True for a keyword name of standard §4.1.2.1, as it is given: one to eight characters, each an upper-case letter, a
/// digit, a hyphen or an underscore. Any other name is written with the HIERARCH convention, on request.
[[nodiscard]] OPENFITS_API bool is_standard_keyword(std::string_view keyword) noexcept;

/// The cards of a header but the structural ones, which the writer makes from the model of a unit: what a header read
/// from a file gives to the header of a new unit. The CONTINUE records that continue a string go with its card, and an
/// orphaned CONTINUE record, which is commentary, stays.
[[nodiscard]] OPENFITS_API header without_structural(const header& cards);

} // namespace openfits
