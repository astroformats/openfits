// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Every parser of the text of headers on the same input: keyword records (standard §4.1, Appendix A), numbers, strings,
// keyword names and patterns, dates (standard §9.1.1) and display formats (standard §7.3.4); and the UTF-8 and UTF-16
// forms of the paths of the API. A parser either accepts the text or throws the error of its kind, and whatever it
// accepts must be written back as text that parses to the same thing. Any other exception escapes and fails the run.

#include <openfits/card.h>
#include <openfits/error.h>

#include "core/keyword_name.h"
#include "core/utf16.h"
#include "core/utf8.h"
#include "core/value_text.h"
#include "header/record.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace detail = openfits::detail;
using openfits::card;
using openfits::card_value;
using openfits::diagnostic;
using openfits::errc;
using openfits::value_kind;

// Stops the run, so that the fuzzer reports the input that broke a property.
void require(bool condition)
{
    if (!condition) {
        std::abort();
    }
}

bool same_bits(double a, double b)
{
    return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

bool same_value(const card_value& a, const card_value& b)
{
    if (a.kind() == value_kind::real && b.kind() == value_kind::real) {
        return same_bits(a.get<double>(), b.get<double>());
    }
    if (a.kind() == value_kind::complex_real && b.kind() == value_kind::complex_real) {
        const std::complex<double> x = a.get<std::complex<double>>();
        const std::complex<double> y = b.get<std::complex<double>>();
        return same_bits(x.real(), y.real()) && same_bits(x.imag(), y.imag());
    }
    return a == b;
}

card parse_conforming(const std::string& record)
{
    std::vector<diagnostic> departures;
    const card result = detail::parse_record(record, departures);
    // What the writer writes conforms to the standard.
    require(departures.empty());
    return result;
}

// A record read, written again, and read back: the keyword, the value and the comment survive, the comment cut when
// it has to be.
void check_record(std::string_view input)
{
    std::string record(input.substr(0, detail::record_length));
    record.resize(detail::record_length, ' ');
    std::vector<diagnostic> departures;
    const card first = detail::parse_record(record, departures);
    require(first.text() == record);
    require(card::parse(record) == first);
    for (const diagnostic& departure : departures) {
        require(departure.context.keyword == first.keyword());
    }
    // A record read is written back as it is, its bytes outside printable ASCII too when they are kept.
    require(detail::write_record(first, {.non_ascii = detail::byte_policy::keep}) == record);

    if (!first.has_value_indicator() && first.value().kind() != value_kind::none) {
        // A CONTINUE record, which only a header writes again.
        require(first.keyword() == "CONTINUE" && first.value().kind() == value_kind::string);
        return;
    }

    const detail::record_options options{.hierarch = true, .truncate_comment = true};
    std::string written;
    try {
        written = first.has_value_indicator()
                      ? detail::write_value_record(first.keyword(), first.value(), first.comment(), options)
                      : detail::write_commentary_record(first.keyword(), first.comment(), options);
    } catch (const openfits::validation_error&) {
        // A value of no kind, bytes outside printable ASCII, a name or a value that no record holds.
        return;
    }
    require(written.size() == detail::record_length);
    const card second = parse_conforming(written);
    require(second.keyword() == first.keyword());
    require(second.has_value_indicator() == first.has_value_indicator());
    require(same_value(second.value(), first.value()));
    require(first.comment().starts_with(second.comment()));
    require(second.unit() == first.unit() || second.comment() != first.comment());
}

// Numbers: what the grammar accepts converts, and its text converts back to the same value.
void check_number(std::string_view text)
{
    const detail::scanned_number scanned = detail::scan_number(text);
    if (scanned.form == detail::number_form::integer) {
        if (const std::optional<card_value> integer = detail::integer_value(text)) {
            const std::string written = detail::format_value(*integer);
            require(detail::scan_number(written).form == detail::number_form::integer);
            require(detail::integer_value(written) == integer);
        }
    }
    if (scanned.form != detail::number_form::not_a_number) {
        if (const std::optional<double> real = detail::real_value(text)) {
            const std::string written = detail::format_real(*real);
            require(detail::scan_number(written).form == detail::number_form::real);
            const std::optional<double> again = detail::real_value(written);
            require(again && same_bits(*again, *real));
        }
    }
}

// Strings: what the writer writes, the reader reads as the same string, but for trailing spaces.
void check_string(std::string_view text)
{
    std::string written;
    try {
        written = detail::format_string(text);
    } catch (const openfits::validation_error& failure) {
        require(failure.code() == errc::invalid_character);
        return;
    }
    std::vector<diagnostic> departures;
    const detail::value_field field = detail::read_value_field(written, true, departures);
    require(departures.empty());
    const auto& read = field.value.get<std::string>();
    require(text.starts_with(read));
    require(read.size() == text.size() || text.substr(read.size()).find_first_not_of(' ') == std::string_view::npos);
}

// Keyword names and patterns.
void check_keyword(std::string_view text)
{
    const std::string canonical = detail::canonical_keyword(text);
    require(detail::canonical_keyword(canonical) == canonical || canonical.starts_with("HIERARCH "));
    require(detail::match_keyword(canonical, canonical));
    require(detail::match_keyword("*", canonical));
    const std::size_t middle = text.size() / 2;
    (void)detail::match_keyword(text.substr(0, middle), text.substr(middle));
}

void check_date(std::string_view text)
{
    openfits::date_time value;
    try {
        value = openfits::parse_date(text);
    } catch (const openfits::invalid_data_error& failure) {
        require(failure.code() == errc::invalid_date);
        return;
    }
    require(openfits::parse_date(openfits::format_date(value)) == value);
}

void check_display_format(std::string_view text, double real, std::int64_t integer)
{
    openfits::display_format format;
    try {
        format = openfits::display_format::parse(text);
    } catch (const openfits::invalid_data_error& failure) {
        require(failure.code() == errc::invalid_display_format);
        return;
    }
    require(openfits::display_format::parse(format.to_string()) == format);
    // Only formats of reasonable widths are rendered, so that a run stays fast.
    if (format.width > 4096) {
        return;
    }
    const auto rendered = [&format](const auto& value, std::size_t width) {
        try {
            require(openfits::format_display(format, value).size() == width);
        } catch (const openfits::invalid_data_error& failure) {
            require(failure.code() == errc::invalid_display_format);
        }
    };
    rendered(real, format.width);
    rendered(integer, format.width);
    rendered(static_cast<std::uint64_t>(integer), format.width);
    rendered(text, format.width);
    rendered(integer < 0, format.width);
    rendered(std::complex<double>(real, -real), (2 * std::size_t{format.width}) + 3);
}

// Paths: text converts to UTF-16 exactly when it is valid UTF-8, and back to itself; the bytes taken as UTF-16 code
// units, in the order of the host, convert to UTF-8 only when they are well formed, and back to themselves.
void check_text_encodings(std::string_view text)
{
    const bool valid = detail::is_valid_utf8(text);
    try {
        const std::u16string wide = detail::utf8_to_utf16(text);
        require(valid);
        require(detail::utf16_to_utf8(wide) == std::optional<std::string>(text));
    } catch (const openfits::invalid_data_error& failure) {
        require(!valid && failure.code() == errc::invalid_utf8);
    }
    std::u16string units(text.size() / 2, u'\0');
    if (!units.empty()) {
        std::memcpy(units.data(), text.data(), units.size() * sizeof(char16_t));
    }
    if (const std::optional<std::string> narrow = detail::utf16_to_utf8(units)) {
        require(detail::is_valid_utf8(*narrow));
        require(detail::utf8_to_utf16(*narrow) == units);
    }
}

} // namespace

// The entry point that libFuzzer, and the replay driver, call for every input. Its name is fixed by libFuzzer.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);

    check_record(text);
    check_number(text);
    check_string(text.substr(0, 68));
    check_keyword(text.substr(0, 80));
    check_date(text);
    check_text_encodings(text);

    // The first 16 bytes, when there are as many, give the values that display formats render.
    std::array<std::byte, 16> numbers{};
    if (size > 0) {
        std::memcpy(numbers.data(), data, std::min(size, numbers.size()));
    }
    double real = 0;
    std::int64_t integer = 0;
    std::memcpy(&real, numbers.data(), sizeof(real));
    std::memcpy(&integer, numbers.data() + sizeof(real), sizeof(integer));
    check_display_format(text, real, integer);
    return 0;
}
