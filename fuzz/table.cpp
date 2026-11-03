// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Tables (standard §7.2, §7.3) and random groups (standard §6) read under hostile keywords: the input gives the kind
// of unit, its columns or parameters, the text of each TFORMn and TDIMn, from valid forms or from its bytes, the values
// of TSCALn, TZEROn, TNULLn, TDISPn and THEAP, the sizes, and the bytes that repeat through the rows and the heap, the
// descriptors of variable-length arrays among them. Every read must fail with an error of the library's kinds or give
// values of the sizes it promises, a source in memory and one read through read() must give the same values or the
// same error, and a format that parses must read back the same from the text it writes.

#include <openfits/error.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/table.h>

#include "table/table_description.h"

#include <algorithm>
#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using openfits::errc;
using openfits::reader;

void require(bool condition)
{
    if (!condition) {
        std::abort();
    }
}

// The bytes of the input, read in order; zeros once they run out.
class input_bytes
{
public:
    explicit input_bytes(std::span<const std::byte> data) : data_(data) {}

    std::uint8_t next()
    {
        return at_ < data_.size() ? std::to_integer<std::uint8_t>(data_[at_++]) : 0;
    }

    // Printable text of the next count bytes.
    std::string text(std::size_t count)
    {
        std::string result;
        for (std::size_t i = 0; i < count; ++i) {
            result += static_cast<char>(' ' + (next() % 95));
        }
        return result;
    }

    [[nodiscard]] std::span<const std::byte> rest() const
    {
        return data_.subspan(std::min(at_, data_.size()));
    }

private:
    std::span<const std::byte> data_;
    std::size_t at_ = 0;
};

std::string record(std::string text)
{
    text.resize(80, ' ');
    return text;
}

std::string card(std::string_view keyword, std::string_view value)
{
    std::string text(keyword);
    text.resize(8, ' ');
    return record(text + "= " + std::string(value));
}

std::string string_value(std::string_view text)
{
    std::string result = "'";
    for (const char c : text) {
        result += c;
        if (c == '\'') {
            result += '\'';
        }
    }
    return result + "'";
}

constexpr std::array<std::string_view, 24> binary_forms{
    "1L",   "13X",    "1B",     "2I",     "1J",      "3K",  "8A",     "1E", "2D", "1C",     "1M", "0J",
    "12A4", "1PJ(4)", "1QD(3)", "1PA(8)", "1PX(20)", "1PL", "1QC(2)", "PB", "6I", "1PE(2)", "4L", "20A",
};

constexpr std::array<std::string_view, 10> ascii_forms{"A8", "I6", "F8.3", "E12.4", "D20.10",
                                                       "A1", "I1", "F4.0", "E8.1",  "A20"};

// The errors that a read may give for a hostile file or hostile options.
bool expected_error(errc code)
{
    switch (code) {
    case errc::invalid_argument:
    case errc::arithmetic_overflow:
    case errc::allocation_too_large:
    case errc::value_out_of_range:
    case errc::undefined_value:
    case errc::reserved_keyword:
    case errc::keyword_type:
    case errc::keyword_not_found:
    case errc::keyword_undefined:
    case errc::invalid_structure:
    case errc::missing_keyword:
    case errc::not_a_table:
    case errc::not_random_groups:
    case errc::unsupported_compression:
    case errc::end_of_data:
    case errc::invalid_column_format:
    case errc::invalid_dimensions:
    case errc::column_not_found:
    case errc::column_type:
    case errc::invalid_descriptor:
    case errc::invalid_field:
    case errc::array_too_large:
    case errc::invalid_display_format:
        return true;
    default:
        return false;
    }
}

// What a read gave: its bytes, or the code of its error.
struct outcome
{
    std::vector<std::byte> values{};
    std::vector<std::uint64_t> offsets{};
    std::optional<errc> error{};

    friend bool operator==(const outcome&, const outcome&) = default;
};

template <typename Function> outcome attempt(const Function& function)
{
    outcome result;
    try {
        function(result);
    } catch (const openfits::error& failure) {
        require(expected_error(failure.code()));
        result.error = failure.code();
    }
    return result;
}

