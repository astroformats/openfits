// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>
#include <openfits/header.h>

#include "core/ascii.h"
#include "core/keyword_name.h"
#include "core/quote.h"
#include "core/value_text.h"
#include "header/keywords.h"
#include "header/long_string.h"
#include "header/record.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory_resource>
#include <string>
#include <utility>

namespace openfits {

// The first card of each canonical keyword, for a header of more cards than searched_cards. Its nodes come from an
// arena of its own, which takes one allocation for the first 32 keywords, where a map takes one for each, and grows by
// blocks after them.
struct header::keyword_index
{
    // The bytes of a node of the map, its key and its value, as the standard libraries make them, or about.
    static constexpr std::size_t node_size = 96;

    explicit keyword_index(std::size_t cards) : arena(std::clamp<std::size_t>(cards, 1, 32) * node_size) {}

    // Makes the card at position the first of keyword, unless it has one.
    void add(std::string_view keyword, std::size_t position)
    {
        (void)first.try_emplace(std::pmr::string(keyword, &arena), position);
    }

    std::pmr::monotonic_buffer_resource arena;
    std::pmr::map<std::pmr::string, std::size_t, std::less<>> first{&arena};
};

namespace {

// The cards of a header that a lookup searches in order, which takes less than an index takes to make: the walk of a
// file reads the header of every unit, most of them short.
constexpr std::size_t searched_cards = 32;

// The text of a commentary card, bytes 9 to 80 of its record.
constexpr std::size_t commentary_length = detail::record_length - 8;

// 2^63 and 2^64, the bounds of the reals that convert to std::int64_t and std::uint64_t.
constexpr double two_to_63 = 9223372036854775808.0;
constexpr double two_to_64 = 18446744073709551616.0;

[[noreturn]] void throw_type(std::string_view keyword, std::string_view type)
{
    throw invalid_data_error(errc::keyword_type,
                             "the value of the keyword " + detail::quote(keyword) + " does not convert to " +
                                 std::string(type),
                             {.keyword = std::string(keyword)});
}

[[noreturn]] void throw_range(std::string_view keyword, std::string_view type)
{
    throw invalid_data_error(errc::value_out_of_range,
                             "the value of the keyword " + detail::quote(keyword) + " is beyond the range of " +
                                 std::string(type),
                             {.keyword = std::string(keyword)});
}

bool is_blank(const card& value) noexcept
{
    return value.keyword().empty() && !value.has_value_indicator() && value.comment().empty();
}

// The index of the first card of the keyword, or nothing when no card has it, or when its value is undefined and not
// required, for which a value is refused.
std::optional<std::size_t> defined_card(const header& cards, std::string_view keyword, bool required)
{
    const std::optional<std::size_t> position = cards.index_of(keyword);
    if (!position || cards[*position].value().kind() != value_kind::undefined) {
        return position;
    }
    if (required) {
        const std::string name(cards[*position].keyword());
        throw invalid_data_error(errc::keyword_undefined,
                                 "the value of the keyword " + detail::quote(name) + " is undefined",
                                 {.keyword = name});
    }
    return std::nullopt;
}

// The text of a value as its record writes it, or as the writer would write it.
std::string text_of(const card& value)
{
    if (!value.value_text().empty()) {
        return std::string(value.value_text());
    }
    try {
        return detail::format_value(value.value());
    } catch (const validation_error&) {
        throw_type(value.keyword(), "a string");
    }
}

// The number of the card at position: an integer or a real, or the string read as one.
card_value number_of(const header& cards, std::size_t position, std::string_view type)
{
    const card& value = cards[position];
    const value_kind kind = value.value().kind();
    if (kind == value_kind::integer || kind == value_kind::real) {
        return value.value();
    }
    if (kind != value_kind::string) {
        throw_type(value.keyword(), type);
    }
    const std::string text = detail::joined_string(cards.cards(), position);
    const std::string_view token = detail::trim_spaces(text);
    const detail::number_form form = detail::scan_number(token).form;
    if (form == detail::number_form::not_a_number) {
        throw_type(value.keyword(), type);
    }
    if (form == detail::number_form::integer) {
        if (std::optional<card_value> integer = detail::integer_value(token)) {
            return std::move(*integer);
        }
    }
    const std::optional<double> real = detail::real_value(token);
    if (!real) {
        throw_range(value.keyword(), type);
    }
    return {*real};
}

// A real that is an integer, which a conversion into an integer type takes.
double integral(double value, std::string_view keyword, std::string_view type)
{
    if (!std::isfinite(value) || std::trunc(value) != value) {
        throw_type(keyword, type);
    }
    return value;
}

// Commentary text in cards of 72 characters, one for an empty text.
void add_commentary(header& cards, std::string_view keyword, std::string_view text)
{
    do {
        cards.append(card::commentary(keyword, text.substr(0, commentary_length)));
        text.remove_prefix(std::min(text.size(), commentary_length));
    } while (!text.empty());
}

} // namespace

header::header(std::vector<card> cards) : cards_(std::move(cards))
{
    rebuild_index();
}

header::header(const header& other) : cards_(other.cards_)
{
    rebuild_index();
}

header::header() noexcept = default;
header::header(header&& other) noexcept = default;
header& header::operator=(header&& other) noexcept = default;
header::~header() = default;

header& header::operator=(const header& other)
{
    header copy(other);
    *this = std::move(copy);
    return *this;
}

const card& header::at(std::size_t index) const
{
    if (index >= cards_.size()) {
        throw usage_error(errc::invalid_argument, "the card " + std::to_string(index) + " of a header of " +
                                                      std::to_string(cards_.size()) + " cards does not exist");
    }
    return cards_[index];
}

bool header::contains(std::string_view keyword) const
{
    return index_of(keyword).has_value();
}

std::optional<std::size_t> header::index_of(std::string_view keyword) const
{
    if (cards_.empty()) {
        return std::nullopt;
    }
    // A name of the standard is canonical as it is, which saves a copy at each lookup.
    std::string name;
    if (!detail::is_standard_keyword(keyword)) {
        name = detail::canonical_keyword(keyword);
        keyword = name;
    }
    if (!index_) {
        for (std::size_t i = 0; i < cards_.size(); ++i) {
            if (cards_[i].keyword() == keyword) {
                return i;
            }
        }
        return std::nullopt;
    }
    const auto found = index_->first.find(keyword);
    if (found == index_->first.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::size_t header::count(std::string_view keyword) const
{
    const std::string name = detail::canonical_keyword(keyword);
    return static_cast<std::size_t>(
        std::ranges::count_if(cards_, [&name](const card& value) { return value.keyword() == name; }));
}

std::vector<std::size_t> header::matching(std::string_view pattern) const
{
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < cards_.size(); ++i) {
        if (detail::match_keyword(pattern, cards_[i].keyword())) {
            result.push_back(i);
        }
    }
    return result;
}

std::optional<std::string> header::comment(std::string_view keyword) const
{
    const std::optional<std::size_t> position = index_of(keyword);
    if (!position) {
        return std::nullopt;
    }
    return detail::joined_comment(cards_, *position);
}

std::optional<bool> header::logical_value(std::string_view keyword, bool required) const
{
    const std::optional<std::size_t> position = defined_card(*this, keyword, required);
    if (!position) {
        return std::nullopt;
    }
    const card& value = cards_[*position];
    if (value.value().kind() == value_kind::logical) {
        return value.value().get<bool>();
    }
    // A logical written as the string 'T' or 'F', which some files have.
    if (value.value().kind() == value_kind::string) {
        const std::string text = detail::joined_string(cards_, *position);
        if (text == "T" || text == "F") {
            return text == "T";
        }
    }
    throw_type(value.keyword(), "bool");
}

std::optional<std::int64_t> header::signed_value(std::string_view keyword, bool required) const
{
    const std::optional<std::size_t> position = defined_card(*this, keyword, required);
    if (!position) {
        return std::nullopt;
    }
    const std::string_view name = cards_[*position].keyword();
    const card_value number = number_of(*this, *position, "an integer");
    if (const auto* integer = std::get_if<std::int64_t>(&number.data())) {
        return *integer;
    }
    if (std::holds_alternative<std::uint64_t>(number.data())) {
        throw_range(name, "the integer type");
    }
    const double real = integral(number.get<double>(), name, "an integer");
    if (real < -two_to_63 || real >= two_to_63) {
        throw_range(name, "the integer type");
    }
    return static_cast<std::int64_t>(real);
}

std::optional<std::uint64_t> header::unsigned_value(std::string_view keyword, bool required) const
{
    const std::optional<std::size_t> position = defined_card(*this, keyword, required);
    if (!position) {
        return std::nullopt;
    }
    const std::string_view name = cards_[*position].keyword();
    const card_value number = number_of(*this, *position, "an integer");
    if (const auto* integer = std::get_if<std::int64_t>(&number.data())) {
        if (*integer < 0) {
            throw_range(name, "the integer type");
        }
        return static_cast<std::uint64_t>(*integer);
    }
    if (const auto* integer = std::get_if<std::uint64_t>(&number.data())) {
        return *integer;
    }
    const double real = integral(number.get<double>(), name, "an integer");
    if (real < 0.0 || real >= two_to_64) {
        throw_range(name, "the integer type");
    }
    return static_cast<std::uint64_t>(real);
}

std::optional<double> header::real_value(std::string_view keyword, bool required) const
{
    const std::optional<std::size_t> position = defined_card(*this, keyword, required);
    if (!position) {
        return std::nullopt;
    }
    const card_value number = number_of(*this, *position, "a real");
    if (const auto* integer = std::get_if<std::int64_t>(&number.data())) {
        return static_cast<double>(*integer);
    }
    if (const auto* integer = std::get_if<std::uint64_t>(&number.data())) {
        return static_cast<double>(*integer);
    }
    return number.get<double>();
}

std::optional<std::string> header::string_value(std::string_view keyword, bool required) const
{
    const std::optional<std::size_t> position = defined_card(*this, keyword, required);
    if (!position) {
        return std::nullopt;
    }
    const card& value = cards_[*position];
    switch (value.value().kind()) {
    case value_kind::string:
        return detail::joined_string(cards_, *position);
    case value_kind::logical:
        return value.value().get<bool>() ? "T" : "F";
    case value_kind::integer:
    case value_kind::real:
    case value_kind::complex_integer:
    case value_kind::complex_real:
        return text_of(value);
    case value_kind::none:
        // A value that does not parse is its text, which a card without a value indicator does not have.
        if (value.has_value_indicator() && !value.value_text().empty()) {
            return std::string(value.value_text());
        }
        break;
    case value_kind::undefined:
        break;
    }
    throw_type(value.keyword(), "a string");
}

std::optional<std::complex<double>> header::complex_value(std::string_view keyword, bool required) const
{
    const std::optional<std::size_t> position = defined_card(*this, keyword, required);
    if (!position) {
        return std::nullopt;
    }
    const card& value = cards_[*position];
    const card_value::storage& data = value.value().data();
    if (const auto* number = std::get_if<std::complex<double>>(&data)) {
        return *number;
    }
    if (const auto* number = std::get_if<complex_integer>(&data)) {
        return std::complex<double>(static_cast<double>(number->real), static_cast<double>(number->imag));
    }
    if (const auto* number = std::get_if<std::int64_t>(&data)) {
        return std::complex<double>(static_cast<double>(*number), 0.0);
    }
    if (const auto* number = std::get_if<std::uint64_t>(&data)) {
        return std::complex<double>(static_cast<double>(*number), 0.0);
    }
    if (const auto* number = std::get_if<double>(&data)) {
        return std::complex<double>(*number, 0.0);
    }
    throw_type(value.keyword(), "a complex number");
}

void header::throw_not_found(std::string_view keyword)
{
    const std::string name = detail::canonical_keyword(keyword);
    throw invalid_data_error(errc::keyword_not_found, "no card of the header has the keyword " + detail::quote(name),
                             {.keyword = name});
}

void header::throw_out_of_range(std::string_view keyword, std::string_view type)
{
    throw_range(detail::canonical_keyword(keyword), type);
}

void header::set(std::string_view keyword, card_value value)
{
    const std::optional<std::size_t> position = index_of(keyword);
    std::string comment;
    if (position && cards_[*position].has_value_indicator()) {
        comment = detail::joined_comment(cards_, *position);
    }
    set(keyword, std::move(value), comment);
}

void header::set(std::string_view keyword, card_value value, std::string_view comment)
{
    card replacement(keyword, std::move(value), comment);
    const std::string_view name = replacement.keyword();
    if (detail::is_commentary_keyword(name) || name == "CONTINUE" || name == "END") {
        throw usage_error(errc::invalid_keyword, "the keyword " + detail::quote(name) + " cannot have a value",
                          {.keyword = std::string(name)});
    }
    const std::optional<std::size_t> position = index_of(name);
    if (!position) {
        append(std::move(replacement));
        return;
    }
    const std::size_t continued = detail::continuation_length(cards_, *position);
    cards_[*position] = std::move(replacement);
    if (continued > 0) {
        const auto first = cards_.begin() + static_cast<std::ptrdiff_t>(*position) + 1;
        cards_.erase(first, first + static_cast<std::ptrdiff_t>(continued));
        rebuild_index();
    }
}

void header::append(card value)
{
    const std::size_t fill = free_cards();
    if (fill == 0 || is_blank(value)) {
        cards_.push_back(std::move(value));
        if (index_) {
            index_->add(cards_.back().keyword(), cards_.size() - 1);
        } else if (cards_.size() > searched_cards) {
            rebuild_index();
        }
        return;
    }
    const std::size_t position = cards_.size() - fill;
    cards_[position] = std::move(value);
    const std::string_view name = cards_[position].keyword();
    if (index_ && !name.empty()) {
        // The blank card replaced may be the first card of the blank keyword, which the index names.
        const auto blank = index_->first.find(std::string_view{});
        if (blank != index_->first.end() && blank->second == position) {
            if (fill > 1) {
                blank->second = position + 1;
            } else {
                index_->first.erase(blank);
            }
        }
        index_->add(name, position);
    }
}

void header::insert(std::size_t position, card value)
{
    if (position > cards_.size()) {
        throw usage_error(errc::invalid_argument, "a card cannot be inserted at " + std::to_string(position) +
                                                      " in a header of " + std::to_string(cards_.size()) + " cards");
    }
    cards_.insert(cards_.begin() + static_cast<std::ptrdiff_t>(position), std::move(value));
    rebuild_index();
}

bool header::erase(std::string_view keyword)
{
    const std::optional<std::size_t> position = index_of(keyword);
    if (!position) {
        return false;
    }
    const auto first = cards_.begin() + static_cast<std::ptrdiff_t>(*position);
    cards_.erase(first, first + static_cast<std::ptrdiff_t>(detail::continuation_length(cards_, *position) + 1));
    rebuild_index();
    return true;
}

void header::erase_at(std::size_t index)
{
    if (index >= cards_.size()) {
        throw usage_error(errc::invalid_argument, "the card " + std::to_string(index) + " of a header of " +
                                                      std::to_string(cards_.size()) + " cards does not exist");
    }
    cards_.erase(cards_.begin() + static_cast<std::ptrdiff_t>(index));
    rebuild_index();
}

bool header::rename(std::string_view from, std::string_view to)
{
    const std::string name = detail::canonical_keyword(to);
    if (name == "CONTINUE" || name == "END") {
        throw usage_error(errc::invalid_keyword, "no card can be renamed " + detail::quote(name), {.keyword = name});
    }
    const std::optional<std::size_t> position = index_of(from);
    if (!position) {
        return false;
    }
    const card& old = cards_[*position];
    const std::string_view text = old.text();
    if (!text.empty() && !text.starts_with("HIERARCH ") && detail::is_standard_keyword(name) &&
        (!old.has_value_indicator() || text.substr(8, 2) == "= ")) {
        // The record keeps its value and comment as they are written, and the CONTINUE records that follow.
        std::string record = name;
        record.resize(8, ' ');
        record += text.substr(8);
        std::vector<diagnostic> departures;
        cards_[*position] = detail::parse_record(record, departures);
    } else {
        const std::size_t continued = detail::continuation_length(cards_, *position);
        card renamed =
            old.has_value_indicator()
                ? card(name, continued > 0 ? card_value(detail::joined_string(cards_, *position)) : old.value(),
                       detail::joined_comment(cards_, *position))
                : card::commentary(name, old.comment());
        cards_[*position] = std::move(renamed);
        const auto first = cards_.begin() + static_cast<std::ptrdiff_t>(*position) + 1;
        cards_.erase(first, first + static_cast<std::ptrdiff_t>(continued));
    }
    rebuild_index();
    return true;
}

void header::add_comment(std::string_view text)
{
    add_commentary(*this, "COMMENT", text);
}

void header::add_history(std::string_view text)
{
    add_commentary(*this, "HISTORY", text);
}

std::size_t header::free_cards() const noexcept
{
    std::size_t fill = 0;
    while (fill < cards_.size() && is_blank(cards_[cards_.size() - fill - 1])) {
        ++fill;
    }
    return fill;
}

std::string header::to_string(bool commentary) const
{
    const detail::record_options options{.hierarch = true, .truncate_comment = true};
    std::string result;
    const std::size_t end = cards_.size() - free_cards();
    for (std::size_t i = 0; i < end; ++i) {
        if (!commentary && detail::is_commentary_keyword(cards_[i].keyword())) {
            continue;
        }
        for (const std::string& record : detail::card_records(cards_[i], options)) {
            result += record;
        }
    }
    std::string end_record = "END";
    end_record.resize(detail::record_length, ' ');
    result += end_record;
    return result;
}

void header::rebuild_index()
{
    if (cards_.size() <= searched_cards) {
        index_.reset();
        return;
    }
    auto keywords = std::make_unique<keyword_index>(cards_.size());
    for (std::size_t i = 0; i < cards_.size(); ++i) {
        keywords->add(cards_[i].keyword(), i);
    }
    index_ = std::move(keywords);
}

bool is_structural(std::string_view keyword)
{
    return detail::is_structural_keyword(detail::canonical_keyword(keyword));
}

bool is_standard_keyword(std::string_view keyword) noexcept
{
    return detail::is_standard_keyword(keyword);
}

std::size_t header::continuation_count(std::size_t index) const
{
    (void)at(index);
    return detail::continuation_length(cards_, index);
}

std::optional<std::string> header::string_at(std::size_t index) const
{
    if (at(index).value().kind() != value_kind::string) {
        return std::nullopt;
    }
    return detail::joined_string(cards_, index);
}

std::string header::comment_at(std::size_t index) const
{
    (void)at(index);
    return detail::joined_comment(cards_, index);
}

header without_structural(const header& cards)
{
    std::vector<card> kept;
    const std::span<const card> all = cards.cards();
    for (std::size_t i = 0; i < all.size(); ++i) {
        const std::size_t continued = detail::continuation_length(all, i);
        const std::string_view keyword = all[i].keyword();
        if (keyword == "CONTINUE" || !detail::is_structural_keyword(keyword)) {
            kept.insert(kept.end(), all.begin() + static_cast<std::ptrdiff_t>(i),
                        all.begin() + static_cast<std::ptrdiff_t>(i + continued + 1));
        }
        i += continued;
    }
    return header(std::move(kept));
}

} // namespace openfits
