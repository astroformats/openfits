// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "checksum/checksum.h"

#include <openfits/error.h>

#include "core/ascii.h"
#include "core/endian.h"
#include "core/quote.h"
#include "core/scratch_buffer.h"
#include "data/image_description.h"
#include "header/long_string.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace openfits::detail {

namespace {

constexpr std::uint64_t block_size = 2880;
constexpr std::uint32_t negative_zero = 0xFFFFFFFFU;
// The bytes that a verification reads from a source at once.
constexpr std::size_t transfer = std::size_t{4} << 20;

// A sum in 64 bits folded into 32, the carries added back: nonzero stays nonzero.
constexpr std::uint32_t fold(std::uint64_t total) noexcept
{
    total = (total & 0xFFFFFFFFU) + (total >> 32U);
    total = (total & 0xFFFFFFFFU) + (total >> 32U);
    return static_cast<std::uint32_t>(total);
}

// The value of a sum of DATASUM: decimal digits, between spaces, maybe after zeros (standard §4.4.2.7).
std::optional<std::uint32_t> sum_in(std::string_view text) noexcept
{
    text = trim_spaces(text);
    if (text.empty() || !std::ranges::all_of(text, is_digit)) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        value = (value * 10) + static_cast<std::uint64_t>(c - '0');
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
    }
    return static_cast<std::uint32_t>(value);
}

// The string of the card at position, its CONTINUE records joined.
std::string string_of(const header& cards, std::size_t position)
{
    return joined_string(cards.cards(), position);
}

// The card of a checksum keyword that has a value: the first card of the keyword, with a value indicator.
std::optional<std::size_t> valued(const header& cards, std::string_view keyword)
{
    const std::optional<std::size_t> position = cards.index_of(keyword);
    if (!position || !cards[*position].has_value_indicator()) {
        return std::nullopt;
    }
    return position;
}

} // namespace

std::uint64_t data_records(std::uint64_t size) noexcept
{
    if (size > std::numeric_limits<std::uint64_t>::max() - (block_size - 1)) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return (size + block_size - 1) / block_size * block_size;
}

std::uint32_t sum_of(std::span<const std::byte> bytes, std::size_t round) noexcept
{
    // Two integers at a time: a 64-bit load holds them, the first in its high half.
    std::uint32_t result = 0;
    const std::byte* data = bytes.data();
    std::size_t pairs = bytes.size() / 8;
    round = std::clamp<std::size_t>(round, 1, sum_round);
    while (pairs > 0) {
        const std::size_t count = std::min(pairs, round);
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const auto value = load_big_endian<std::uint64_t>(std::span<const std::byte, 8>(data + (8 * i), 8));
            total += (value >> 32U) + (value & 0xFFFFFFFFU);
        }
        result = add_sums(result, fold(total));
        data += 8 * count;
        pairs -= count;
    }
    // The last bytes, completed with zeros, which add nothing.
    const std::size_t rest = bytes.size() % 8;
    if (rest > 0) {
        std::array<std::byte, 8> last{};
        std::copy_n(data, rest, last.begin());
        const auto value = load_big_endian<std::uint64_t>(last);
        result = add_sums(result, fold((value >> 32U) + (value & 0xFFFFFFFFU)));
    }
    return result;
}

void checksum_accumulator::add(std::span<const std::byte> bytes) noexcept
{
    // An integer that the last piece ended within, completed first.
    while (pending_bytes_ != 0 && !bytes.empty()) {
        pending_ |= std::uint32_t{std::to_integer<std::uint8_t>(bytes.front())} << (8U * (3U - pending_bytes_));
        bytes = bytes.subspan(1);
        if (++pending_bytes_ == 4) {
            sum_ = add_sums(sum_, pending_);
            pending_ = 0;
            pending_bytes_ = 0;
        }
    }
    const std::size_t whole = bytes.size() / 4 * 4;
    sum_ = add_sums(sum_, sum_of(bytes.first(whole)));
    for (const std::byte value : bytes.subspan(whole)) {
        pending_ |= std::uint32_t{std::to_integer<std::uint8_t>(value)} << (8U * (3U - pending_bytes_));
        ++pending_bytes_;
    }
}

std::uint32_t checksum_accumulator::sum() const noexcept
{
    return add_sums(sum_, pending_);
}

void summing_sink::write(std::span<const std::byte> data)
{
    if (target_ != nullptr) {
        target_->write(data);
    }
    sum_.add(data);
    written_ += data.size();
}

std::uint64_t summing_sink::position() const
{
    return target_ != nullptr ? target_->position() : written_;
}

