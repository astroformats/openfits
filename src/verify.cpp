// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/verify.h>

#include "checksum/checksum.h"
#include "core/ascii.h"
#include "core/endian.h"
#include "core/quote.h"
#include "hdu/copy.h"
#include "hdu/header_reader.h"
#include "io/thread_safe_source.h"
#include "table/ascii_field.h"
#include "table/table_description.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace openfits {

namespace {

// A problem that repeats is listed this many times, and the next finding of it counts the others.
constexpr std::uint64_t listed_repeats = 10;
// The bytes of rows read at a time, and of a field read at a time when its rows are larger.
constexpr std::uint64_t data_chunk = std::uint64_t{4} << 20;

// The findings of a report, those of a problem that repeats listed up to listed_repeats times.
class report
{
public:
    void add(diagnostic finding)
    {
        findings_.push_back(std::move(finding));
    }

    void add(severity level, errc code, std::string_view section, std::string message, error_context context)
    {
        add({.severity = level,
             .code = code,
             .section = std::string(section),
             .message = std::move(message),
             .context = std::move(context)});
    }

    // A finding of a problem that may repeat, which key names, such as the invalid fields of a column: listed up to
    // listed_repeats times, then counted in one finding of the problem, whose message says how many more of what were
    // found.
    void add_repeated(const std::string& key, std::string_view what, diagnostic finding)
    {
        repeat& seen = repeats_[key];
        ++seen.count;
        if (seen.count <= listed_repeats) {
            add(std::move(finding));
            return;
        }
        const std::uint64_t more = seen.count - listed_repeats;
        const std::string message = std::to_string(more) + " more " + std::string(what) + ", not listed";
        if (!seen.summary) {
            seen.summary = findings_.size();
            finding.context.offset.reset();
            finding.message = message;
            add(std::move(finding));
        } else {
            findings_[*seen.summary].message = message;
        }
    }

    [[nodiscard]] std::vector<diagnostic> take() noexcept
    {
        return std::exchange(findings_, {});
    }

private:
    struct repeat
    {
        std::uint64_t count = 0;
        std::optional<std::size_t> summary{};
    };

    std::vector<diagnostic> findings_{};
    std::map<std::string, repeat> repeats_{};
};

// The progress of a verification over the bytes of the file, which never goes back, though the checks of a unit read
// its bytes more than once.
class verification_progress
{
public:
    verification_progress(const progress_function& progress, std::uint64_t total) : progress_(&progress), total_(total)
    {}

    // Reports that the bytes before position are verified; false when the progress function asks to stop.
    [[nodiscard]] bool advance(std::uint64_t position) const
    {
        reached_ = std::max(reached_, std::min(position, total_));
        return !*progress_ || (*progress_)(reached_, total_);
    }

    void reach(std::uint64_t position) const
    {
        if (!advance(position)) {
            throw cancelled_error(errc::cancelled, "the verification was cancelled");
        }
    }

