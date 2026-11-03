// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "table/table_description.h"

#include <openfits/error.h>

#include "core/ascii.h"
#include "core/quote.h"
#include "data/image_description.h"
#include "header/keywords.h"
#include "header/long_string.h"
#include "header/record.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace openfits::detail {

namespace {

constexpr std::int64_t max_fields = 999;

std::string indexed(std::string_view root, std::uint64_t n)
{
    return std::string(root) + std::to_string(n);
}

// The checks of the keywords of a table or of random groups, which record each problem in the log.
class keyword_checks
{
public:
    keyword_checks(const header& cards, std::uint64_t index, std::uint64_t header_offset, diagnostic_log& log) noexcept
        : cards_(cards), index_(index), header_offset_(header_offset), log_(log)
    {}

protected:
    [[nodiscard]] error_context at(std::size_t position) const
    {
        return {.hdu = index_,
                .keyword = std::string(cards_[position].keyword()),
                .offset = header_offset_ + (position * record_length)};
    }

    [[nodiscard]] error_context about(std::string_view keyword, std::optional<std::uint64_t> column = {}) const
    {
        return {.hdu = index_, .keyword = std::string(keyword), .column = column, .offset = header_offset_};
    }

    [[nodiscard]] error_context at(std::size_t position, std::uint64_t column) const
    {
        error_context context = at(position);
        context.column = column;
        return context;
    }

    // The integer of a mandatory keyword of every extension, which the walk checked first: nothing when it has none.
    [[nodiscard]] std::optional<std::int64_t> checked_integer(std::string_view keyword) const
    {
        try {
            return cards_.find<std::int64_t>(keyword);
        } catch (const invalid_data_error&) {
            return std::nullopt;
        }
    }

    // The value of the keyword as a string, joined with its CONTINUE records, or nothing when it holds no string; a
    // warning when it holds another value.
    std::optional<std::string> string_of(std::string_view keyword, std::optional<std::uint64_t> column)
    {
        const std::optional<std::size_t> position = cards_.index_of(keyword);
        if (!position || !cards_[*position].has_value_indicator()) {
            return std::nullopt;
        }
        const value_kind kind = cards_[*position].value().kind();
        if (kind == value_kind::string) {
            return joined_string(cards_.cards(), *position);
        }
        if (kind != value_kind::undefined) {
            log_.warning(errc::reserved_keyword,
                         quote(keyword) + " does not hold a string, as the standard requires, and is ignored",
                         with_column(at(*position), column));
        }
        return std::nullopt;
    }

    // The value of the keyword as a number, or nothing when it holds none; a warning when it holds another value.
    std::optional<double> number_of(std::string_view keyword, std::optional<std::uint64_t> column)
    {
        const std::optional<std::size_t> position = cards_.index_of(keyword);
        if (!position || !cards_[*position].has_value_indicator()) {
            return std::nullopt;
        }
        const card_value& value = cards_[*position].value();
        if (value.kind() == value_kind::integer || value.kind() == value_kind::real) {
            return cards_.value<double>(keyword);
        }
        if (value.kind() != value_kind::undefined) {
            log_.warning(errc::reserved_keyword,
                         quote(keyword) + " does not hold a number, as the standard requires, and is ignored",
                         with_column(at(*position), column));
        }
        return std::nullopt;
    }

    static error_context with_column(error_context context, std::optional<std::uint64_t> column)
    {
        context.column = column;
        return context;
    }

    [[nodiscard]] const header& header_cards() const noexcept
    {
        return cards_;
    }

    [[nodiscard]] diagnostic_log& diagnostics() const noexcept
    {
        return log_;
    }

    [[nodiscard]] std::uint64_t unit_index() const noexcept
    {
        return index_;
    }

private:
    const header& cards_;
    std::uint64_t index_;
    std::uint64_t header_offset_;
    diagnostic_log& log_;
};

class table_analysis : keyword_checks
{
public:
    table_analysis(const header& cards, hdu_kind kind, std::uint64_t index, std::uint64_t header_offset,
                   diagnostic_log& log) noexcept
        : keyword_checks(cards, index, header_offset, log), ascii_(kind == hdu_kind::table)
    {}

