// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/card.h>
#include <openfits/error.h>

#include "core/keyword_name.h"
#include "core/value_text.h"
#include "header/record.h"

#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace openfits {

namespace {

// The double of the shortest decimal form of a float, which converts back to the same float.
double shortest_double(float value)
{
    if (!std::isfinite(value)) {
        return static_cast<double>(value);
    }
    std::array<char, 32> buffer{};
    const char* const end = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value).ptr;
    const std::optional<double> parsed = detail::real_value(std::string_view(buffer.data(), end));
    return parsed ? *parsed : static_cast<double>(value);
}

card_value::storage integer_storage(std::uint64_t value) noexcept
{
    if (value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return card_value::storage(std::in_place_type<std::int64_t>, static_cast<std::int64_t>(value));
    }
    return card_value::storage(std::in_place_type<std::uint64_t>, value);
}

} // namespace

// The copy is made from the value that other holds, with the constructor of std::variant for one alternative, and not
// with the copy constructor of std::variant. That of libstdc++ 12 to 16, when the copy of the alternative throws, has
// the storage of the variant destroy the alternative of the index variant_npos, which it takes for a valid one in a
// variant whose alternatives never leave it valueless, as here: a failed allocation would be undefined behaviour, a
// crash, instead of std::bad_alloc. The constructor for one alternative throws before the storage exists.
card_value::card_value(const card_value& other)
    : value_(std::visit(
          [](const auto& held) { return storage(std::in_place_type<std::remove_cvref_t<decltype(held)>>, held); },
          other.value_))
{}

card_value::card_value(undefined_value value) noexcept : value_(std::in_place_type<undefined_value>, value) {}
card_value::card_value(bool value) noexcept : value_(std::in_place_type<bool>, value) {}
card_value::card_value(std::int64_t value) noexcept : value_(std::in_place_type<std::int64_t>, value) {}
card_value::card_value(std::uint64_t value) noexcept : value_(integer_storage(value)) {}
card_value::card_value(float value) : value_(std::in_place_type<double>, shortest_double(value)) {}
card_value::card_value(double value) noexcept : value_(std::in_place_type<double>, value) {}
card_value::card_value(complex_integer value) noexcept : value_(std::in_place_type<complex_integer>, value) {}
card_value::card_value(std::complex<float> value)
    : value_(std::in_place_type<std::complex<double>>, shortest_double(value.real()), shortest_double(value.imag()))
{}
card_value::card_value(std::complex<double> value) noexcept : value_(std::in_place_type<std::complex<double>>, value) {}
card_value::card_value(std::string value) noexcept : value_(std::in_place_type<std::string>, std::move(value)) {}
card_value::card_value(std::string_view value) : value_(std::in_place_type<std::string>, value) {}
card_value::card_value(const char* value) : value_(std::in_place_type<std::string>, value) {}

value_kind card_value::kind() const noexcept
{
    // The alternatives of storage, in their order. The variant is never valueless: the copy of an alternative is made
    // before it replaces the old one, and every alternative moves without throwing.
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

void card_value::throw_type_mismatch()
{
    throw usage_error(errc::invalid_argument, "the value of the keyword is not held in the type asked for");
}

card::card(std::string_view keyword, card_value value, std::string_view comment)
    : keyword_(detail::canonical_keyword(keyword)), value_(std::move(value)), comment_(comment), value_indicator_(true)
{}

card card::commentary(std::string_view keyword, std::string_view text)
{
    card result;
    result.keyword_ = detail::canonical_keyword(keyword);
    result.comment_ = std::string(text);
    return result;
}

card card::parse(std::string_view record)
{
    if (record.size() > detail::record_length) {
        throw usage_error(errc::invalid_argument,
                          "a keyword record has 80 characters, not " + std::to_string(record.size()));
    }
    std::string padded(record);
    padded.resize(detail::record_length, ' ');
    std::vector<diagnostic> departures;
    return detail::parse_record(padded, departures);
}

std::optional<std::string_view> card::unit() const noexcept
{
    const std::string_view text = comment_;
    if (!value_indicator_ || !text.starts_with('[')) {
        return std::nullopt;
    }
    const std::size_t close = text.find(']');
    if (close == std::string_view::npos) {
        return std::nullopt;
    }
    return text.substr(1, close - 1);
}

} // namespace openfits