    // The progress of a verification of the unit that starts at offset, whose bytes map to those of the file.
    [[nodiscard]] progress_function within(std::uint64_t offset) const
    {
        if (!*progress_) {
            return {};
        }
        return [this, offset](std::uint64_t done, std::uint64_t) { return advance(offset + done); };
    }

private:
    const progress_function* progress_;
    std::uint64_t total_;
    mutable std::uint64_t reached_ = 0;
};

// The message of an error without the context that what() appends to it, which a finding holds apart.
std::string message_of(const error& failure)
{
    std::string text = failure.what();
    const error_context& context = failure.context();
    const bool placed = context.hdu || !context.keyword.empty() || context.column || context.offset;
    if (const std::size_t open = text.rfind(" ("); placed && open != std::string::npos && text.ends_with(')')) {
        text.erase(open);
    }
    return text;
}

// The section of the standard that gives the fill of the data of a unit of the kind; empty for the kinds whose fill
// it does not give.
std::string_view fill_section(hdu_kind kind) noexcept
{
    switch (kind) {
    case hdu_kind::primary:
        return "3.3.2";
    case hdu_kind::random_groups:
        return "6.2";
    case hdu_kind::image:
        return "7.1.3";
    case hdu_kind::table:
        return "7.2.3";
    case hdu_kind::bintable:
    case hdu_kind::compressed_image:
    case hdu_kind::compressed_table:
        return "7.3.3";
    case hdu_kind::unknown:
        break;
    }
    return {};
}

bool has_error(const hdu_info& unit)
{
    return std::ranges::any_of(unit.diagnostics(),
                               [](const diagnostic& entry) { return entry.severity == severity::error; });
}

// The description that the reads take of a unit whose header the walk found no error in: an error of it is a finding
// of the section of its kind.
void check_description(const reader& file, const hdu_info& unit, report& out)
{
    if (has_error(unit) || !unit.data_size()) {
        return;
    }
    const auto refused = [&out, &unit](const error& failure, errc code) {
        std::string_view section;
        switch (unit.kind()) {
        case hdu_kind::compressed_image:
            section = code == errc::unsupported_compression ? "10.4" : "10.1";
            break;
        case hdu_kind::compressed_table:
            section = code == errc::unsupported_compression ? "10.3.5" : "10.3";
            break;
        case hdu_kind::table:
            section = "7.2";
            break;
        case hdu_kind::bintable:
            section = "7.3";
            break;
        case hdu_kind::random_groups:
            section = "6";
            break;
        case hdu_kind::primary:
        case hdu_kind::image:
            section = "4.4.1";
            break;
        case hdu_kind::unknown:
            break;
        }
        error_context context = failure.context();
        context.hdu = unit.index();
        out.add(severity::error, code, section, message_of(failure), std::move(context));
    };
    try {
        switch (unit.kind()) {
        case hdu_kind::primary:
        case hdu_kind::image:
        case hdu_kind::compressed_image:
            (void)file.image(unit.index());
            break;
        case hdu_kind::table:
        case hdu_kind::bintable:
        case hdu_kind::compressed_table:
            (void)file.table(unit.index());
            break;
        case hdu_kind::random_groups:
            (void)file.groups(unit.index());
            break;
        case hdu_kind::unknown:
            break;
        }
    } catch (const invalid_data_error& failure) {
        refused(failure, failure.code());
    } catch (const unsupported_error& failure) {
        refused(failure, failure.code());
    } catch (const limit_error& failure) {
        refused(failure, failure.code());
    }
}

// ZQUANTIZ = 'NONE', a convention outside the standard, which the reader reads as an image stored without quantization.
void check_quantization(const hdu_info& unit, report& out)
{
    if (unit.kind() != hdu_kind::compressed_image) {
        return;
    }
    const header& cards = unit.header();
    const std::optional<std::size_t> position = cards.index_of("ZQUANTIZ");
    if (!position || cards[*position].value().kind() != value_kind::string) {
        return;
    }
    if (detail::trim_trailing_spaces(cards[*position].value().get<std::string>()) == "NONE") {
        out.add(severity::info, errc::invalid_compression, "10.2.1",
                "ZQUANTIZ is 'NONE', a convention outside the standard, whose values are NO_DITHER, "
                "SUBTRACTIVE_DITHER_1 and SUBTRACTIVE_DITHER_2: the image is read as stored, without a quantization",
                {.hdu = unit.index(), .keyword = "ZQUANTIZ"});
    }
}

// The fill of the last block of the data of a unit: zeros, or spaces in an ASCII table.
void check_fill(const detail::thread_safe_source& source, const hdu_info& unit, report& out)
{
    const std::string_view section = fill_section(unit.kind());
    const std::optional<std::uint64_t> data_size = unit.data_size();
    if (section.empty() || !data_size) {
        return;
    }
    const std::uint64_t size = *data_size;
    const std::uint64_t records = detail::data_records(size);
    const std::uint64_t offset = unit.data_offset();
    if (records > std::numeric_limits<std::uint64_t>::max() - offset || offset + records > source.size()) {
        // A truncated unit, which the walk reported.
        return;
    }
    const std::uint64_t fill_size = records - size;
    if (fill_size == 0) {
        return;
    }
    std::array<std::byte, detail::block_size> fill{};
    const std::span<std::byte> bytes = std::span(fill).first(fill_size);
    source.read(offset + size, bytes);
    const bool ascii = unit.kind() == hdu_kind::table;
    const std::byte expected = ascii ? std::byte{' '} : std::byte{0};
    const auto other = std::ranges::find_if(bytes, [expected](std::byte value) { return value != expected; });
    if (other != bytes.end()) {
        out.add(severity::info, errc::data_fill, section,
                std::string("the fill of the last block of the data holds other bytes than ") +
                    (ascii ? "spaces" : "zeros"),
                {.hdu = unit.index(), .offset = offset + size + static_cast<std::uint64_t>(other - bytes.begin())});
    }
}

// What a field of a table is checked for.
enum class field_check : std::uint8_t
{
    integer,    // an Iw field of an ASCII table
    real,       // an Fw.d, Ew.d or Dw.d field
    text,       // an Aw field: ASCII text
    logical,    // L of a binary table: T, F or a 0 byte
    characters, // A of a binary table: ASCII text up to the first NUL
    descriptor, // P or Q: an array within the heap
};

struct checked_field
{
    // The number of the column, from 1, its description and its format, and the check of its fields, whose bytes are
    // size, and whose decimals are those of a real field of an ASCII table.
    std::uint64_t number = 0;
    const column_info* column = nullptr;
    const column_format* format = nullptr;
    field_check check = field_check::integer;
    std::uint64_t size = 0;
    std::uint32_t decimals = 0;
};

// The checks of the fields of a table, those that reads make only of the fields they read and those that they do not
// make.
class field_checks
{
public:
    field_checks(const hdu_info& unit, const table_info& table, report& out)
        : unit_(&unit), table_(&table), out_(&out), ascii_(table.kind == hdu_kind::table)
    {
        for (std::size_t i = 0; i < table.columns.size(); ++i) {
            const column_info& column = table.columns[i];
            if (!column.format) {
                continue;
            }
            const column_format& format = *column.format;
            std::optional<field_check> check;
            if (ascii_) {
                check = format.type == column_type::character ? field_check::text
                        : format.type == column_type::int64   ? field_check::integer
                                                              : field_check::real;
            } else if (format.storage != column_storage::fixed) {
                check = field_check::descriptor;
            } else if (format.type == column_type::logical) {
                check = field_check::logical;
            } else if (format.type == column_type::character) {
                check = field_check::characters;
            }
            if (check && format.repeat > 0) {
                fields_.push_back({.number = i + 1,
                                   .column = &column,
                                   .format = &format,
                                   .check = *check,
                                   .size = format.field_size(),
                                   .decimals = format.field ? format.field->digits.value_or(0) : 0});
            }
        }
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return fields_.empty();
    }