    std::optional<table_info> run()
    {
        table_info table;
        table.kind = ascii_ ? hdu_kind::table : hdu_kind::bintable;
        const std::optional<std::int64_t> bitpix = checked_integer("BITPIX");
        const std::optional<std::int64_t> naxis = checked_integer("NAXIS");
        if (!bitpix || !naxis) {
            return std::nullopt;
        }
        if (*bitpix != 8) {
            diagnostics().error(errc::invalid_structure,
                                "BITPIX of a table is 8 (standard §7.2.1, §7.3.1), and is " + std::to_string(*bitpix) +
                                    " here, which leaves the columns unknown",
                                about("BITPIX"));
            usable_ = false;
        }
        if (*naxis != 2) {
            diagnostics().error(errc::invalid_structure,
                                "NAXIS of a table is 2 (standard §7.2.1, §7.3.1), and is " + std::to_string(*naxis) +
                                    " here, which leaves the columns unknown",
                                about("NAXIS"));
            return std::nullopt;
        }
        const std::optional<std::int64_t> naxis1 = checked_integer("NAXIS1");
        const std::optional<std::int64_t> naxis2 = checked_integer("NAXIS2");
        const std::optional<std::int64_t> pcount = checked_integer("PCOUNT");
        if (!naxis1 || !naxis2 || *naxis1 < 0 || *naxis2 < 0 || (pcount && *pcount < 0)) {
            return std::nullopt;
        }
        table.row_size = static_cast<std::uint64_t>(*naxis1);
        table.rows = static_cast<std::uint64_t>(*naxis2);
        const std::optional<std::uint64_t> fields = read_tfields();
        if (!fields) {
            return std::nullopt;
        }
        if (!ascii_ && !locate_heap(table, static_cast<std::uint64_t>(pcount.value_or(0)))) {
            usable_ = false;
        }
        std::uint64_t offset = 0;
        table.columns.reserve(*fields);
        for (std::uint64_t n = 1; n <= *fields; ++n) {
            std::optional<column_info> column = read_column(n, table.row_size);
            if (!column) {
                usable_ = false;
                continue;
            }
            if (!ascii_) {
                column->offset = offset;
                offset = add_field(offset, *column, n);
            }
            table.columns.push_back(std::move(*column));
        }
        check_extra_formats(*fields);
        if (usable_ && !ascii_) {
            check_row_size(offset, table.row_size);
        }
        if (!usable_) {
            return std::nullopt;
        }
        return table;
    }

private:
    // TFIELDS, the eighth keyword (standard §7.2.1, §7.3.1): from 0 to 999.
    std::optional<std::uint64_t> read_tfields()
    {
        const std::optional<std::size_t> position = header_cards().index_of("TFIELDS");
        if (!position) {
            diagnostics().error(
                errc::missing_keyword,
                "the mandatory keyword 'TFIELDS' is missing from the table, which leaves its columns unknown",
                about("TFIELDS"));
            return std::nullopt;
        }
        if (*position != 7) {
            diagnostics().warning(errc::keyword_order,
                                  "the mandatory keyword 'TFIELDS' is record " + std::to_string(*position + 1) +
                                      " of the header, where the standard places it at record 8",
                                  at(*position));
        }
        const card& value = header_cards()[*position];
        std::optional<std::int64_t> fields;
        if (const auto* integer = std::get_if<std::int64_t>(&value.value().data())) {
            fields = *integer;
        } else {
            fields = checked_integer("TFIELDS");
            if (!fields) {
                diagnostics().error(errc::keyword_type, "the mandatory keyword 'TFIELDS' holds no integer",
                                    at(*position));
                return std::nullopt;
            }
            diagnostics().warning(errc::keyword_type,
                                  "the mandatory keyword 'TFIELDS' holds no integer, and is read as " +
                                      std::to_string(*fields),
                                  at(*position));
        }
        if (*fields < 0 || *fields > max_fields) {
            diagnostics().error(errc::invalid_structure,
                                "TFIELDS is " + std::to_string(*fields) +
                                    ", which is not from 0 to 999 (standard §7.2.1, §7.3.1)",
                                at(*position));
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(*fields);
    }

    // The heap of a binary table: THEAP from the end of the rows to the end of the data, NAXIS1 x NAXIS2 without it
    // (standard §7.3.5).
    bool locate_heap(table_info& table, std::uint64_t pcount)
    {
        if (table.rows != 0 && table.row_size > std::numeric_limits<std::uint64_t>::max() / table.rows) {
            return false;
        }
        const std::uint64_t rows = table.row_size * table.rows;
        if (pcount > std::numeric_limits<std::uint64_t>::max() - rows) {
            return false;
        }
        table.heap_offset = rows;
        table.heap_size = pcount;
        const std::optional<std::size_t> position = header_cards().index_of("THEAP");
        if (!position || !header_cards()[*position].has_value_indicator() ||
            header_cards()[*position].value().kind() == value_kind::undefined) {
            return true;
        }
        const std::optional<std::int64_t> theap = checked_integer("THEAP");
        if (!theap || header_cards()[*position].value().kind() != value_kind::integer) {
            diagnostics().warning(errc::reserved_keyword,
                                  "THEAP does not hold an integer, and the heap follows the rows", at(*position));
            return true;
        }
        if (pcount == 0) {
            diagnostics().warning(errc::reserved_keyword,
                                  "THEAP is given with PCOUNT = 0, which the standard forbids (§7.3.2)", at(*position));
        }
        if (std::cmp_less(*theap, rows)) {
            diagnostics().error(errc::invalid_structure,
                                "THEAP is " + std::to_string(*theap) + ", before the end of the rows at byte " +
                                    std::to_string(rows) + " of the data (standard §7.3.5)",
                                at(*position));
            return false;
        }
        const auto heap = static_cast<std::uint64_t>(*theap);
        if (heap - rows > pcount) {
            diagnostics().error(errc::invalid_structure,
                                "THEAP is " + std::to_string(heap) + ", beyond the end of the data at byte " +
                                    std::to_string(rows + pcount) + " (standard §7.3.5)",
                                at(*position));
            return false;
        }
        table.heap_offset = heap;
        table.heap_size = rows + pcount - heap;
        return true;
    }

    // The format of the column n: TFORMn, a string of the grammar of its table; one in lower case is read with a
    // warning.
    std::optional<column_format> read_format(std::uint64_t n)
    {
        const std::string keyword = indexed("TFORM", n);
        const std::optional<std::size_t> position = header_cards().index_of(keyword);
        if (!position) {
            diagnostics().error(errc::missing_keyword,
                                "the mandatory keyword " + quote(keyword) + " is missing from the table",
                                about(keyword, n));
            return std::nullopt;
        }
        if (header_cards()[*position].value().kind() != value_kind::string) {
            diagnostics().error(errc::keyword_type, quote(keyword) + " holds no string", at(*position, n));
            return std::nullopt;
        }
        const std::string text = joined_string(header_cards().cards(), *position);
        const auto parse = [this](std::string_view form) {
            return ascii_ ? column_format::parse_ascii(form) : column_format::parse_binary(form);
        };
        try {
            return parse(text);
        } catch (const invalid_data_error& refusal) {
            std::string upper = text;
            std::ranges::transform(upper, upper.begin(), to_upper);
            if (upper != text) {
                std::optional<column_format> lowered;
                try {
                    lowered = parse(upper);
                } catch (const invalid_data_error&) {
                    // Refused as it is, below.
                    lowered.reset();
                }
                if (lowered) {
                    diagnostics().warning(errc::invalid_column_format,
                                          quote(keyword) + " is " + quote(text) + ", in lower case, which is read as " +
                                              quote(upper),
                                          at(*position, n));
                    return lowered;
                }
            }
            diagnostics().error(refusal.code(), refusal.what(), at(*position, n));
            return std::nullopt;
        }
    }

    std::optional<column_info> read_column(std::uint64_t n, std::uint64_t row_size)
    {
        std::optional<column_format> format = read_format(n);
        if (!format) {
            return std::nullopt;
        }
        column_info column;
        column.format = format;
        column.name = std::string(trim_trailing_spaces(string_of(indexed("TTYPE", n), n).value_or("")));
        column.unit = std::string(trim_trailing_spaces(string_of(indexed("TUNIT", n), n).value_or("")));
        read_scaling(column, *format, n);
        read_null(column, *format, n);
        read_display(column, *format, n);
        if (!ascii_) {
            read_dimensions(column, *format, n);
        } else if (!read_position(column, *format, n, row_size)) {
            return std::nullopt;
        }
        return column;
    }

    // TSCALn and TZEROn, which characters, logicals and bits must not have (standard §7.2.2, §7.3.2).
    void read_scaling(column_info& column, const column_format& format, std::uint64_t n)
    {
        const column_type type = format.type;
        const bool scalable =
            type != column_type::character && type != column_type::logical && type != column_type::bit;
        for (const std::string_view root : {std::string_view("TSCAL"), std::string_view("TZERO")}) {
            const std::string keyword = indexed(root, n);
            const std::optional<double> value = number_of(keyword, n);
            if (!value) {
                continue;
            }
            if (!scalable) {
                diagnostics().warning(errc::reserved_keyword,
                                      quote(keyword) +
                                          " is given for a column of characters, logicals or bits, which the "
                                          "standard forbids, and is ignored",
                                      about(keyword, n));
                continue;
            }
            if (root == "TSCAL") {
                column.scaling.scale = *value;
                if (*value == 0.0) {
                    diagnostics().error(errc::reserved_keyword,
                                        quote(keyword) +
                                            " is 0, which leaves the physical values of the column unavailable",
                                        about(keyword, n));
                }
            } else {
                column.scaling.zero = *value;
            }
        }
    }

    // TNULLn: an integer of the integer columns of a binary table (standard §7.3.2), a string of an ASCII table
    // (standard §7.2.2).
    void read_null(column_info& column, const column_format& format, std::uint64_t n)
    {
        const std::string keyword = indexed("TNULL", n);
        const std::optional<std::size_t> position = header_cards().index_of(keyword);
        if (!position || !header_cards()[*position].has_value_indicator() ||
            header_cards()[*position].value().kind() == value_kind::undefined) {
            return;
        }
        const card& value = header_cards()[*position];
        if (ascii_) {
            if (value.value().kind() == value_kind::string) {
                column.null_string = joined_string(header_cards().cards(), *position);
            } else {
                // A number without quotes, as some writers give it: its text.
                column.null_string = std::string(value.value_text());
                diagnostics().warning(errc::reserved_keyword,
                                      quote(keyword) + " of an ASCII table holds no string, and its text " +
                                          quote(value.value_text()) + " is read as the null string",
                                      at(*position, n));
            }
            return;
        }
        const column_type type = format.type;
        const bool integers = type == column_type::uint8 || type == column_type::int16 || type == column_type::int32 ||
                              type == column_type::int64;
        if (!integers) {
            diagnostics().warning(errc::reserved_keyword,
                                  quote(keyword) + " is given for a column that is not of integers, which the standard "
                                                   "forbids (§7.3.2), and is ignored",
                                  at(*position, n));
            return;
        }
        if (const auto* integer = std::get_if<std::int64_t>(&value.value().data())) {
            column.null = *integer;
            return;
        }
        diagnostics().warning(errc::reserved_keyword,
                              quote(keyword) +
                                  " does not hold an integer of 64 bits, as the standard requires, and is ignored",
                              at(*position, n));
    }

    // TDISPn, a display format that may show the values of the column (standard §7.3.4).
    void read_display(column_info& column, const column_format& format, std::uint64_t n)
    {
        const std::string keyword = indexed("TDISP", n);
        const std::optional<std::string> text = string_of(keyword, n);
        if (!text) {
            return;
        }
        try {
            const display_format display = display_format::parse(*text);
            if (!displays(display, format)) {
                diagnostics().warning(
                    errc::invalid_display_format,
                    quote(keyword) + " is " + quote(*text) +
                        ", which cannot display the values of the column (standard §7.3.4), and is ignored",
                    about(keyword, n));
                return;
            }
            column.display = display;
        } catch (const invalid_data_error& refusal) {
            diagnostics().warning(errc::invalid_display_format, std::string(refusal.what()) + "; it is ignored",
                                  about(keyword, n));
        }
    }

    // TDIMn of a binary table (standard §7.3.2), and the substrings of the rAw convention without it.
    void read_dimensions(column_info& column, const column_format& format, std::uint64_t n)
    {
        const std::string keyword = indexed("TDIM", n);
        if (const std::optional<std::string> text = string_of(keyword, n)) {
            const std::optional<std::vector<std::uint64_t>> dimensions = parse_dimensions(*text);
            if (!dimensions) {
                diagnostics().warning(errc::invalid_dimensions,
                                      quote(keyword) + " is " + quote(*text) +
                                          ", which is not of the form (l,m,...) of standard §7.3.2, and is ignored",
                                      about(keyword, n));
            } else if (format.storage == column_storage::fixed && elements(*dimensions) > format.repeat) {
                diagnostics().warning(errc::invalid_dimensions,
                                      quote(keyword) + " is " + quote(*text) + ", an array larger than the " +
                                          std::to_string(format.repeat) +
                                          " elements of the field (standard §7.3.2), and is ignored",
                                      about(keyword, n));
            } else {
                column.dimensions = *dimensions;
                return;
            }
        }
        if (format.type == column_type::character && format.substring_width) {
            column.dimensions = {*format.substring_width, format.repeat / *format.substring_width};
        }
    }

    // TBCOLn of an ASCII table: the field from that character within the row (standard §7.2.1).
    bool read_position(column_info& column, const column_format& format, std::uint64_t n, std::uint64_t row_size)
    {
        const std::string keyword = indexed("TBCOL", n);
        const std::optional<std::size_t> position = header_cards().index_of(keyword);
        if (!position) {
            diagnostics().error(errc::missing_keyword,
                                "the mandatory keyword " + quote(keyword) + " is missing from the ASCII table",
                                about(keyword, n));
            return false;
        }
        const std::optional<std::int64_t> start = checked_integer(keyword);
        if (!start || header_cards()[*position].value().kind() != value_kind::integer) {
            diagnostics().error(errc::keyword_type, quote(keyword) + " holds no integer", at(*position, n));
            return false;
        }
        const std::uint64_t width = format.field_size();
        if (*start < 1 || static_cast<std::uint64_t>(*start) - 1 > row_size ||
            width > row_size - (static_cast<std::uint64_t>(*start) - 1)) {
            diagnostics().error(errc::invalid_structure,
                                quote(keyword) + " is " + std::to_string(*start) + ", and the field of " +
                                    std::to_string(width) + " characters from it does not lie within the " +
                                    std::to_string(row_size) + " characters of a row (standard §7.2.1)",
                                at(*position, n));
            return false;
        }
        column.offset = static_cast<std::uint64_t>(*start) - 1;
        return true;
    }

    std::uint64_t add_field(std::uint64_t offset, const column_info& column, std::uint64_t n)
    {
        if (!column.format) {
            return offset;
        }
        const column_format& format = *column.format;
        std::uint64_t size = 0;
        try {
            size = format.field_size();
        } catch (const usage_error&) {
            size = std::numeric_limits<std::uint64_t>::max();
        }
        if (size > std::numeric_limits<std::uint64_t>::max() - offset) {
            diagnostics().error(errc::invalid_column_format,
                                "the fields of the columns up to " + std::to_string(n) + " do not fit in 64 bits",
                                about(indexed("TFORM", n), n));
            usable_ = false;
            return offset;
        }
        return offset + size;
    }

    // NAXIS1, which equals the bytes of the fields (standard §7.3.1, Eq. 8): fields beyond the row leave them unknown,
    // and bytes after the last field are read as unused.
    void check_row_size(std::uint64_t fields, std::uint64_t row_size)
    {
        if (fields > row_size) {
            diagnostics().error(errc::invalid_column_format,
                                "the fields of the columns take " + std::to_string(fields) +
                                    " bytes, beyond NAXIS1 = " + std::to_string(row_size) + " (standard §7.3.1, Eq. 8)",
                                about("NAXIS1"));
            usable_ = false;
        } else if (fields < row_size) {
            diagnostics().warning(errc::invalid_column_format,
                                  "the fields of the columns take " + std::to_string(fields) +
                                      " bytes, fewer than NAXIS1 = " + std::to_string(row_size) +
                                      " (standard §7.3.1, Eq. 8); the rest of each row is unused",
                                  about("NAXIS1"));
        }
    }

    // TFORMn for an n beyond TFIELDS, which the standard forbids (§7.2.1, §7.3.1).
    void check_extra_formats(std::uint64_t fields)
    {
        for (const std::size_t i : header_cards().matching("TFORM#")) {
            const std::optional<std::uint32_t> n = keyword_index(header_cards()[i].keyword(), "TFORM");
            if (n && *n > fields && header_cards()[i].has_value_indicator()) {
                diagnostics().warning(errc::invalid_structure,
                                      "TFIELDS is " + std::to_string(fields) + ", and " +
                                          quote(header_cards()[i].keyword()) + " is ignored",
                                      at(i));
            }
        }
    }

    [[nodiscard]] static std::uint64_t elements(const std::vector<std::uint64_t>& dimensions) noexcept
    {
        std::uint64_t product = 1;
        for (const std::uint64_t length : dimensions) {
            if (length != 0 && product > std::numeric_limits<std::uint64_t>::max() / length) {
                return std::numeric_limits<std::uint64_t>::max();
            }
            product *= length;
        }
        return product;
    }

    bool ascii_;
    bool usable_ = true;
};

class groups_analysis : keyword_checks
{
public:
    using keyword_checks::keyword_checks;

