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

void depart(std::vector<diagnostic>& departures, severity level, errc code, std::string_view section,
            std::string message)
{
    departures.push_back({.severity = level,
                          .code = code,
                          .section = std::string(section),
                          .message = std::move(message),
                          .context = {}});
}

// The characters of a keyword name read from a record, found in one pass.
struct name_characters
{
    bool lowercase = false;
    bool spaces = false;
    // Characters that a name of standard §4.1.2.1 cannot hold, lowercase letters and spaces among them.
    bool others = false;
};

name_characters characters_of(std::string_view name) noexcept
{
    name_characters found;
    for (const char c : name) {
        found.lowercase = found.lowercase || is_lower(c);
        found.spaces = found.spaces || c == ' ';
        found.others = found.others || !is_keyword_char(c);
    }
    return found;
}

// What makes a keyword name read from a record other than a name of standard §4.1.2.1, or nothing.
std::optional<std::string_view> name_problem(std::string_view name, const name_characters& characters,
                                             bool left_justified)
{
    if (!left_justified) {
        return "is not left-justified in bytes 1 to 8";
    }
    if (name.size() > 8) {
        return "has more than eight characters";
    }
    if (characters.lowercase) {
        return "has lowercase letters";
    }
    if (characters.spaces) {
        return "has spaces inside";
    }
    if (characters.others) {
        return "has characters that keyword names cannot hold";
    }
    return std::nullopt;
}

void check_name(std::string_view name, const name_characters& characters, bool left_justified,
                std::vector<diagnostic>& departures)
{
    if (const std::optional<std::string_view> problem = name_problem(name, characters, left_justified)) {
        depart(departures, severity::warning, errc::invalid_keyword, "4.1.2.1",
               "the keyword name " + quote(name) + ' ' + std::string(*problem));
    }
}

// The card into, whose value and comment are read into it already, its departures complete with the keyword in their
// context.
void finish(std::string keyword, const value_extent& extent, std::size_t field_begin, std::string_view record,
            bool value_indicator, std::vector<diagnostic>& found, std::vector<diagnostic>& departures, card& into)
{
    for (diagnostic& departure : found) {
        departure.context.keyword = keyword;
        departures.push_back(std::move(departure));
    }
    card_access::set_record(into, std::move(keyword), record, field_begin + extent.value_begin,
                            field_begin + extent.value_end, value_indicator, extent.fixed_format);
}

// The value field of record from field_begin, read into the value and the comment of into.
value_extent read_field_into(std::string_view record, std::size_t field_begin, bool at_byte_11,
                             std::vector<diagnostic>& found, card& into)
{
    return read_value_field_into(record.substr(field_begin), at_byte_11, found, card_access::value_of(into),
                                 card_access::comment_of(into));
}

// A card without value into into: bytes 9 to 80 are its text.
void commentary_card(std::string keyword, std::string_view record, std::vector<diagnostic>& found,
                     std::vector<diagnostic>& departures, card& into)
{
    card_access::value_of(into) = card_value();
    card_access::comment_of(into).assign(trim_trailing_spaces(record.substr(8)));
    finish(std::move(keyword), value_extent{}, 0, record, false, found, departures, into);
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

card card_access::make(std::string keyword, card_value value, std::string comment, std::string_view record,
                       std::size_t value_begin, std::size_t value_end, bool value_indicator, bool fixed_format)
{
    card result;
    set(result, std::move(keyword), std::move(value), std::move(comment), record, value_begin, value_end,
        value_indicator, fixed_format);
    return result;
}

void card_access::set(card& target, std::string keyword, card_value value, std::string comment, std::string_view record,
                      std::size_t value_begin, std::size_t value_end, bool value_indicator, bool fixed_format)
{
    target.value_ = std::move(value);
    target.comment_ = std::move(comment);
    set_record(target, std::move(keyword), record, value_begin, value_end, value_indicator, fixed_format);
}

void card_access::set_record(card& target, std::string keyword, std::string_view record, std::size_t value_begin,
                             std::size_t value_end, bool value_indicator, bool fixed_format)
{
    target.keyword_ = std::move(keyword);
    target.text_.clear();
    const std::size_t kept = std::min(record.size(), target.record_text_.size());
    std::copy_n(record.begin(), kept, target.record_text_.begin());
    std::fill(target.record_text_.begin() + static_cast<std::ptrdiff_t>(kept), target.record_text_.end(), ' ');
    target.value_begin_ = static_cast<std::uint8_t>(value_begin);
    target.value_end_ = static_cast<std::uint8_t>(value_end);
    target.value_indicator_ = value_indicator;
    target.fixed_format_ = fixed_format;
    target.record_ = true;
}

std::string ascii_text(std::string_view text, const record_options& options, const error_context& where,
                       std::string_view what)
{
    if (is_printable(text) || options.non_ascii == byte_policy::keep) {
        return std::string(text);
    }
    if (options.non_ascii == byte_policy::refuse) {
        throw validation_error(errc::invalid_character,
                               std::string(what) + ' ' + quote(text) + " holds a character outside printable ASCII",
                               where);
    }
    std::string replaced(text);
    for (char& c : replaced) {
        if (!is_printable(c)) {
            c = '?';
        }
    }
    return replaced;
}

card parse_record(std::string_view record, std::vector<diagnostic>& departures)
{
    card result;
    parse_record_into(record, departures, result);
    return result;
}

void parse_record_into(std::string_view record, std::vector<diagnostic>& departures, card& into)
{
    std::vector<diagnostic> found;
    if (!is_printable(record)) {
        depart(found, severity::warning, errc::invalid_character, "4.1.2.3",
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
                    depart(found, severity::warning, errc::invalid_keyword, "4.1.2.1",
                           "the keyword name " + quote(name) + " has lowercase letters");
                }
                const value_extent extent = read_field_into(record, equals + 1, false, found, into);
                finish(canonical_hierarch_name(name), extent, equals + 1, record, true, found, departures, into);
                return;
            }
        }
    }

    const std::string_view keyword_field = record.substr(0, 8);
    const std::size_t first = keyword_field.find_first_not_of(' ');
    if (first == std::string_view::npos) {
        commentary_card({}, record, found, departures, into);
        return;
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
    // A name without lowercase letters and spaces is canonical as it is, which a copy keeps.
    const name_characters characters = characters_of(name);
    std::string keyword = characters.lowercase || characters.spaces ? canonical_keyword(name) : std::string(name);
    check_name(name, characters, first == 0, found);

    // Commentary keywords have no value, whatever bytes 9 and 10 hold (standard §4.4.2.4).
    if (is_commentary_keyword(keyword)) {
        commentary_card(std::move(keyword), record, found, departures, into);
        return;
    }

    // A conforming CONTINUE record has spaces in bytes 9 and 10 and a string, which a header joins to the string of
    // the keyword before it; any other is commentary (standard §4.2.1.2).
    if (keyword == "CONTINUE") {
        if (record.substr(8, 2) == "  ") {
            std::vector<diagnostic> continued = found;
            const value_extent extent = read_field_into(record, 10, false, continued, into);
            if (card_access::value_of(into).kind() == value_kind::string) {
                finish(std::move(keyword), extent, 10, record, false, continued, departures, into);
                return;
            }
        }
        commentary_card(std::move(keyword), record, found, departures, into);
        return;
    }

    if (!indicator) {
        commentary_card(std::move(keyword), record, found, departures, into);
        return;
    }
    if (misplaced) {
        depart(found, severity::warning, errc::invalid_value_indicator, "4.1.2.2",
               "the value indicator is not '= ' in bytes 9 and 10");
    }
    const value_extent extent = read_field_into(record, field_begin, field_begin == 10, found, into);
    finish(std::move(keyword), extent, field_begin, record, true, found, departures, into);
}