    [[nodiscard]] const std::vector<checked_field>& fields() const noexcept
    {
        return fields_;
    }

    // Checks the field of a row, or a piece of it at offset from its start, which a field larger than the chunks of a
    // read takes; bytes start at the file offset at.
    void check(const checked_field& field, std::uint64_t row, std::span<const std::byte> bytes, std::uint64_t piece,
               std::uint64_t at)
    {
        switch (field.check) {
        case field_check::integer:
        case field_check::real:
            check_number(field, row, text_of(bytes), at);
            break;
        case field_check::text:
            check_text(field, row, bytes, at, "7.2.5", false);
            break;
        case field_check::characters:
            check_text(field, row, bytes, at, "7.3.3.1", true, piece);
            break;
        case field_check::logical:
            check_logicals(field, row, bytes, at);
            break;
        case field_check::descriptor:
            check_descriptor(field, row, bytes, at);
            break;
        }
    }

private:
    static std::string_view text_of(std::span<const std::byte> bytes) noexcept
    {
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }

    [[nodiscard]] std::string key(const checked_field& field, std::string_view problem) const
    {
        return std::to_string(unit_->index()) + ':' + std::to_string(field.number) + ':' + std::string(problem);
    }

    [[nodiscard]] diagnostic finding(severity level, errc code, std::string_view section, std::string message,
                                     const checked_field& field, std::uint64_t at) const
    {
        return {.severity = level,
                .code = code,
                .section = std::string(section),
                .message = std::move(message),
                .context = {.hdu = unit_->index(), .column = field.number, .offset = at}};
    }