template <typename T> void keep(outcome& result, const std::vector<T>& values)
{
    result.values.resize(values.size() * sizeof(T));
    if (!values.empty()) {
        std::memcpy(result.values.data(), values.data(), result.values.size());
    }
}

// The reads of a column, each under the options; a column of variable-length arrays is read as arrays.
std::vector<outcome> read_column(const reader& input, std::size_t column, const openfits::column_read_options& options)
{
    std::vector<outcome> results;
    results.push_back(attempt([&](outcome& result) { keep(result, input.read_column<double>(1, column, options)); }));
    results.push_back(
        attempt([&](outcome& result) { keep(result, input.read_column<std::int16_t>(1, column, options)); }));
    results.push_back(
        attempt([&](outcome& result) { keep(result, input.read_column<std::complex<float>>(1, column, options)); }));
    results.push_back(attempt([&](outcome& result) {
        const openfits::column_arrays<std::int64_t> arrays = input.read_arrays<std::int64_t>(1, column, options);
        require(!arrays.offsets.empty() && arrays.offsets.back() == arrays.values.size());
        for (std::size_t i = 1; i < arrays.offsets.size(); ++i) {
            require(arrays.offsets[i - 1] <= arrays.offsets[i]);
        }
        keep(result, arrays.values);
        result.offsets = arrays.offsets;
    }));
    results.push_back(attempt([&](outcome& result) {
        openfits::column_read_options strings = options;
        strings.blank = openfits::blank_policy::nan;
        for (const std::string& text : input.read_strings(1, column, strings)) {
            keep(result, std::vector<char>(text.begin(), text.end()));
        }
    }));
    return results;
}

// Formats that parse write a text that parses back the same (standard §7.2.1, §7.3.1), and so do dimensions.
void check_grammar(std::string_view text, bool ascii)
{
    try {
        const openfits::column_format format =
            ascii ? openfits::column_format::parse_ascii(text) : openfits::column_format::parse_binary(text);
        const std::string written = format.to_string();
        const openfits::column_format again =
            ascii ? openfits::column_format::parse_ascii(written) : openfits::column_format::parse_binary(written);
        require(again == format);
        (void)format.field_size();
    } catch (const openfits::error& failure) {
        require(expected_error(failure.code()));
    }
    if (const std::optional<std::vector<std::uint64_t>> dimensions = openfits::detail::parse_dimensions(text)) {
        require(openfits::detail::parse_dimensions(openfits::detail::format_dimensions(*dimensions)) == dimensions);
    }
}

