// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Untrusted bytes read as a FITS file (standard §3, §4) from memory, under small limits: every unit walked, its header
// read through the typed API, written back as text that reads back as the same cards, and its records compared with
// the bytes they were read from; the input is also parsed as a file specification. A failure must be an error of the
// library's kinds; any other exception escapes and fails the run.

#include <openfits/error.h>
#include <openfits/hdu.h>
#include <openfits/header.h>
#include <openfits/io.h>
#include <openfits/reader.h>

#include <algorithm>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using openfits::card;
using openfits::hdu_info;
using openfits::header;
using openfits::reader;

// Stops the run, so that the fuzzer reports the input that broke a property.
void require(bool condition)
{
    if (!condition) {
        std::abort();
    }
}

openfits::read_options small_limits()
{
    openfits::read_options options;
    options.limits.max_hdus = 16;
    options.limits.max_header_bytes = std::uint64_t{64} * 2880;
    options.limits.max_total_header_bytes = std::uint64_t{128} * 2880;
    options.limits.max_diagnostics = 64;
    return options;
}

template <typename T> void convert(const header& cards, std::string_view keyword)
{
    try {
        (void)cards.find<T>(keyword);
    } catch (const openfits::invalid_data_error& failure) {
        // A value that does not convert to T.
        const openfits::errc code = failure.code();
        require(code == openfits::errc::keyword_type || code == openfits::errc::value_out_of_range);
    }
}

void check_values(const header& cards)
{
    for (const card& value : cards) {
        const std::string_view keyword = value.keyword();
        require(cards.index_of(keyword).has_value());
        convert<bool>(cards, keyword);
        convert<std::int8_t>(cards, keyword);
        convert<std::uint16_t>(cards, keyword);
        convert<std::int64_t>(cards, keyword);
        convert<std::uint64_t>(cards, keyword);
        convert<float>(cards, keyword);
        convert<double>(cards, keyword);
        convert<std::string>(cards, keyword);
        convert<std::complex<double>>(cards, keyword);
        require(cards.comment(keyword).has_value());
        require(!cards.matching(keyword).empty());
    }
}

// The records of the cards are the bytes they were read from, a record that the file cuts padded with spaces.
void check_records(const hdu_info& unit, std::span<const std::byte> file)
{
    const header& cards = unit.header();
    require(unit.header_offset() + unit.header_size() <= file.size());
    for (std::size_t i = 0; i < cards.size(); ++i) {
        const std::string_view text = cards[i].text();
        require(text.size() == 80);
        const std::uint64_t offset = unit.header_offset() + (i * 80);
        require(offset < file.size());
        const std::size_t present = std::min<std::uint64_t>(80, file.size() - offset);
        require(std::memcmp(text.data(), file.data() + offset, present) == 0);
        require(std::all_of(text.begin() + static_cast<std::ptrdiff_t>(present), text.end(),
                            [](char c) { return c == ' '; }));
    }
}

// A primary header written back as text reads back as the same cards, its fill space left out. Its END record may take
// a block more than the limit of the first read allowed, so that the second read keeps the default limits.
void check_rewrite(const header& cards)
{
    std::string text = cards.to_string();
    text.resize((text.size() + 2879) / 2880 * 2880, ' ');
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    const reader again(std::make_unique<openfits::memory_source>(std::move(bytes)));
    const header& reread = again.hdu(0).header();
    require(reread.size() == cards.size() - cards.free_cards());
    for (std::size_t i = 0; i < reread.size(); ++i) {
        require(reread[i] == cards[i]);
    }
}

void check_section(const hdu_info& unit)
{
    try {
        const auto naxis = unit.header().value<std::uint64_t>("NAXIS");
        openfits::region section;
        for (std::uint64_t i = 0; i < std::min<std::uint64_t>(naxis, 9); ++i) {
            section.start.push_back(0);
            section.size.push_back(1);
            section.step.push_back(i % 2 == 0 ? 2 : -3);
        }
        (void)openfits::adjust_for_section(unit.header(), section);
    } catch (const openfits::usage_error& failure) {
        // A region that the image does not have.
        require(failure.code() == openfits::errc::invalid_argument);
    } catch (const openfits::invalid_data_error& failure) {
        // Axes that do not read as integers.
        require(failure.code() != openfits::errc::invalid_argument);
    }
}

void check_unit(const reader& input, const hdu_info& unit, std::span<const std::byte> file)
{
    require(unit.header_size() > 0);
    check_records(unit, file);
    check_values(unit.header());
    check_section(unit);
    if (unit.index() == 0) {
        check_rewrite(unit.header());
    }
    if (const std::optional<std::uint64_t> size = unit.data_size(); size && *size > 0) {
        std::vector<std::byte> bytes(std::min<std::uint64_t>(*size, 16));
        try {
            input.read_raw(unit.index(), *size - bytes.size(), bytes);
        } catch (const openfits::io_error& failure) {
            // The data of a truncated last unit.
            require(failure.code() == openfits::errc::end_of_data);
        }
    }
}

} // namespace

// The entry point that libFuzzer, and the replay driver, call for every input. Its name is fixed by libFuzzer.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::span<const std::byte> file(reinterpret_cast<const std::byte*>(data), size);
    try {
        (void)openfits::file_specification::parse({reinterpret_cast<const char*>(data), size});
    } catch (const openfits::usage_error& failure) {
        // Text that is not a file specification.
        require(failure.code() == openfits::errc::invalid_file_specification);
    }

    std::optional<reader> input;
    try {
        input.emplace(std::make_unique<openfits::memory_source>(file), small_limits());
    } catch (const openfits::invalid_data_error&) {
        return 0;
    } catch (const openfits::limit_error&) {
        return 0;
    } catch (const openfits::integrity_error&) {
        // Bytes that start as a gzip member does and do not inflate, which the reader takes for a gzip file.
        return 0;
    }
    std::uint64_t end = 0;
    for (const hdu_info& unit : input->hdus()) {
        require(unit.header_offset() >= end);
        require(&input->hdu(unit.index()) == &unit);
        check_unit(*input, unit, file);
        end = unit.data_offset();
    }
    return 0;
}