    void check_number(const checked_field& field, std::uint64_t row, std::string_view text, std::uint64_t at)
    {
        if (detail::is_null_field(text, field.column->null_string)) {
            return;
        }
        const std::uint32_t decimals = field.decimals;
        const bool integer = field.check == field_check::integer;
        const detail::field_number number =
            integer ? detail::read_integer_field(text) : detail::read_real_field(text, decimals);
        const std::string where = "the field of row " + std::to_string(row + 1) + ", " + detail::quote(text);
        if (number.problem) {
            out_->add_repeated(key(field, "number"), "fields of the column that are not numbers",
                               finding(severity::error, *number.problem, "7.2.5",
                                       where + (*number.problem == errc::value_out_of_range
                                                    ? ", is a number beyond the range of the reads"
                                                    : ", is not a number of its format"),
                                       field, at));
            return;
        }
        const detail::field_departure departure =
            integer ? detail::integer_field_departure(text) : detail::real_field_departure(text, decimals);
        switch (departure) {
        case detail::field_departure::none:
            break;
        case detail::field_departure::embedded_spaces:
            out_->add_repeated(key(field, "spaces"), "fields of the column with spaces inside their numbers",
                               finding(severity::warning, errc::invalid_field, "7.2.5",
                                       where + ", has spaces inside its number, which the standard forbids", field,
                                       at));
            break;
        case detail::field_departure::lowercase_exponent:
            out_->add_repeated(key(field, "exponent"), "fields of the column with a lowercase exponent letter",
                               finding(severity::warning, errc::invalid_field, "7.2.5",
                                       where + ", has an exponent letter in lower case, where the standard has E and D",
                                       field, at));
            break;
        case detail::field_departure::implied_decimal_point:
            out_->add_repeated(key(field, "point"), "fields of the column with an implied decimal point",
                               finding(severity::info, errc::invalid_field, "7.2.5",
                                       where + ", has no decimal point, and its last " + std::to_string(decimals) +
                                           " digits are its decimals, which the standard deprecates",
                                       field, at));
            break;
        }
    }

    // Text of ASCII characters, 0x20 to 0x7E, up to the first NUL of a field of a binary table when nul_ends.
    void check_text(const checked_field& field, std::uint64_t row, std::span<const std::byte> bytes, std::uint64_t at,
                    std::string_view section, bool nul_ends, std::uint64_t piece = 0)
    {
        if (piece == 0) {
            ended_ = false;
        }
        if (ended_) {
            return;
        }
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            const auto value = std::to_integer<unsigned>(bytes[i]);
            if (nul_ends && value == 0) {
                ended_ = true;
                return;
            }
            if (value < 0x20 || value > 0x7E) {
                out_->add_repeated(key(field, "text"), "fields of the column with bytes outside ASCII text",
                                   finding(severity::warning, errc::invalid_character, section,
                                           "the field of row " + std::to_string(row + 1) + " holds the byte " +
                                               std::to_string(value) + ", which is not an ASCII text character",
                                           field, at + i));
                return;
            }
        }
    }