namespace {

// The name of the record of a keyword with a value, checked: one that can have a value, and of the standard or, when
// options allow the HIERARCH convention, one that its record can hold.
std::string value_name(std::string_view keyword, const record_options& options)
{
    std::string name = canonical_hierarch_name(keyword);
    const error_context where{.hdu = options.hdu, .keyword = name};
    if (is_commentary_keyword(name) || name == "CONTINUE" || name == "END") {
        throw validation_error(errc::invalid_keyword, "the keyword " + quote(name) + " cannot have a value", where);
    }
    if (!is_standard_keyword(name)) {
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
    return name;
}

// The record of the keyword name, checked by value_name(), with the text of its value, of kind, and comment.
std::string value_record(const std::string& name, std::string_view text, value_kind kind, std::string_view comment,
                         const record_options& options, const error_context& where)
{
    const bool standard = is_standard_keyword(name);
    std::string record;
    std::size_t comment_start = 0;
    if (standard) {
        record = name;
        record.resize(8, ' ');
        record += "= ";
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
        record = std::string(hierarch_record_prefix) + name + " = ";
        if (record.size() + text.size() > record_length) {
            // A character less, as the convention allows.
            record.erase(record.size() - 3, 1);
        }
        record += text;
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

} // namespace

std::string write_value_record(std::string_view keyword, const card_value& value, std::string_view comment,
                               const record_options& options)
{
    const std::string name = value_name(keyword, options);
    const error_context where{.hdu = options.hdu, .keyword = name};
    const std::string written_comment = ascii_text(comment, options, where, "the comment");
    std::string text;
    try {
        if (value.kind() == value_kind::string) {
            text = format_string(ascii_text(value.get<std::string>(), options, where, "the string"),
                                 options.non_ascii == byte_policy::keep);
        } else {
            text = format_value(value);
        }
    } catch (const validation_error& failure) {
        throw validation_error(failure.code(), failure.what(), where);
    }
    return value_record(name, text, value.kind(), written_comment, options, where);
}

std::string write_value_text_record(std::string_view keyword, std::string_view text, value_kind kind,
                                    std::string_view comment, const record_options& options)
{
    const std::string name = value_name(keyword, options);
    const error_context where{.hdu = options.hdu, .keyword = name};
    const std::string written_comment = ascii_text(comment, options, where, "the comment");
    const std::string written_text = ascii_text(text, options, where, "the value text");
    try {
        check_value_text(written_text, options.non_ascii == byte_policy::keep);
    } catch (const validation_error& failure) {
        throw validation_error(failure.code(), failure.what(), where);
    }
    return value_record(name, written_text, kind, written_comment, options, where);
}

std::string write_commentary_record(std::string_view keyword, std::string_view text, const record_options& options)
{
    const std::string name = canonical_hierarch_name(keyword);
    const error_context where{.hdu = options.hdu, .keyword = name};
    if (name == "CONTINUE" || name == "END" || (!name.empty() && !is_standard_keyword(name))) {
        throw validation_error(errc::invalid_keyword,
                               "the keyword " + quote(name) + " cannot be written as a commentary card", where);
    }
    const std::string written = ascii_text(text, options, where, "the text");
    text = written;
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
        return ascii_text(value.text(), options, {.hdu = options.hdu, .keyword = std::string(value.keyword())},
                          "the record");
    }
    if (value.has_value_indicator()) {
        if (!value.value_text().empty()) {
            return write_value_text_record(value.keyword(), value.value_text(), value.value().kind(), value.comment(),
                                           options);
        }
        return write_value_record(value.keyword(), value.value(), value.comment(), options);
    }
    return write_commentary_record(value.keyword(), value.comment(), options);
}

} // namespace openfits::detail
