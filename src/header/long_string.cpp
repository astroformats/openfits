// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "header/long_string.h"

#include <openfits/error.h>

#include "core/ascii.h"
#include "core/keyword_name.h"
#include "core/quote.h"
#include "header/keywords.h"

#include <string_view>

namespace openfits::detail {

namespace {

// A CONTINUE record holds its string from byte 11, after the name and two spaces.
constexpr std::string_view continue_prefix = "CONTINUE  '";
constexpr std::string_view comment_separator = " / ";
// The characters between the quotes of a CONTINUE record, the ampersand that continues the string included.
constexpr std::size_t continued_room = record_length - continue_prefix.size() - 1;
// The records of null strings that hold a comment: continued by an ampersand, and the last one.
constexpr std::string_view continued_null = "CONTINUE  '&' / ";
constexpr std::string_view final_null = "CONTINUE  '' / ";

constexpr std::size_t escaped_size(char c) noexcept
{
    return c == '\'' ? 2 : 1;
}

std::size_t escaped_size(std::string_view text) noexcept
{
    std::size_t size = 0;
    for (const char c : text) {
        size += escaped_size(c);
    }
    return size;
}

// The pieces of value between the quotes of their records, each followed by the ampersand that continues it, the last
// one too when last_continued: the first piece fits first_room characters once its quotes are doubled, the others
// continued_room. A doubled quote is never split.
std::vector<std::string_view> pieces_of(std::string_view value, std::size_t first_room, bool last_continued)
{
    std::vector<std::string_view> pieces;
    std::size_t room = first_room;
    while (escaped_size(value) + (last_continued ? 1 : 0) > room) {
        std::size_t taken = 0;
        std::size_t size = 1; // The ampersand.
        while (taken < value.size() && size + escaped_size(value[taken]) <= room) {
            size += escaped_size(value[taken]);
            ++taken;
        }
        pieces.push_back(value.substr(0, taken));
        value.remove_prefix(taken);
        room = continued_room;
    }
    pieces.push_back(value);
    return pieces;
}

std::string continuation_record(std::string_view piece, bool continued)
{
    std::string record(continue_prefix);
    for (const char c : piece) {
        record += c;
        if (c == '\'') {
            record += '\'';
        }
    }
    if (continued) {
        record += '&';
    }
    record += '\'';
    return record;
}

std::vector<std::string> long_string_records(std::string_view keyword, std::string_view value, std::string_view comment,
                                             const record_options& options)
{
    const std::string name = canonical_hierarch_name(keyword);
    const error_context where{.hdu = options.hdu, .keyword = name};
    if (is_reserved_keyword(name) && (!options.continue_reserved || is_structural_keyword(name))) {
        throw validation_error(errc::continued_reserved_keyword,
                               "the string of " + quote(name) +
                                   " is longer than a record holds, and CONTINUE must not continue a reserved keyword",
                               where);
    }
    if (!is_printable(value) || !is_printable(comment)) {
        throw validation_error(
            errc::invalid_character,
            "the string or the comment of " + quote(name) + " holds a character outside printable ASCII", where);
    }
    // The first record holds the string between quotes after "NAME    = " or "HIERARCH NAME = ", and its piece needs
    // eight characters at least, to which the writer pads strings.
    const std::size_t prefix = is_standard_keyword(name) ? 10 : 9 + name.size() + 3;
    if (prefix + 10 > record_length) {
        throw validation_error(errc::value_too_long, "the keyword " + quote(name) + " leaves no room for a long string",
                               where);
    }
    const std::size_t first_room = record_length - prefix - 2;

    // The records of the string, without trailing spaces; the last one continued too when a comment follows apart.
    const auto string_records = [&](bool last_continued) {
        const std::vector<std::string_view> pieces = pieces_of(value, first_room, last_continued);
        std::string first(pieces.front());
        if (pieces.size() > 1 || last_continued) {
            first += '&';
        }
        std::vector<std::string> result;
        result.emplace_back(trim_trailing_spaces(write_value_record(name, card_value(std::move(first)), {}, options)));
        for (std::size_t i = 1; i < pieces.size(); ++i) {
            result.push_back(continuation_record(pieces[i], i + 1 < pieces.size() || last_continued));
        }
        return result;
    };

    // The comment goes on the last record of the string where it fits, and on records of null strings after it
    // otherwise. A part that fills its record to byte 80 is joined to the next without a space when the header is read.
    std::vector<std::string> records = string_records(false);
    if (!comment.empty()) {
        if (records.back().size() + comment_separator.size() + comment.size() <= record_length) {
            records.back() += comment_separator;
            records.back() += comment;
        } else {
            records = string_records(true);
            const std::size_t continued_part = record_length - continued_null.size();
            while (comment.size() > record_length - final_null.size()) {
                records.push_back(std::string(continued_null).append(comment.substr(0, continued_part)));
                comment.remove_prefix(continued_part);
            }
            records.push_back(std::string(final_null).append(comment));
        }
    }
    for (std::string& record : records) {
        record.resize(record_length, ' ');
    }
    return records;
}

} // namespace

bool is_continuation(const card& value) noexcept
{
    return value.keyword() == "CONTINUE" && !value.has_value_indicator() && value.value().kind() == value_kind::string;
}

std::size_t continuation_length(std::span<const card> cards, std::size_t index) noexcept
{
    std::size_t length = 0;
    std::size_t current = index;
    while (current + 1 < cards.size() && cards[current].value().kind() == value_kind::string &&
           cards[current].value().get<std::string>().ends_with('&') && is_continuation(cards[current + 1])) {
        ++length;
        ++current;
    }
    return length;
}

std::string joined_string(std::span<const card> cards, std::size_t index)
{
    std::string result = cards[index].value().get<std::string>();
    const std::size_t length = continuation_length(cards, index);
    for (std::size_t i = 1; i <= length; ++i) {
        result.pop_back();
        result += cards[index + i].value().get<std::string>();
    }
    return result;
}

std::string joined_comment(std::span<const card> cards, std::size_t index)
{
    std::string result(cards[index].comment());
    bool spaced = trim_trailing_spaces(cards[index].text()).size() < record_length;
    const std::size_t length = continuation_length(cards, index);
    for (std::size_t i = 1; i <= length; ++i) {
        const card& next = cards[index + i];
        if (!next.comment().empty()) {
            if (!result.empty() && spaced) {
                result += ' ';
            }
            result += next.comment();
        }
        spaced = trim_trailing_spaces(next.text()).size() < record_length;
    }
    return result;
}

std::vector<std::string> card_records(const card& value, const record_options& options)
{
    if (!value.text().empty()) {
        return {std::string(value.text())};
    }
    try {
        return {write_record(value, options)};
    } catch (const validation_error& failure) {
        if (failure.code() != errc::value_too_long || !value.has_value_indicator() ||
            value.value().kind() != value_kind::string) {
            throw;
        }
    }
    return long_string_records(value.keyword(), value.value().get<std::string>(), value.comment(), options);
}

} // namespace openfits::detail