    void check_logicals(const checked_field& field, std::uint64_t row, std::span<const std::byte> bytes,
                        std::uint64_t at)
    {
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            const auto value = std::to_integer<unsigned>(bytes[i]);
            if (value != 'T' && value != 'F' && value != 0) {
                out_->add_repeated(key(field, "logical"), "fields of the column with values that are not logicals",
                                   finding(severity::error, errc::invalid_field, "7.3.3.1",
                                           "the field of row " + std::to_string(row + 1) + " holds the byte " +
                                               std::to_string(value) + ", which is neither T, F nor 0",
                                           field, at + i));
                return;
            }
        }
    }

    void check_descriptor(const checked_field& field, std::uint64_t row, std::span<const std::byte> bytes,
                          std::uint64_t at)
    {
        const column_format& format = *field.format;
        std::uint64_t count = 0;
        std::uint64_t offset = 0;
        if (format.storage == column_storage::variable_p) {
            count = detail::load_big_endian<std::uint32_t>(bytes.first<4>());
            offset = detail::load_big_endian<std::uint32_t>(bytes.subspan<4, 4>());
        } else {
            count = detail::load_big_endian<std::uint64_t>(bytes.first<8>());
            offset = detail::load_big_endian<std::uint64_t>(bytes.subspan<8, 8>());
            // The descriptors of Q are signed: a value with its top bit set is negative.
            constexpr std::uint64_t sign = std::uint64_t{1} << 63U;
            if ((count & sign) != 0 || (offset & sign) != 0) {
                out_->add_repeated(
                    key(field, "negative"), "descriptors of the column with a negative value",
                    finding(severity::error, errc::invalid_descriptor, "7.3.5",
                            "the descriptor of row " + std::to_string(row + 1) + " holds a negative value", field, at));
                return;
            }
        }
        const std::uint64_t element = element_size(format.type);
        const std::uint64_t size =
            format.type == column_type::bit ? (count / 8) + static_cast<std::uint64_t>(count % 8 != 0)
            : count > std::numeric_limits<std::uint64_t>::max() / element ? std::numeric_limits<std::uint64_t>::max()
                                                                          : count * element;
        if (offset > table_->heap_size || size > table_->heap_size - offset) {
            out_->add_repeated(key(field, "heap"), "descriptors of the column that point outside the heap",
                               finding(severity::error, errc::invalid_descriptor, "7.3.5",
                                       "the descriptor of row " + std::to_string(row + 1) + " points to " +
                                           std::to_string(size) + " bytes at byte " + std::to_string(offset) +
                                           " of the heap, which has " + std::to_string(table_->heap_size),
                                       field, at));
            return;
        }
        if (format.max_elements && count > *format.max_elements) {
            out_->add_repeated(key(field, "emax"), "descriptors of the column beyond its emax",
                               finding(severity::warning, errc::invalid_descriptor, "7.3.5",
                                       "the array of row " + std::to_string(row + 1) + " has " + std::to_string(count) +
                                           " elements, more than the " + std::to_string(*format.max_elements) +
                                           " that emax of TFORM" + std::to_string(field.number) + " gives",
                                       field, at));
        }
    }

    static std::uint64_t element_size(column_type type) noexcept
    {
        switch (type) {
        case column_type::int16:
            return 2;
        case column_type::int32:
        case column_type::float32:
            return 4;
        case column_type::int64:
        case column_type::float64:
        case column_type::complex_float32:
            return 8;
        case column_type::complex_float64:
            return 16;
        case column_type::logical:
        case column_type::bit:
        case column_type::uint8:
        case column_type::character:
            break;
        }
        return 1;
    }

    const hdu_info* unit_;
    const table_info* table_;
    report* out_;
    bool ascii_;
    std::vector<checked_field> fields_{};
    // Whether the characters of the field being checked in pieces have ended at a NUL.
    bool ended_ = false;
};

