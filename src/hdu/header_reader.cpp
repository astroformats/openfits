// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "hdu/header_reader.h"

#include "checksum/checksum.h"
#include "core/ascii.h"
#include "core/quote.h"
#include "header/keywords.h"
#include "header/long_string.h"
#include "header/record.h"

#include <algorithm>
#include <string>
#include <utility>

namespace openfits::detail {

namespace {

constexpr std::uint64_t first_read = 4 * block_size;
constexpr std::uint64_t most_ahead = std::uint64_t{1} << 18;
constexpr std::string_view end_keyword = "END     ";

std::string_view text_of(std::span<const std::byte> bytes) noexcept
{
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// The bytes of a header from its start, read from the source through the window of the walk as they are needed, or
// seen in its content in memory.
class header_bytes
{
public:
    header_bytes(const thread_safe_source& source, std::uint64_t offset, read_window& window) noexcept
        : source_(source), offset_(offset), available_(source.size() - offset), window_(window)
    {
        if (!source.bytes().empty()) {
            memory_ = source.bytes().subspan(offset);
        }
    }

    // The first size bytes, or every byte to the end of the source when it has fewer.
    std::span<const std::byte> fetch(std::uint64_t size)
    {
        size = std::min(size, available_);
        if (!memory_.empty()) {
            return memory_.first(size);
        }
        return window_.bytes(source_, offset_, size);
    }

private:
    const thread_safe_source& source_;
    std::uint64_t offset_;
    std::uint64_t available_;
    read_window& window_;
    std::span<const std::byte> memory_{};
};

// The record at position, padded with spaces where the bytes end first.
std::string record_at(std::span<const std::byte> bytes, std::uint64_t position)
{
    std::string record(
        text_of(bytes.subspan(position, std::min<std::uint64_t>(record_length, bytes.size() - position))));
    record.resize(record_length, ' ');
    return record;
}

bool all_spaces(std::string_view text) noexcept
{
    return text.find_first_not_of(' ') == std::string_view::npos;
}

// True when the first record of bytes has keyword with a value: seen in its first ten bytes in the fixed format, in
// which writers give SIMPLE and XTENSION, and parsed as a reader reads any other form.
bool starts_with_keyword(std::span<const std::byte> bytes, std::string_view keyword)
{
    if (bytes.size() >= 10) {
        const std::string_view start = text_of(bytes.first(10));
        if (start.starts_with(keyword) && all_spaces(start.substr(keyword.size(), 8 - keyword.size())) &&
            start.substr(8) == "= ") {
            return true;
        }
    }
    std::vector<diagnostic> ignored;
    const card first = parse_record(record_at(bytes, 0), ignored);
    return first.keyword() == keyword && first.has_value_indicator();
}

// An orphaned CONTINUE record is commentary text in bytes 9 to 80, as a COMMENT record (standard §4.2.1.2).
card commentary_of(const card& value)
{
    return card_access::make(std::string(value.keyword()), {},
                             std::string(trim_trailing_spaces(value.text().substr(8))), std::string(value.text()), 0, 0,
                             false, false);
}

void check_continuations(std::vector<card>& cards, std::uint64_t offset, std::uint64_t hdu, diagnostic_log& log)
{
    for (std::size_t i = 0; i < cards.size(); ++i) {
        const card& current = cards[i];
        const std::uint64_t position = offset + (i * record_length);
        if (current.has_value_indicator() && current.value().kind() == value_kind::string) {
            const std::size_t length = continuation_length(cards, i);
            if (length > 0 && is_reserved_keyword(current.keyword())) {
                log.warning(errc::continued_reserved_keyword,
                            "CONTINUE records continue the string of the reserved keyword " + quote(current.keyword()) +
                                ", which the standard does not allow; the string is joined",
                            {.hdu = hdu, .keyword = std::string(current.keyword()), .offset = position});
            }
            i += length;
        } else if (is_continuation(current)) {
            log.info(errc::orphaned_continue, "a CONTINUE record continues no string, and is read as commentary",
                     {.hdu = hdu, .keyword = "CONTINUE", .offset = position});
            cards[i] = commentary_of(current);
        }
    }
}

void check_repeated(const std::vector<card>& cards, std::uint64_t offset, std::uint64_t hdu, diagnostic_log& log)
{
    // The cards with a value in the order of their keywords, then of their records, in one allocation: a card after
    // the first of its keyword repeats it.
    std::vector<std::size_t> valued;
    valued.reserve(cards.size());
    for (std::size_t i = 0; i < cards.size(); ++i) {
        if (cards[i].has_value_indicator()) {
            valued.push_back(i);
        }
    }
    std::ranges::sort(valued, [&cards](std::size_t a, std::size_t b) {
        const std::string_view left = cards[a].keyword();
        const std::string_view right = cards[b].keyword();
        return left < right || (left == right && a < b);
    });
    std::vector<std::size_t> repeated;
    for (std::size_t k = 1; k < valued.size(); ++k) {
        if (cards[valued[k]].keyword() == cards[valued[k - 1]].keyword()) {
            repeated.push_back(valued[k]);
        }
    }
    std::ranges::sort(repeated);
    for (const std::size_t i : repeated) {
        const card& current = cards[i];
        log.warning(errc::repeated_keyword,
                    "the keyword " + quote(current.keyword()) +
                        " has a value in an earlier record too, which lookups find",
                    {.hdu = hdu, .keyword = std::string(current.keyword()), .offset = offset + (i * record_length)});
    }
}

} // namespace

std::span<const std::byte> read_window::bytes(const thread_safe_source& source, std::uint64_t offset,
                                              std::uint64_t size)
{
    const std::span<const std::byte> held = buffer_.bytes().first(size_);
    const bool after = offset >= offset_;
    if (after && offset - offset_ <= size_ && size <= size_ - (offset - offset_)) {
        return held.subspan(offset - offset_, size);
    }
    // The bytes held count once a read has filled them: a read that fails leaves the window without the bytes it was
    // to read, so that a walk tried again reads them again.
    if (offset == offset_ && size_ != 0) {
        // The same header, longer than the bytes read for it: the rest read after them.
        const std::size_t kept = size_;
        grow(size, kept);
        source.read(offset + kept, buffer_.bytes().subspan(kept, size - kept));
        size_ = size;
        return buffer_.bytes().first(size_);
    }
    // A header within the bytes read or close after them: the units are small, and the reads grow.
    const bool close = after && offset - offset_ < 2 * std::max<std::uint64_t>(size_, first_read);
    ahead_ = close ? std::min(std::max(ahead_ * 2, first_read), most_ahead) : 0;
    const std::size_t wanted = std::min(std::max(size, ahead_), source.size() - offset);
    size_ = 0;
    grow(wanted, 0);
    offset_ = offset;
    source.read(offset, buffer_.bytes().first(wanted));
    size_ = wanted;
    return buffer_.bytes().first(size);
}

void read_window::grow(std::size_t size, std::size_t kept)
{
    if (size <= buffer_.bytes().size()) {
        return;
    }
    scratch_buffer larger(size);
    std::ranges::copy(buffer_.bytes().first(kept), larger.bytes().begin());
    buffer_ = std::move(larger);
}

unit_header read_header(const thread_safe_source& source, std::uint64_t offset, std::uint64_t hdu,
                        std::string_view first_keyword, const limits& limits, diagnostic_log& log, read_window& window)
{
    const std::uint64_t available = source.size() - offset;
    const std::uint64_t cap = limits.max_header_bytes == 0 ? available : std::min(available, limits.max_header_bytes);
    header_bytes bytes(source, offset, window);
    std::span<const std::byte> data = bytes.fetch(std::min(std::max<std::uint64_t>(cap, record_length), first_read));

    if (!starts_with_keyword(data, first_keyword)) {
        return {.outcome = header_outcome::not_a_header};
    }

    // The records up to END, in reads that double the bytes read so far.
    std::uint64_t scanned = 0;
    bool found_end = false;
    while (true) {
        while (scanned < data.size()) {
            // A record that the end of the source cuts is read as if padded with spaces.
            if (data.size() - scanned < record_length) {
                found_end = data.size() == available && record_at(data, scanned).starts_with(end_keyword);
                break;
            }
            if (text_of(data.subspan(scanned, end_keyword.size())) == end_keyword) {
                found_end = true;
                break;
            }
            scanned += record_length;
        }
        if (found_end || data.size() == available) {
            break;
        }
        if (data.size() >= cap) {
            log.error(errc::header_too_large,
                      "the header is longer than the limit of " + std::to_string(limits.max_header_bytes) + " bytes",
                      {.hdu = hdu, .offset = offset});
            return {.outcome = header_outcome::too_large};
        }
        data = bytes.fetch(std::min<std::uint64_t>(cap, data.size() * 2));
    }

    unit_header result;
    const error_context where{.hdu = hdu, .offset = offset};
    std::uint64_t records_end = data.size();
    if (found_end) {
        records_end = scanned;
        const std::uint64_t header_end = scanned + record_length;
        const std::uint64_t block_end = (header_end + block_size - 1) / block_size * block_size;
        result.size = std::min(block_end, available);
        if (limits.max_header_bytes != 0 && result.size > limits.max_header_bytes) {
            log.error(errc::header_too_large,
                      "the header is longer than the limit of " + std::to_string(limits.max_header_bytes) + " bytes",
                      where);
            return {.outcome = header_outcome::too_large};
        }
        data = bytes.fetch(result.size);
        if (!all_spaces(record_at(data, scanned).substr(8))) {
            log.warning(errc::header_fill, "the END record holds other bytes than spaces after END, which are ignored",
                        {.hdu = hdu, .keyword = "END", .offset = offset + scanned});
        }
        if (header_end < data.size() && !all_spaces(text_of(data.subspan(header_end)))) {
            log.warning(errc::header_fill,
                        "the last block of the header holds other bytes than spaces after END, which are ignored",
                        {.hdu = hdu, .offset = offset + header_end});
        }
        if (result.size < block_end) {
            log.warning(errc::truncated_file,
                        "the file ends within the last block of the header, which is read as if it were complete",
                        where);
        }
    } else {
        result.size = available;
        log.warning(errc::missing_end, "the header has no END record before the end of the file", where);
        if (available % record_length != 0) {
            log.warning(errc::truncated_file,
                        "the file ends within a record of the header, which is read as if padded with spaces", where);
        }
    }

    std::vector<diagnostic> departures;
    result.cards.reserve((records_end + record_length - 1) / record_length);
    for (std::uint64_t position = 0; position < records_end; position += record_length) {
        departures.clear();
        card value = position + record_length <= data.size()
                         ? parse_record(text_of(data.subspan(position, record_length)), departures)
                         : parse_record(record_at(data, position), departures);
        for (diagnostic& departure : departures) {
            departure.context.hdu = hdu;
            departure.context.offset = offset + position;
            log.add(departure.severity, departure.code, std::move(departure.message), std::move(departure.context));
        }
        result.cards.push_back(std::move(value));
    }
    check_continuations(result.cards, offset, hdu, log);
    check_repeated(result.cards, offset, hdu, log);
    // The blocks summed while they are at hand, for the header part of CHECKSUM.
    const bool checksum =
        std::ranges::any_of(result.cards, [](const card& value) { return value.keyword() == "CHECKSUM"; });
    if (checksum && found_end && result.size % block_size == 0) {
        result.sum = sum_of(data.first(result.size));
    }
    return result;
}

} // namespace openfits::detail