void add_source_bytes(checksum_accumulator& sum, const thread_safe_source& source, std::uint64_t offset,
                      std::uint64_t size, const std::function<void(std::uint64_t)>& advance)
{
    if (offset >= source.size()) {
        return;
    }
    size = std::min(size, source.size() - offset);
    const std::span<const std::byte> held = source.bytes();
    // Pieces of memory too, so that the progress of a unit of gigabytes in memory is reported as it is summed.
    scratch_buffer staging(held.empty() ? std::min<std::uint64_t>(size, transfer) : 0);
    for (std::uint64_t done = 0; done < size;) {
        const std::size_t count = std::min<std::uint64_t>(size - done, transfer);
        if (held.empty()) {
            const std::span<std::byte> piece = staging.bytes().first(count);
            source.read(offset + done, piece);
            sum.add(piece);
        } else {
            sum.add(held.subspan(offset + done, count));
        }
        done += count;
        if (advance) {
            advance(count);
        }
    }
}

recorded_datasum datasum_of(const header& cards)
{
    const std::optional<std::size_t> position = valued(cards, "DATASUM");
    if (!position) {
        return {};
    }
    const card_value& value = cards[*position].value();
    switch (value.kind()) {
    case value_kind::none:
    case value_kind::undefined:
        return {};
    case value_kind::string: {
        const std::string text = string_of(cards, *position);
        if (trim_spaces(text).empty()) {
            return {};
        }
        return {.known = true, .value = sum_in(text)};
    }
    case value_kind::integer:
        // Written without quotes: CFITSIO reads the number all the same.
        if (const auto* integer = std::get_if<std::int64_t>(&value.data());
            integer != nullptr && std::in_range<std::uint32_t>(*integer)) {
            return {.known = true, .value = static_cast<std::uint32_t>(*integer)};
        }
        return {.known = true};
    case value_kind::logical:
    case value_kind::real:
    case value_kind::complex_integer:
    case value_kind::complex_real:
        break;
    }
    return {.known = true};
}

bool has_checksum(const header& cards)
{
    const std::optional<std::size_t> position = valued(cards, "CHECKSUM");
    if (!position) {
        return false;
    }
    const value_kind kind = cards[*position].value().kind();
    if (kind == value_kind::string) {
        return !trim_spaces(string_of(cards, *position)).empty();
    }
    return kind != value_kind::none && kind != value_kind::undefined;
}

void check_checksum_keywords(const header& cards, std::optional<std::uint32_t> header_sum, std::uint64_t hdu,
                             std::uint64_t header_offset, diagnostic_log& log)
{
    const auto at = [&](std::size_t position) {
        return error_context{.hdu = hdu,
                             .keyword = std::string(cards[position].keyword()),
                             .offset = header_offset + (position * record_length)};
    };
    const recorded_datasum datasum = datasum_of(cards);
    if (const std::optional<std::size_t> position = valued(cards, "DATASUM"); position && datasum.known) {
        const value_kind kind = cards[*position].value().kind();
        if (kind == value_kind::integer && datasum.value) {
            log.warning(errc::reserved_keyword,
                        "DATASUM holds an integer, where the standard requires a string of its digits; it is read as "
                        "the sum it gives",
                        at(*position));
        } else if (!datasum.value) {
            log.warning(errc::reserved_keyword,
                        "DATASUM holds " +
                            (kind == value_kind::string ? quote(string_of(cards, *position)) : std::string("a value")) +
                            ", which is not the decimal number of a 32-bit sum: the data of the unit cannot match it",
                        at(*position));
        }
    }
    const std::optional<std::size_t> position = valued(cards, "CHECKSUM");
    if (!position || !has_checksum(cards)) {
        return;
    }
    if (cards[*position].value().kind() != value_kind::string) {
        log.warning(errc::reserved_keyword, "CHECKSUM does not hold a string, as the standard requires", at(*position));
    }
    // The data records sum to what DATASUM records, unless they changed too, which a read of the data finds.
    if (header_sum && datasum.value && add_sums(*header_sum, *datasum.value) != negative_zero) {
        log.warning(errc::checksum_mismatch,
                    "the header and the data that DATASUM records do not sum to negative zero, as CHECKSUM makes them: "
                    "the header changed after CHECKSUM was computed",
                    at(*position));
    }
}