// The fields of the rows of a table that the file holds whole, read in chunks of rows, or field by field in pieces when
// a row is larger than a chunk.
void check_table_data(const detail::thread_safe_source& source, const hdu_info& unit, report& out,
                      const verification_progress& progress)
{
    const hdu_kind kind = unit.kind();
    if (kind != hdu_kind::table && kind != hdu_kind::bintable && kind != hdu_kind::compressed_image &&
        kind != hdu_kind::compressed_table) {
        return;
    }
    if (has_error(unit) || !unit.data_size()) {
        return;
    }
    table_info table;
    try {
        // A compressed image or table is checked as the binary table that holds it.
        table = detail::describe_table(unit, false);
    } catch (const error&) {
        return;
    }
    field_checks checks(unit, table, out);
    if (checks.empty() || table.row_size == 0) {
        return;
    }
    const std::uint64_t offset = unit.data_offset();
    // The rows that the file holds whole: a truncated unit has fewer than its header gives.
    const std::uint64_t present =
        std::min(unit.data_size().value_or(0), source.size() - std::min(source.size(), offset));
    const std::uint64_t rows = std::min(table.rows, present / table.row_size);
    if (table.row_size <= data_chunk) {
        const std::uint64_t per_chunk = data_chunk / table.row_size;
        const std::size_t chunk_bytes = std::min(rows, per_chunk) * table.row_size;
        std::vector<std::byte> chunk(chunk_bytes);
        for (std::uint64_t first = 0; first < rows; first += per_chunk) {
            const std::uint64_t count = std::min(per_chunk, rows - first);
            const std::size_t bytes = count * table.row_size;
            const std::uint64_t start = offset + (first * table.row_size);
            source.read(start, std::span(chunk).first(bytes));
            for (std::uint64_t r = 0; r < count; ++r) {
                const std::size_t row_start = r * table.row_size;
                const std::span<const std::byte> row = std::span(chunk).subspan(row_start, table.row_size);
                for (const checked_field& field : checks.fields()) {
                    const std::size_t field_offset = field.column->offset;
                    const std::size_t field_size = field.size;
                    checks.check(field, first + r, row.subspan(field_offset, field_size), 0,
                                 start + row_start + field_offset);
                }
            }
            progress.reach(start + bytes);
        }
        return;
    }
    std::vector<std::byte> piece;
    for (std::uint64_t r = 0; r < rows; ++r) {
        for (const checked_field& field : checks.fields()) {
            const std::uint64_t field_start = offset + (r * table.row_size) + field.column->offset;
            for (std::uint64_t done = 0; done < field.size;) {
                const std::size_t size = std::min(field.size - done, data_chunk);
                piece.resize(size);
                source.read(field_start + done, piece);
                checks.check(field, r, piece, done, field_start + done);
                done += size;
            }
        }
        progress.reach(offset + ((r + 1) * table.row_size));
    }
}

// DATASUM and CHECKSUM, verified against the whole unit (standard §4.4.2.7).
void check_checksums(const reader& file, const hdu_info& unit, report& out, const verification_progress& progress)
{
    const header& cards = unit.header();
    if (!unit.data_size() || (!detail::datasum_of(cards).known && !detail::has_checksum(cards))) {
        return;
    }
    const checksum_report result = file.verify(unit.index(), {.progress = progress.within(unit.header_offset())});
    if (result.datasum == checksum_state::invalid) {
        out.add(severity::error, errc::checksum_mismatch, "4.4.2.7",
                "the data records sum to " + std::to_string(result.data_sum) + ", which DATASUM does not record",
                {.hdu = unit.index(), .keyword = "DATASUM"});
    }
    if (result.checksum == checksum_state::invalid) {
        out.add(severity::error, errc::checksum_mismatch, "4.4.2.7",
                "the unit sums to " + std::to_string(result.hdu_sum) +
                    ", not to negative zero, 4294967295, as CHECKSUM makes it",
                {.hdu = unit.index(), .keyword = "CHECKSUM"});
    }
}

// The string of a keyword without trailing spaces, in upper case, or nothing.
std::optional<std::string> upper_string(const header& cards, std::string_view keyword)
{
    const std::optional<std::size_t> position = cards.index_of(keyword);
    if (!position || cards[*position].value().kind() != value_kind::string) {
        return std::nullopt;
    }
    std::string text(detail::trim_trailing_spaces(cards[*position].value().get<std::string>()));
    std::ranges::transform(text, text.begin(), detail::to_upper);
    return text;
}