std::string table_header(input_bytes& in, bool ascii, std::size_t& data_size)
{
    const std::size_t fields = 1 + (in.next() % 4);
    std::vector<std::string> forms;
    std::vector<std::string> extra;
    std::uint64_t row_size = 0;
    for (std::size_t n = 1; n <= fields; ++n) {
        const std::uint8_t choice = in.next();
        std::string form;
        if (choice % 4 == 0) {
            form = in.text(1 + (in.next() % 10));
        } else if (ascii) {
            form = std::string(ascii_forms[choice % ascii_forms.size()]);
        } else {
            form = std::string(binary_forms[choice % binary_forms.size()]);
        }
        check_grammar(form, ascii);
        forms.push_back(form);
        try {
            const openfits::column_format format =
                ascii ? openfits::column_format::parse_ascii(form) : openfits::column_format::parse_binary(form);
            row_size += std::min<std::uint64_t>(format.field_size(), 64);
        } catch (const openfits::error&) {
            row_size += 4;
        }
        const std::uint8_t present = in.next();
        const std::string index = std::to_string(n);
        if ((present & 1U) != 0) {
            extra.push_back(card("TSCAL" + index, in.text(8)));
        }
        if ((present & 2U) != 0) {
            extra.push_back(card("TZERO" + index, in.text(8)));
        }
        if ((present & 4U) != 0) {
            extra.push_back(card("TNULL" + index, (present & 8U) != 0 ? string_value(in.text(4)) : in.text(6)));
        }
        if ((present & 16U) != 0) {
            const std::string dimensions = (present & 32U) != 0 ? in.text(8)
                                                                : "(" + std::to_string(1 + in.next() % 4) + "," +
                                                                      std::to_string(1 + in.next() % 4) + ")";
            check_grammar(dimensions, false);
            extra.push_back(card("TDIM" + index, string_value(dimensions)));
        }
        if ((present & 64U) != 0) {
            extra.push_back(card("TDISP" + index, string_value(in.text(6))));
        }
        if ((present & 128U) != 0) {
            extra.push_back(card("TTYPE" + index, string_value(in.text(4))));
        }
    }
    // The row size of the formats, or of the input, and its rows and heap.
    const std::uint8_t sizes = in.next();
    if ((sizes & 1U) != 0) {
        row_size = in.next() % 96U;
    }
    const std::uint64_t rows = in.next() % 17U;
    const std::uint64_t pcount = ascii ? 0U : in.next() % 200U;
    std::string header = card("XTENSION", ascii ? "'TABLE   '" : "'BINTABLE'") + card("BITPIX", "8") +
                         card("NAXIS", "2") + card("NAXIS1", std::to_string(row_size)) +
                         card("NAXIS2", std::to_string(rows)) + card("PCOUNT", std::to_string(pcount)) +
                         card("GCOUNT", "1") + card("TFIELDS", std::to_string(fields));
    std::uint64_t column = 1;
    for (std::size_t n = 1; n <= fields; ++n) {
        const std::string index = std::to_string(n);
        if (ascii) {
            header += card("TBCOL" + index, std::to_string(column));
            column += 1U + (in.next() % 9U);
        }
        header += card("TFORM" + index, string_value(forms[n - 1]));
    }
    for (const std::string& text : extra) {
        header += text;
    }
    if ((sizes & 2U) != 0) {
        header += card("THEAP", std::to_string((row_size * rows) + (in.next() % 32) - 8));
    }
    header += record("END");
    header.resize((header.size() + 2879) / 2880 * 2880, ' ');
    data_size = (row_size * rows) + pcount;
    return header;
}

std::string groups_header(input_bytes& in, std::size_t& data_size)
{
    constexpr std::array<int, 6> bitpix_values{8, 16, 32, 64, -32, -64};
    const int bitpix = bitpix_values[in.next() % bitpix_values.size()];
    const std::size_t naxis = 1 + (in.next() % 3);
    std::string header = card("SIMPLE", "T") + card("BITPIX", std::to_string(bitpix)) +
                         card("NAXIS", std::to_string(naxis)) + card("NAXIS1", "0");
    std::uint64_t elements = 1;
    for (std::size_t a = 2; a <= naxis; ++a) {
        const std::uint64_t length = 1 + (in.next() % 4);
        elements *= length;
        header += card("NAXIS" + std::to_string(a), std::to_string(length));
    }
    const std::uint64_t parameters = in.next() % 5;
    const std::uint64_t groups = in.next() % 6;
    header += card("GROUPS", "T") + card("PCOUNT", std::to_string(parameters)) + card("GCOUNT", std::to_string(groups));
    for (std::uint64_t p = 1; p <= parameters; ++p) {
        const std::uint8_t present = in.next();
        const std::string index = std::to_string(p);
        if ((present & 1U) != 0) {
            header += card("PTYPE" + index, string_value((present & 8U) != 0 ? "SAME" : in.text(3)));
        }
        if ((present & 2U) != 0) {
            header += card("PSCAL" + index, in.text(8));
        }
        if ((present & 4U) != 0) {
            header += card("PZERO" + index, in.text(8));
        }
    }
    header += record("END");
    header.resize((header.size() + 2879) / 2880 * 2880, ' ');
    data_size = groups * (parameters + elements) * static_cast<std::uint64_t>(bitpix < 0 ? -bitpix : bitpix) / 8;
    return header;
}