checksum_report verify_unit(const thread_safe_source& source, const hdu_info& unit, const progress_function& progress)
{
    const std::optional<std::uint64_t> data_size = unit.data_size();
    if (!data_size) {
        throw_unavailable(unit);
    }
    // The data records end at the block after the data, or at the end of a truncated file.
    const std::uint64_t data_offset = unit.data_offset();
    const std::uint64_t present = data_offset < source.size() ? source.size() - data_offset : 0;
    const std::uint64_t records = std::min(present, data_records(data_size.value_or(0)));
    const std::uint64_t header_bytes = std::min(unit.header_size(), source.size() - unit.header_offset());

    const std::uint64_t total = header_bytes + records;
    std::uint64_t done = 0;
    const std::function<void(std::uint64_t)> advance = [&](std::uint64_t bytes) {
        done += bytes;
        if (progress && !progress(done, total)) {
            throw cancelled_error(errc::cancelled,
                                  "the verification of unit " + std::to_string(unit.index()) + " was cancelled",
                                  {.hdu = unit.index()});
        }
    };
    advance(0);
    checksum_accumulator header_sum;
    add_source_bytes(header_sum, source, unit.header_offset(), header_bytes, advance);
    checksum_accumulator data_sum;
    add_source_bytes(data_sum, source, data_offset, records, advance);

    checksum_report report{.data_sum = data_sum.sum(), .hdu_sum = add_sums(header_sum.sum(), data_sum.sum())};
    const recorded_datasum datasum = datasum_of(unit.header());
    if (datasum.known) {
        report.datasum = datasum.value == report.data_sum ? checksum_state::valid : checksum_state::invalid;
    }
    if (has_checksum(unit.header())) {
        report.checksum = report.hdu_sum == negative_zero ? checksum_state::valid : checksum_state::invalid;
    }
    return report;
}

void throw_datasum_mismatch(std::uint64_t hdu, std::uint32_t sum, const recorded_datasum& recorded)
{
    throw integrity_error(errc::checksum_mismatch,
                          "the data of unit " + std::to_string(hdu) + " sum to " + std::to_string(sum) +
                              (recorded.value ? ", and its DATASUM records " + std::to_string(*recorded.value) +
                                                    ": they changed after DATASUM was computed"
                                              : ", and its DATASUM records no sum"),
                          {.hdu = hdu, .keyword = "DATASUM"});
}

void throw_checksum_mismatch(std::uint64_t hdu, const checksum_report& report)
{
    if (report.datasum == checksum_state::invalid) {
        throw integrity_error(errc::checksum_mismatch,
                              "the data of unit " + std::to_string(hdu) + " sum to " + std::to_string(report.data_sum) +
                                  ", which its DATASUM does not record",
                              {.hdu = hdu, .keyword = "DATASUM"});
    }
    throw integrity_error(errc::checksum_mismatch,
                          "unit " + std::to_string(hdu) + " sums to " + std::to_string(report.hdu_sum) +
                              ", where its CHECKSUM makes it negative zero: it changed after CHECKSUM was computed",
                          {.hdu = hdu, .keyword = "CHECKSUM"});
}

std::vector<card> checksum_cards(std::string_view comment)
{
    std::vector<card> cards;
    cards.emplace_back("CHECKSUM", "0000000000000000", comment);
    cards.emplace_back("DATASUM", "0", comment);
    return cards;
}

byte_range seal_checksums(std::string& blocks, std::uint32_t data_sum, std::string_view comment,
                          const record_options& options)
{
    // The writer refuses both keywords in the headers it is given, so that its own records are the only ones.
    std::size_t checksum = blocks.size();
    std::size_t datasum = blocks.size();
    for (std::size_t at = 0; at + record_length <= blocks.size(); at += record_length) {
        const std::string_view name(blocks.data() + at, 8);
        if (name == "CHECKSUM") {
            checksum = at;
        } else if (name == "DATASUM ") {
            datasum = at;
        }
    }
    if (checksum == blocks.size() || datasum == blocks.size()) {
        throw usage_error(errc::invalid_argument, "the header has no records of CHECKSUM and DATASUM to complete");
    }
    blocks.replace(datasum, record_length, write_value_record("DATASUM", std::to_string(data_sum), comment, options));
    // The value of CHECKSUM is the placeholder of 16 zeros while the header is summed (standard Appendix J.1), and
    // starts at byte 12 of its record.
    const std::uint32_t total = add_sums(sum_of(std::as_bytes(std::span(blocks))), data_sum);
    const std::string value = encode_checksum(total);
    blocks.replace(checksum + 11, value.size(), value);
    const std::size_t first = std::min(checksum, datasum);
    return {.offset = first, .size = std::max(checksum, datasum) + record_length - first};
}

} // namespace openfits::detail