std::int64_t integer_or_one(const header& cards, std::string_view keyword)
{
    try {
        return cards.find<std::int64_t>(keyword).value_or(1);
    } catch (const invalid_data_error&) {
        return 1;
    }
}

// Units of one type with the same EXTNAME, EXTVER and EXTLEVEL, which standard §4.4.2.6 recommends against, the primary
// HDU being of the type of an IMAGE extension: the findings of each unit, by index.
std::map<std::size_t, std::vector<diagnostic>>
repeated_names(const std::vector<std::reference_wrapper<const hdu_info>>& units)
{
    std::map<std::size_t, std::vector<diagnostic>> found;
    std::map<std::tuple<std::string, std::string, std::int64_t, std::int64_t>, std::size_t> first;
    for (const hdu_info& unit : units) {
        const header& cards = unit.header();
        const std::optional<std::string> name = upper_string(cards, "EXTNAME");
        if (!name) {
            continue;
        }
        const std::string type =
            unit.index() == 0 ? std::string("IMAGE") : upper_string(cards, "XTENSION").value_or(std::string());
        const auto [place, added] = first.try_emplace(
            {type, *name, integer_or_one(cards, "EXTVER"), integer_or_one(cards, "EXTLEVEL")}, unit.index());
        if (!added) {
            found[unit.index()].push_back({.severity = severity::warning,
                                           .code = errc::repeated_hdu_name,
                                           .section = "4.4.2.6",
                                           .message = "the unit has the type, EXTNAME, EXTVER and EXTLEVEL of unit " +
                                                      std::to_string(place->second) +
                                                      ", which a lookup by name finds first",
                                           .context = {.hdu = unit.index(), .keyword = "EXTNAME"}});
        }
    }
    return found;
}

// A finding of the open of a file, from its exception.
diagnostic open_finding(const error& failure, std::string_view section)
{
    return {.severity = severity::error,
            .code = failure.code(),
            .section = std::string(section),
            .message = message_of(failure),
            .context = failure.context()};
}

} // namespace

std::vector<diagnostic> verify(const reader& file, const verify_options& options)
{
    const detail::thread_safe_source& source = detail::reader_access::source(file);
    const std::vector<std::reference_wrapper<const hdu_info>> units = file.hdus();
    const std::vector<diagnostic> walk = file.diagnostics();
    std::map<std::size_t, std::vector<diagnostic>> names = repeated_names(units);
    const verification_progress progress(options.progress, file.size());
    progress.reach(0);
    report out;
    for (const hdu_info& unit : units) {
        for (const diagnostic& entry : unit.diagnostics()) {
            out.add(entry);
        }
        for (const diagnostic& entry : walk) {
            if (entry.context.hdu == unit.index()) {
                out.add(entry);
            }
        }
        for (diagnostic& entry : names[unit.index()]) {
            out.add(std::move(entry));
        }
        check_description(file, unit, out);
        check_quantization(unit, out);
        check_fill(source, unit, out);
        check_table_data(source, unit, out, progress);
        check_checksums(file, unit, out, progress);
        progress.reach(unit.data_offset() + detail::data_records(unit.data_size().value_or(0)));
    }
    for (const diagnostic& entry : walk) {
        if (entry.context.hdu.value_or(units.size()) >= units.size()) {
            out.add(entry);
        }
    }
    progress.reach(file.size());
    return out.take();
}

std::vector<diagnostic> verify(std::string_view path, const read_options& open, const verify_options& options)
{
    read_options lenient = open;
    lenient.strict = false;
    std::optional<reader> file;
    try {
        file.emplace(path, lenient);
    } catch (const invalid_data_error& failure) {
        return {open_finding(failure, failure.code() == errc::not_a_fits_file ? "4.4.1.1" : "")};
    } catch (const limit_error& failure) {
        return {open_finding(failure, "")};
    } catch (const integrity_error& failure) {
        return {open_finding(failure, "")};
    }
    return verify(*file, options);
}

} // namespace openfits