std::unique_ptr<openfits::input_source> source_of(const std::vector<std::byte>& file, bool in_memory)
{
    if (in_memory) {
        return std::make_unique<openfits::memory_source>(std::vector<std::byte>(file));
    }
    // A source that the reader reads through read(), in runs: it gives no bytes in memory.
    class streamed final : public openfits::input_source
    {
    public:
        explicit streamed(std::vector<std::byte> data) : data_(std::move(data)) {}
        std::uint64_t size() const override
        {
            return data_.size();
        }
        void read(std::uint64_t offset, std::span<std::byte> destination) const override
        {
            // A read of no bytes has a span that may be null, which memcpy() must not get.
            std::ranges::copy(std::span<const std::byte>(data_).subspan(offset, destination.size()),
                              destination.begin());
        }

    private:
        std::vector<std::byte> data_;
    };
    return std::make_unique<streamed>(file);
}

} // namespace

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    input_bytes in({reinterpret_cast<const std::byte*>(data), size});
    const std::uint8_t kind = in.next() % 3;
    std::size_t data_size = 0;
    std::string headers;
    if (kind == 2) {
        headers = groups_header(in, data_size);
    } else {
        headers = card("SIMPLE", "T") + card("BITPIX", "8") + card("NAXIS", "0") + record("END");
        headers.resize(2880, ' ');
        headers += table_header(in, kind == 1, data_size);
    }
    openfits::column_read_options options;
    options.blank = static_cast<openfits::blank_policy>(in.next() % 4);
    options.blank_value = static_cast<double>(static_cast<std::int8_t>(in.next()));
    options.overflow = static_cast<openfits::overflow_policy>(in.next() % 2);
    options.rounding = static_cast<openfits::rounding>(in.next() % 2);
    options.scaling = static_cast<openfits::scaling_policy>(in.next() % 3);
    options.override_scaling = {.scale = static_cast<double>(static_cast<std::int8_t>(in.next())) / 4.0,
                                .zero = static_cast<double>(static_cast<std::int8_t>(in.next()))};
    options.trim_strings = (in.next() % 2) != 0;
    if ((in.next() % 2) != 0) {
        const auto first = static_cast<std::uint64_t>(in.next() % 8);
        const auto count = static_cast<std::uint64_t>(in.next() % 8);
        options.rows = openfits::row_range{.first = first, .count = count};
    }
    const bool truncated = (in.next() % 8) == 0;

    // The data: the rest of the input, repeated, padded to the block, and cut short when truncated.
    std::vector<std::byte> file(headers.size());
    std::memcpy(file.data(), headers.data(), headers.size());
    const std::span<const std::byte> pattern = in.rest();
    const std::size_t padded = (data_size + 2879) / 2880 * 2880;
    for (std::size_t i = 0; i < padded; ++i) {
        file.push_back(i < data_size && !pattern.empty() ? pattern[i % pattern.size()] : std::byte{0});
    }
    if (truncated && file.size() > headers.size()) {
        file.resize(headers.size() + ((file.size() - headers.size()) / 2));
    }

    openfits::read_options limits;
    limits.limits.max_allocation = std::uint64_t{1} << 22;
    limits.limits.max_vla_elements = 4096;
    limits.limits.max_heap_bytes = std::uint64_t{1} << 20;
    std::vector<std::vector<outcome>> results;
    for (const bool in_memory : {true, false}) {
        std::vector<outcome> reads;
        try {
            const reader input(source_of(file, in_memory), limits);
            if (kind == 2) {
                const openfits::groups_info groups = input.groups(0);
                for (std::size_t p = 0; p < groups.parameters.size(); ++p) {
                    reads.push_back(attempt([&](outcome& result) {
                        keep(result, input.read_parameter<double>(0, p, {.overflow = options.overflow}));
                    }));
                }
                reads.push_back(
                    attempt([&](outcome& result) { keep(result, input.read_parameter<float>(0, "SAME")); }));
                for (std::uint64_t g = 0; g < groups.count; ++g) {
                    reads.push_back(
                        attempt([&](outcome& result) { keep(result, input.read_group<std::int32_t>(0, g)); }));
                }
            } else {
                const openfits::table_info table = input.table(1);
                for (std::size_t c = 0; c < table.columns.size(); ++c) {
                    const std::vector<outcome> column = read_column(input, c, options);
                    reads.insert(reads.end(), column.begin(), column.end());
                }
            }
        } catch (const openfits::error& failure) {
            require(expected_error(failure.code()));
            reads.push_back({.error = failure.code()});
        }
        results.push_back(std::move(reads));
    }
    require(results[0] == results[1]);
    return 0;
}
