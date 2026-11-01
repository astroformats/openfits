// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "header/record.h"

#include "core/ascii.h"
#include "core/keyword_name.h"
#include "core/quote.h"
#include "core/value_text.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

namespace openfits::detail {

namespace {

constexpr std::string_view hierarch_record_prefix = "HIERARCH ";
// A value of the fixed format ends in byte 30, and a comment after it starts in byte 34, after " / ".
constexpr std::size_t fixed_value_end = 30;
constexpr std::string_view comment_separator = " / ";

void depart(std::vector<diagnostic>& departures, severity level, errc code, std::string message)
{
    departures.push_back({.severity = level, .code = code, .message = std::move(message), .context = {}});
}

// What makes a keyword name read from a record other than a name of standard §4.1.2.1, or nothing.
std::optional<std::string_view> name_problem(std::string_view name, bool left_justified)
{
    if (!left_justified) {
        return "is not left-justified in bytes 1 to 8";
    }
    if (name.size() > 8) {
        return "has more than eight characters";
    }
    if (std::ranges::any_of(name, is_lower)) {
        return "has lowercase letters";
    }
    if (name.find(' ') != std::string_view::npos) {
        return "has spaces inside";
    }
    if (!std::ranges::all_of(name, is_keyword_char)) {
        return "has characters that keyword names cannot hold";
    }
    return std::nullopt;
}

void check_name(std::string_view name, bool left_justified, std::vector<diagnostic>& departures)
{
    if (const std::optional<std::string_view> problem = name_problem(name, left_justified)) {
        depart(departures, severity::warning, errc::invalid_keyword,
               "the keyword name " + quote(name) + ' ' + std::string(*problem));
    }
}

// A card whose departures are complete, with the keyword in their context.
card finish(std::string keyword, value_field field, std::size_t field_begin, std::string_view record,
            bool value_indicator, std::vector<diagnostic>& found, std::vector<diagnostic>& departures)
{
    for (diagnostic& departure : found) {
        departure.context.keyword = keyword;
        departures.push_back(std::move(departure));
    }
    return card_access::make(std::move(keyword), std::move(field.value), std::move(field.comment), std::string(record),
                             field_begin + field.value_begin, field_begin + field.value_end, value_indicator,
                             field.fixed_format);
}

// A card without value: bytes 9 to 80 are its text.
card commentary_card(std::string keyword, std::string_view record, std::vector<diagnostic>& found,
                     std::vector<diagnostic>& departures)
{
    value_field field;
    field.comment = std::string(trim_trailing_spaces(record.substr(8)));
    return finish(std::move(keyword), std::move(field), 0, record, false, found, departures);
}

// The comment of a record with the space it needs from start on, cut when options allow it.
void append_comment(std::string& record, std::size_t start, std::string_view comment, const record_options& options,
                    const error_context& where)
{
    if (comment.empty()) {
        return;
    }
    const std::size_t room =
        record_length > start + comment_separator.size() ? record_length - start - comment_separator.size() : 0;
    if (comment.size() > room) {
        if (!options.truncate_comment) {
            throw validation_error(errc::comment_too_long,
                                   "the comment " + quote(comment) + " does not fit in the keyword record", where);
        }
        if (room == 0) {
            return;
        }
        comment = comment.substr(0, room);
    }
    record.resize(start, ' ');
    record += comment_separator;
    record += comment;
}

} // namespace

card card_access::make(std::string keyword, card_value value, std::string comment, std::string text,
                       std::size_t value_begin, std::size_t value_end, bool value_indicator, bool fixed_format)
{
    card result;
    result.keyword_ = std::move(keyword);
    result.value_ = std::move(value);
    result.comment_ = std::move(comment);
    result.text_ = std::move(text);
    result.value_begin_ = static_cast<std::uint8_t>(value_begin);
    result.value_end_ = static_cast<std::uint8_t>(value_end);
    result.value_indicator_ = value_indicator;
    result.fixed_format_ = fixed_format;
    return result;
}

card parse_record(std::string_view record, std::vector<diagnostic>& departures)
{
    std::vector<diagnostic> found;
    if (!is_printable(record)) {
        depart(found, severity::warning, errc::invalid_character,
               "the record holds bytes outside printable ASCII, which are kept as they are");
    }

    // The HIERARCH convention: the name runs from byte 10 to the first equals sign, which is the value indicator.
    if (record.starts_with(hierarch_record_prefix)) {
        const std::size_t equals = record.find('=', hierarch_record_prefix.size());
        if (equals != std::string_view::npos) {
            const std::string_view name =
                trim_spaces(record.substr(hierarch_record_prefix.size(), equals - hierarch_record_prefix.size()));
            if (!name.empty()) {
                if (std::ranges::any_of(name, is_lower)) {
                    depart(found, severity::warning, errc::invalid_keyword,
                           "the keyword name " + quote(name) + " has lowercase letters");
                }
                value_field field = read_value_field(record.substr(equals + 1), false, found);
                return finish(canonical_hierarch_name(name), std::move(field), equals + 1, record, true, found,
                              departures);
            }
        }
    }

    const std::string_view keyword_field = record.substr(0, 8);
    const std::size_t first = keyword_field.find_first_not_of(' ');
    if (first == std::string_view::npos) {
        return commentary_card({}, record, found, departures);
    }

    // The name runs to the first space or equals sign; a value indicator elsewhere than in bytes 9 and 10 is read as
    // one when only spaces separate it from the name.
    std::size_t name_end = first;
    while (name_end < record.size() && record[name_end] != ' ' && record[name_end] != '=') {
        ++name_end;
    }
    std::string_view name = trim_spaces(keyword_field);
    std::size_t field_begin = 0;
    bool indicator = false;
    bool misplaced = false;
    if (record.substr(8, 2) == "= ") {
        indicator = true;
        field_begin = 10;
    } else {
        std::size_t next = name_end;
        while (next < record.size() && record[next] == ' ') {
            ++next;
        }
        if (next < record.size() && record[next] == '=') {
            name = record.substr(first, name_end - first);
            indicator = true;
            misplaced = true;
            field_begin = next + 1;
        }
    }
    std::string keyword = canonical_keyword(name);
    check_name(name, first == 0, found);

    // Commentary keywords have no value, whatever bytes 9 and 10 hold (standard §4.4.2.4).
    if (is_commentary_keyword(keyword)) {
        return commentary_card(std::move(keyword), record, found, departures);
    }

    // A conforming CONTINUE record has spaces in bytes 9 and 10 and a string, which a header joins to the string of
    // the keyword before it; any other is commentary (standard §4.2.1.2).
    if (keyword == "CONTINUE") {
        if (record.substr(8, 2) == "  ") {
            std::vector<diagnostic> continued = found;
            value_field field = read_value_field(record.substr(10), false, continued);
            if (field.value.kind() == value_kind::string) {
                return finish(std::move(keyword), std::move(field), 10, record, false, continued, departures);
            }
        }
        return commentary_card(std::move(keyword), record, found, departures);
    }

    if (!indicator) {
        return commentary_card(std::move(keyword), record, found, departures);
    }
    if (misplaced) {
        depart(found, severity::warning, errc::invalid_value_indicator,
               "the value indicator is not '= ' in bytes 9 and 10");
    }
    value_field field = read_value_field(record.substr(field_begin), field_begin == 10, found);
    return finish(std::move(keyword), std::move(field), field_begin, record, true, found, departures);
}

std::string write_value_record(std::string_view keyword, const card_value& value, std::string_view comment,
                               const record_options& options)
{
    const std::string name = canonical_hierarch_name(keyword);
    const error_context where{.hdu = options.hdu, .keyword = name};
    if (is_commentary_keyword(name) || name == "CONTINUE" || name == "END") {
        throw validation_error(errc::invalid_keyword, "the keyword " + quote(name) + " cannot have a value", where);
    }
    const bool standard = is_standard_keyword(name);
    if (!standard) {
        if (!options.hierarch) {
            throw validation_error(errc::invalid_keyword,
                                   "the keyword name " + quote(name) +
                                       " is not a name of the standard, and the HIERARCH convention is not allowed",
                                   where);
        }
        if (!is_printable(name) || name.find('=') != std::string::npos) {
            throw validation_error(
                errc::invalid_keyword,
                "the keyword name " + quote(name) + " cannot be written with the HIERARCH convention", where);
        }
    }
    if (!is_printable(comment)) {
        throw validation_error(errc::invalid_character,
                               "the comment " + quote(comment) + " holds a character outside printable ASCII", where);
    }
    std::string text;
    try {
        text = format_value(value);
    } catch (const validation_error& failure) {
        throw validation_error(failure.code(), failure.what(), where);
    }

    std::string record;
    std::size_t comment_start = 0;
    if (standard) {
        record = name;
        record.resize(8, ' ');
        record += "= ";
        const value_kind kind = value.kind();
        const bool right_justified = kind != value_kind::string && kind != value_kind::undefined &&
                                     text.size() <= fixed_value_end - record.size();
        if (right_justified) {
            record.append(fixed_value_end - record.size() - text.size(), ' ');
        }
        record += text;
        // The comment after byte 30, where the comments of fixed-format values line up, when it fits there.
        comment_start = std::max(record.size(), fixed_value_end);
        if (comment.size() + comment_separator.size() + comment_start > record_length) {
            comment_start = record.size();
        }
    } else {
        record = std::string(hierarch_record_prefix) + name + " = " + text;
        if (record.size() > record_length) {
            // A character less, as the convention allows.
            record = std::string(hierarch_record_prefix) + name + "= " + text;
        }
        comment_start = record.size();
    }
    if (record.size() > record_length) {
        throw validation_error(errc::value_too_long, "the value " + quote(text) + " does not fit in the keyword record",
                               where);
    }
    append_comment(record, comment_start, comment, options, where);
    record.resize(record_length, ' ');
    return record;
}

std::string write_commentary_record(std::string_view keyword, std::string_view text, const record_options& options)
{
    const std::string name = canonical_hierarch_name(keyword);
    const error_context where{.hdu = options.hdu, .keyword = name};
    if (name == "CONTINUE" || name == "END" || (!name.empty() && !is_standard_keyword(name))) {
        throw validation_error(errc::invalid_keyword,
                               "the keyword " + quote(name) + " cannot be written as a commentary card", where);
    }
    if (!is_printable(text)) {
        throw validation_error(errc::invalid_character,
                               "the text " + quote(text) + " holds a character outside printable ASCII", where);
    }
    std::string record = name;
    record.resize(8, ' ');
    if (text.size() > record_length - record.size()) {
        if (!options.truncate_comment) {
            throw validation_error(errc::comment_too_long,
                                   "the text " + quote(text) + " does not fit in the keyword record", where);
        }
        text = text.substr(0, record_length - record.size());
    }
    record += text;
    record.resize(record_length, ' ');

    // After any keyword but COMMENT, HISTORY and the blank one, a text could read as a value: an equals sign after
    // spaces, or after characters that continue an eight-character name, or the rest of a record of the HIERARCH
    // convention. The record is read back to make sure it is commentary.
    if (!is_commentary_keyword(name)) {
        std::vector<diagnostic> departures;
        const card read_back = parse_record(record, departures);
        if (read_back.has_value_indicator() || read_back.keyword() != name) {
            throw validation_error(errc::invalid_value_indicator,
                                   "the text " + quote(text) + " would be read as the value of a keyword", where);
        }
    }
    return record;
}

std::string write_record(const card& value, const record_options& options)
{
    if (!value.text().empty()) {
        return std::string(value.text());
    }
    if (value.has_value_indicator()) {
        return write_value_record(value.keyword(), value.value(), value.comment(), options);
    }
    return write_commentary_record(value.keyword(), value.comment(), options);
}

} // namespace openfits::detail
