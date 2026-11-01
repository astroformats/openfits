// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>
#include <openfits/hdu.h>

#include "core/ascii.h"
#include "core/quote.h"
#include "core/value_text.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <string>
#include <system_error>

namespace openfits {

namespace {

[[noreturn]] void refuse(std::string_view text, const std::string& reason)
{
    throw usage_error(errc::invalid_file_specification, "the file specification " + detail::quote(text) + " " + reason);
}

// A decimal integer without sign, or nothing.
std::optional<std::uint64_t> number_of(std::string_view token) noexcept
{
    if (token.empty() || !std::ranges::all_of(token, detail::is_digit)) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : token) {
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return std::nullopt;
        }
        value = (value * 10) + digit;
    }
    return value;
}

bool starts_with_word(std::string_view text, std::string_view word) noexcept
{
    if (text.size() < word.size()) {
        return false;
    }
    for (std::size_t i = 0; i < word.size(); ++i) {
        if (detail::to_upper(text[i]) != detail::to_upper(word[i])) {
            return false;
        }
    }
    if (text.size() == word.size()) {
        return true;
    }
    const char next = text[word.size()];
    return !detail::is_digit(next) && !detail::is_upper(next) && !detail::is_lower(next);
}

// What a bracket of CFITSIO's syntax holds that is neither a unit nor an image section: a filter, which this library
// does not apply.
std::string filter_of(std::string_view content)
{
    content = detail::trim_spaces(content);
    if (starts_with_word(content, "col")) {
        return "selects columns, a filter of the syntax of extended file names that this library does not apply";
    }
    if (starts_with_word(content, "bin")) {
        return "makes a histogram, a filter of the syntax of extended file names that this library does not apply";
    }
    if (starts_with_word(content, "pix")) {
        return "filters pixels, a filter of the syntax of extended file names that this library does not apply";
    }
    if (starts_with_word(content, "compress")) {
        return "asks for compression, an option of the syntax of extended file names that this library does not apply";
    }
    return "filters rows, a filter of the syntax of extended file names that this library does not apply";
}

// An image section holds a colon, or starts with * or -*.
bool is_section(std::string_view content) noexcept
{
    content = detail::trim_spaces(content);
    return content.find(':') != std::string_view::npos || content.starts_with('*') || content.starts_with("-*");
}

// One range of an image section: *, -*, first:last, any of them followed by :step, or :step alone for the whole axis.
std::optional<section_range> range_of(std::string_view token)
{
    token = detail::trim_spaces(token);
    section_range range;
    std::string_view rest = token;
    if (token.starts_with("-*")) {
        range.first = std::nullopt;
        range.last = 1;
        rest = token.substr(2);
    } else if (token.starts_with('*')) {
        rest = token.substr(1);
    } else if (!token.empty() && !token.starts_with(':')) {
        const std::size_t colon = token.find(':');
        if (colon == std::string_view::npos) {
            return std::nullopt;
        }
        const std::string_view after = token.substr(colon + 1);
        const std::size_t second = after.find(':');
        range.first = number_of(detail::trim_spaces(token.substr(0, colon)));
        range.last = number_of(detail::trim_spaces(after.substr(0, second)));
        if (!range.first || !range.last || *range.first == 0 || *range.last == 0) {
            return std::nullopt;
        }
        rest = second == std::string_view::npos ? std::string_view{} : after.substr(second);
    }
    rest = detail::trim_spaces(rest);
    if (!rest.empty()) {
        const std::optional<std::uint64_t> step =
            rest.front() == ':' ? number_of(detail::trim_spaces(rest.substr(1))) : std::nullopt;
        if (!step || *step == 0) {
            return std::nullopt;
        }
        range.step = *step;
    }
    return range;
}

std::vector<section_range> section_of(std::string_view content, std::string_view text)
{
    std::vector<section_range> section;
    std::size_t axis = 1;
    while (true) {
        const std::size_t comma = content.find(',');
        const std::optional<section_range> range = range_of(content.substr(0, comma));
        if (!range) {
            refuse(text, "has a range for axis " + std::to_string(axis) +
                             " that is not *, -* or first:last, with :step "
                             "or not, of pixels from 1 and a step from 1");
        }
        section.push_back(*range);
        if (comma == std::string_view::npos) {
            return section;
        }
        content.remove_prefix(comma + 1);
        ++axis;
    }
}

void unit_of(std::string_view content, std::string_view text, file_specification& result)
{
    if (content.find(';') != std::string_view::npos) {
        refuse(text, "selects an image in a table cell, which this library does not do");
    }
    const std::string_view trimmed = detail::trim_spaces(content);
    if (const std::optional<std::uint64_t> index = number_of(trimmed)) {
        result.hdu_index = index;
        return;
    }
    if (trimmed.find_first_of("<>=!&|") != std::string_view::npos || starts_with_word(trimmed, "col") ||
        starts_with_word(trimmed, "bin") || starts_with_word(trimmed, "pix") || starts_with_word(trimmed, "compress")) {
        refuse(text, filter_of(trimmed));
    }
    const std::size_t comma = trimmed.find(',');
    const std::string_view name = detail::trim_spaces(trimmed.substr(0, comma));
    if (name.empty()) {
        refuse(text, "names no unit in its first bracket");
    }
    result.hdu_name = name;
    if (comma == std::string_view::npos) {
        return;
    }
    const std::string_view after = trimmed.substr(comma + 1);
    if (after.find(',') != std::string_view::npos) {
        refuse(text, "selects a unit by its type, which this library does not do");
    }
    const std::optional<std::uint64_t> version = number_of(detail::trim_spaces(after));
    if (!version || *version > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        refuse(text, "gives a version of the unit that is not a number");
    }
    result.hdu_version = static_cast<std::int64_t>(*version);
}