    // The parameters are walked over the cards that give them, never over PCOUNT, which an empty file can make
    // anything.
    std::optional<groups_info> run(bool build)
    {
        const std::optional<std::int64_t> bitpix = checked_integer("BITPIX");
        const std::optional<std::int64_t> naxis = checked_integer("NAXIS");
        const std::optional<std::int64_t> pcount = checked_integer("PCOUNT");
        const std::optional<std::int64_t> gcount = checked_integer("GCOUNT");
        if (!bitpix || !naxis || !pcount || !gcount || *naxis < 1 || *pcount < 0 || *gcount < 0) {
            return std::nullopt;
        }
        for (const std::string_view root : {std::string_view("PSCAL#"), std::string_view("PZERO#")}) {
            for (const std::size_t i : header_cards().matching(root)) {
                check_parameter_scaling(i, static_cast<std::uint64_t>(*pcount));
            }
        }
        if (!build) {
            return std::nullopt;
        }
        groups_info groups;
        groups.count = static_cast<std::uint64_t>(*gcount);
        groups.parameters.resize(static_cast<std::size_t>(*pcount));
        for (std::size_t i = 0; i < header_cards().size(); ++i) {
            const std::string_view keyword = header_cards()[i].keyword();
            for (const std::string_view root :
                 {std::string_view("PTYPE"), std::string_view("PSCAL"), std::string_view("PZERO")}) {
                const std::optional<std::uint32_t> n = keyword_index(keyword, root);
                if (!n || *n > groups.parameters.size() || header_cards().index_of(keyword) != i) {
                    continue;
                }
                group_parameter& parameter = groups.parameters[*n - 1];
                if (root == "PTYPE") {
                    parameter.name = std::string(trim_trailing_spaces(string_of(keyword, std::nullopt).value_or("")));
                } else if (const std::optional<double> value = number_of(keyword, std::nullopt)) {
                    (root == "PSCAL" ? parameter.scaling.scale : parameter.scaling.zero) = *value;
                }
            }
        }
        return groups;
    }

private:
    void check_parameter_scaling(std::size_t position, std::uint64_t pcount)
    {
        const std::string_view keyword = header_cards()[position].keyword();
        const std::optional<std::uint32_t> n = keyword_index(keyword, "PSCAL");
        if (!n || *n > pcount || header_cards().index_of(keyword) != position) {
            return;
        }
        const card_value& value = header_cards()[position].value();
        const bool zero = (value.kind() == value_kind::real && value.get<double>() == 0.0) ||
                          (std::holds_alternative<std::int64_t>(value.data()) && value.get<std::int64_t>() == 0);
        if (zero) {
            diagnostics().error(errc::reserved_keyword,
                                quote(keyword) + " is 0, which leaves the physical values of the parameter unavailable",
                                at(position));
        }
    }
};

[[noreturn]] void throw_first_error(const diagnostic_log& log, const hdu_info& unit)
{
    for (const diagnostic& found : log.entries()) {
        if (found.severity == severity::error) {
            throw invalid_data_error(found.code, found.message, found.context);
        }
    }
    throw_unavailable(unit);
}

} // namespace

std::optional<table_info> analyze_table(const header& cards, hdu_kind kind, std::uint64_t index,
                                        std::uint64_t header_offset, diagnostic_log& log)
{
    return table_analysis(cards, kind, index, header_offset, log).run();
}

std::optional<groups_info> analyze_groups(const header& cards, std::uint64_t header_offset, diagnostic_log& log)
{
    return groups_analysis(cards, 0, header_offset, log).run(false);
}

table_info describe_table(const hdu_info& unit, bool with_header)
{
    const error_context where{.hdu = unit.index()};
    switch (unit.kind()) {
    case hdu_kind::table:
    case hdu_kind::bintable:
    case hdu_kind::compressed_image:
    case hdu_kind::compressed_table:
        break;
    case hdu_kind::primary:
    case hdu_kind::image:
    case hdu_kind::random_groups:
    case hdu_kind::unknown:
        throw usage_error(errc::not_a_table, "unit " + std::to_string(unit.index()) + " holds no table", where);
    }
    if (!unit.data_size()) {
        throw_unavailable(unit);
    }
    diagnostic_log log(false, 0);
    std::optional<table_info> table =
        analyze_table(unit.header(), unit.kind(), unit.index(), unit.header_offset(), log);
    if (!table) {
        throw_first_error(log, unit);
    }
    if (with_header) {
        table->header = unit.header();
    }
    return std::move(*table);
}

groups_info describe_groups(const hdu_info& unit, const limits& limits, bool with_header)
{
    if (unit.kind() != hdu_kind::random_groups) {
        throw usage_error(errc::not_random_groups, "unit " + std::to_string(unit.index()) + " holds no random groups",
                          {.hdu = unit.index()});
    }
    if (!unit.data_size()) {
        throw_unavailable(unit);
    }
    const header& cards = unit.header();
    // The walk checked PCOUNT, and a unit without groups, or a truncated one, holds no data that bound it.
    const std::optional<std::int64_t> pcount = cards.find<std::int64_t>("PCOUNT");
    const std::uint64_t most = limits.max_allocation;
    if (pcount && *pcount > 0 && most != 0 && static_cast<std::uint64_t>(*pcount) > most / sizeof(group_parameter)) {
        throw limit_error(errc::allocation_too_large,
                          "the " + std::to_string(*pcount) + " parameters of the groups of unit " +
                              std::to_string(unit.index()) + " take more than limits::max_allocation, " +
                              std::to_string(most) + " bytes",
                          {.hdu = unit.index()});
    }
    // The arrays are described as images are, from NAXIS2 on.
    const image_info arrays = describe_array(unit, false);
    diagnostic_log log(false, 0);
    std::optional<groups_info> groups = groups_analysis(cards, 0, unit.header_offset(), log).run(true);
    if (!groups) {
        throw_first_error(log, unit);
    }
    if (arrays.bitpix) {
        groups->bitpix = *arrays.bitpix;
    }
    groups->geometry.axes.assign(arrays.geometry.axes.begin() + 1, arrays.geometry.axes.end());
    groups->scaling = arrays.scaling;
    groups->blank = arrays.blank;
    if (with_header) {
        groups->header = cards;
    }
    return std::move(*groups);
}

std::optional<std::vector<std::uint64_t>> parse_dimensions(std::string_view text)
{
    const std::string_view form = trim_spaces(text);
    if (form.size() < 2 || form.front() != '(' || form.back() != ')') {
        return std::nullopt;
    }
    std::vector<std::uint64_t> dimensions;
    std::string_view rest = form.substr(1, form.size() - 2);
    while (true) {
        const std::size_t comma = rest.find(',');
        const std::string_view item = trim_spaces(rest.substr(0, comma));
        if (item.empty() || item.size() > 19 || !std::ranges::all_of(item, is_digit)) {
            return std::nullopt;
        }
        std::uint64_t length = 0;
        for (const char c : item) {
            length = (length * 10) + static_cast<std::uint64_t>(c - '0');
        }
        if (length == 0) {
            return std::nullopt;
        }
        dimensions.push_back(length);
        if (comma == std::string_view::npos) {
            break;
        }
        rest = rest.substr(comma + 1);
    }
    return dimensions;
}

std::string format_dimensions(const std::vector<std::uint64_t>& dimensions)
{
    std::string text = "(";
    for (std::size_t i = 0; i < dimensions.size(); ++i) {
        text += (i == 0 ? "" : ",") + std::to_string(dimensions[i]);
    }
    return text + ")";
}

bool displays(const display_format& display, const column_format& format) noexcept
{
    const display_code code = display.code;
    switch (format.type) {
    case column_type::character:
        return code == display_code::character || code == display_code::general;
    case column_type::logical:
        return code == display_code::logical || code == display_code::general;
    case column_type::bit:
    case column_type::uint8:
    case column_type::int16:
    case column_type::int32:
    case column_type::int64:
        return code != display_code::character && code != display_code::logical;
    case column_type::float32:
    case column_type::float64:
    case column_type::complex_float32:
    case column_type::complex_float64:
        break;
    }
    return code == display_code::fixed || code == display_code::exponential || code == display_code::engineering ||
           code == display_code::scientific || code == display_code::general ||
           code == display_code::double_exponential;
}

} // namespace openfits::detail