// A value as a header keeps it after CFITSIO writes it with 16 significant digits.
double rounded(double value)
{
    std::array<char, 32> text{};
    const std::to_chars_result written =
        std::to_chars(text.data(), text.data() + text.size(), value, std::chars_format::scientific, 15);
    if (written.ec != std::errc{}) {
        return value;
    }
    // The reader of reals, which the libraries without a floating-point std::from_chars need.
    return detail::real_value(std::string_view(text.data(), static_cast<std::size_t>(written.ptr - text.data())))
        .value_or(value);
}

std::optional<double> real_of(const header& cards, const std::string& keyword)
{
    try {
        return cards.find<double>(keyword);
    } catch (const invalid_data_error&) {
        return std::nullopt;
    }
}

void scale(header& cards, const std::string& keyword, double factor)
{
    if (const std::optional<double> value = real_of(cards, keyword)) {
        cards.set(keyword, rounded(*value * factor));
    }
}

} // namespace

file_specification file_specification::parse(std::string_view text)
{
    file_specification result;
    const std::size_t open = text.find('[');
    const std::string_view path = text.substr(0, open);
    if (path.empty()) {
        refuse(text, "names no file");
    }
    if (path.ends_with(')') && path.find('(') != std::string_view::npos) {
        refuse(text, "names an output file in parentheses, which this library does not write");
    }
    result.path = path;
    if (open == std::string_view::npos) {
        return result;
    }
    std::vector<std::string_view> groups;
    for (std::string_view rest = text.substr(open); !rest.empty();) {
        if (rest.front() != '[') {
            refuse(text, "holds " + detail::quote(rest) + " after a bracket");
        }
        const std::size_t close = rest.find(']');
        if (close == std::string_view::npos) {
            refuse(text, "has a bracket that is not closed");
        }
        groups.push_back(rest.substr(1, close - 1));
        rest.remove_prefix(close + 1);
    }
    std::size_t next = 0;
    if (!is_section(groups.front())) {
        unit_of(groups.front(), text, result);
        next = 1;
    }
    if (next < groups.size()) {
        if (!is_section(groups[next])) {
            refuse(text, filter_of(groups[next]));
        }
        result.section = section_of(groups[next], text);
        ++next;
    }
    if (next < groups.size()) {
        refuse(text, filter_of(groups[next]));
    }
    return result;
}

header adjust_for_section(const header& image, const region& section)
{
    const auto naxis = image.value<std::uint64_t>("NAXIS");
    const std::vector<std::uint64_t> axes = image.values<std::uint64_t>("NAXIS", 1, naxis);
    if (section.start.size() != naxis || section.size.size() != naxis ||
        (!section.step.empty() && section.step.size() != naxis)) {
        throw usage_error(errc::invalid_argument, "the region of the section has other axes than the " +
                                                      std::to_string(naxis) + " of the image");
    }
    header result = image;
    for (std::size_t i = 0; i < naxis; ++i) {
        const std::uint64_t start = section.start[i];
        const std::uint64_t size = section.size[i];
        const std::int64_t step = section.step.empty() ? 1 : section.step[i];
        if (size == 0 || start >= axes[i] || size > axes[i] - start || step == 0) {
            throw usage_error(errc::invalid_argument,
                              "the region of the section does not lie in the image on axis " + std::to_string(i + 1));
        }
        // The section as CFITSIO gives it: from its first pixel, counted from 1, in steps of increment, backwards
        // from its last when the step is negative.
        const bool reversed = step < 0;
        const std::uint64_t increment =
            reversed ? static_cast<std::uint64_t>(-(step + 1)) + 1 : static_cast<std::uint64_t>(step);
        const std::uint64_t first = reversed ? start + size : start + 1;
        const std::string axis = std::to_string(i + 1);
        result.set("NAXIS" + axis, ((size - 1) / increment) + 1);
        if (first == 1 && increment == 1) {
            continue;
        }
        const auto from = static_cast<double>(first);
        const auto by = static_cast<double>(increment);
        for (char alternate = '@'; alternate <= 'Z'; ++alternate) {
            const std::string suffix = alternate == '@' ? std::string() : std::string(1, alternate);
            const auto name = [&axis, &suffix](std::string_view root) {
                std::string keyword(root);
                keyword += axis;
                keyword += suffix;
                return keyword;
            };
            const std::string crpix = name("CRPIX");
            const std::optional<double> reference = real_of(result, crpix);
            if (!reference) {
                continue;
            }
            result.set(crpix, rounded(reversed ? ((from - *reference) / by) + 1.0 : ((*reference - from) / by) + 1.0));
            if (!reversed && increment == 1) {
                continue;
            }
            // CFITSIO changes the increments of a description only where it has a reference pixel.
            const double factor = reversed ? -by : by;
            scale(result, name("CDELT"), factor);
            for (int j = 1; j <= 9; ++j) {
                scale(result, name("CD" + std::to_string(j) + "_"), factor);
            }
        }
    }
    return result;
}

} // namespace openfits
