// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "hdu/structure.h"

#include <openfits/card.h>
#include <openfits/error.h>

#include "compress/image_tiles.h"
#include "core/ascii.h"
#include "core/quote.h"
#include "header/keywords.h"
#include "header/long_string.h"
#include "header/record.h"
#include "table/table_description.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <vector>

namespace openfits::detail {

namespace {

constexpr std::int64_t max_naxis = 999;
constexpr auto bitpix_values = std::to_array<std::int64_t>({8, 16, 32, 64, -32, -64});

std::optional<std::uint64_t> product(std::uint64_t a, std::uint64_t b) noexcept
{
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
        return std::nullopt;
    }
    return a * b;
}

// The axes of the data, read from NAXIS and NAXISn.
struct axes_read
{
    // NAXIS, when it could be read.
    std::optional<std::size_t> naxis{};
    // Every NAXISn, when each could be read.
    std::optional<std::vector<std::uint64_t>> lengths{};
};

class analysis
{
public:
    analysis(const header& cards, std::uint64_t index, std::uint64_t header_offset, diagnostic_log& log) noexcept
        : cards_(cards), index_(index), header_offset_(header_offset), log_(log)
    {}

    unit_structure run()
    {
        unit_structure result;
        result.kind = index_ == 0 ? check_simple() : extension_kind();
        kind_ = result.kind;
        const std::optional<std::int64_t> bitpix = read_bitpix();
        const axes_read axes = read_axes();
        const std::size_t naxis = axes.naxis.value_or(0);
        const bool groups = index_ == 0 && is_random_groups(axes);
        if (groups) {
            result.kind = hdu_kind::random_groups;
            kind_ = result.kind;
        }

        std::optional<std::uint64_t> pcount = 0;
        std::optional<std::uint64_t> gcount = 1;
        if (index_ > 0 || groups) {
            // The standard places PCOUNT and GCOUNT right after the last NAXISn in an extension; random groups give
            // them no place.
            const bool image = result.kind == hdu_kind::image;
            pcount = read_count("PCOUNT", groups ? std::nullopt : std::optional(3 + naxis), 0, image);
            gcount = read_count("GCOUNT", groups ? std::nullopt : std::optional(4 + naxis), 1, image);
            check_counts(result.kind, pcount, gcount);
        }
        if (result.kind == hdu_kind::bintable) {
            result.kind = compressed_kind();
        }
        if (bitpix && axes.lengths && pcount && gcount) {
            result.data_size = data_size(*bitpix, *axes.lengths, groups, *pcount, *gcount);
        }
        read_name(result);
        check_reserved(result.kind, bitpix);
        check_places(result.kind);
        check_contents(result.kind);
        return result;
    }

private:
    [[nodiscard]] error_context at(std::size_t position) const
    {
        return {.hdu = index_,
                .keyword = std::string(cards_[position].keyword()),
                .offset = header_offset_ + (position * record_length)};
    }

    [[nodiscard]] error_context about(std::string_view keyword) const
    {
        return {.hdu = index_, .keyword = std::string(keyword), .offset = header_offset_};
    }

    [[nodiscard]] std::string name_at(std::size_t position) const
    {
        return quote(cards_[position].keyword());
    }

    // The section of the standard that gives the mandatory keywords of the unit, as far as its kind is known.
    [[nodiscard]] std::string_view mandatory_section() const noexcept
    {
        switch (kind_) {
        case hdu_kind::primary:
            return "4.4.1.1";
        case hdu_kind::random_groups:
            return "6.1.1";
        case hdu_kind::image:
            return "7.1.1";
        case hdu_kind::table:
            return "7.2.1";
        case hdu_kind::bintable:
        case hdu_kind::compressed_image:
        case hdu_kind::compressed_table:
            return "7.3.1";
        case hdu_kind::unknown:
            break;
        }
        return "4.4.1.2";
    }

    // The place and the format of a mandatory keyword, at the record expected when the standard gives it one.
    void place(std::size_t position, std::optional<std::size_t> expected)
    {
        if (expected && position != *expected) {
            log_.warning(errc::keyword_order, mandatory_section(),
                         "the mandatory keyword " + name_at(position) + " is record " + std::to_string(position + 1) +
                             " of the header, where the standard places it at record " + std::to_string(*expected + 1),
                         at(position));
        }
        const value_kind kind = cards_[position].value().kind();
        if (kind != value_kind::none && kind != value_kind::undefined && !card_access::fixed_format(cards_[position])) {
            log_.info(errc::free_format, "4.4.1",
                      "the value of the mandatory keyword " + name_at(position) +
                          " is not in the fixed format, which the standard requires of mandatory keywords",
                      at(position));
        }
    }

    // The first card of a mandatory keyword, or nothing when the header lacks it, an error.
    std::optional<std::size_t> mandatory(std::string_view keyword, std::optional<std::size_t> expected)
    {
        const std::optional<std::size_t> position = cards_.index_of(keyword);
        if (!position) {
            log_.error(errc::missing_keyword, mandatory_section(),
                       "the mandatory keyword " + quote(keyword) + " is missing", about(keyword));
            return std::nullopt;
        }
        place(*position, expected);
        return position;
    }

    // The integer of a mandatory keyword, or nothing when it holds none, an error. A value that converts to one, a
    // string or an integral real, is read with a warning.
    std::optional<std::int64_t> integer_at(std::size_t position)
    {
        const card& value = cards_[position];
        if (const auto* integer = std::get_if<std::int64_t>(&value.value().data())) {
            return *integer;
        }
        std::optional<std::int64_t> converted;
        try {
            converted = cards_.value<std::int64_t>(value.keyword());
        } catch (const invalid_data_error&) {
            converted = std::nullopt;
        }
        if (!converted) {
            log_.error(errc::keyword_type, mandatory_section(),
                       "the mandatory keyword " + name_at(position) + " holds no integer", at(position));
            return std::nullopt;
        }
        log_.warning(errc::keyword_type, mandatory_section(),
                     "the mandatory keyword " + name_at(position) + " holds no integer, and is read as " +
                         std::to_string(*converted),
                     at(position));
        return converted;
    }

    // SIMPLE, the first keyword, which read_header() found: a file that does not conform to the standard, SIMPLE = F,
    // is read as one that does, with a warning.
    hdu_kind check_simple()
    {
        place(0, std::size_t{0});
        const card_value& value = cards_[0].value();
        if (value.kind() == value_kind::logical) {
            if (!value.get<bool>()) {
                log_.warning(errc::invalid_structure, "4.4.1.1",
                             "SIMPLE is F, which says that the file does not conform to the standard; it is read as "
                             "one that does",
                             at(0));
            }
        } else if (value.kind() == value_kind::string &&
                   (value.get<std::string>() == "T" || value.get<std::string>() == "F")) {
            log_.warning(
                errc::keyword_type, "4.4.1.1",
                "SIMPLE holds the string " + quote(value.get<std::string>()) + ", which is read as the logical", at(0));
        } else {
            log_.warning(errc::keyword_type, "4.4.1.1", "SIMPLE holds no logical; the file is read as FITS", at(0));
        }
        return hdu_kind::primary;
    }

    // The kind of an extension from XTENSION, the first keyword, which read_header() found.
    hdu_kind extension_kind()
    {
        place(0, std::size_t{0});
        const card& xtension = cards_[0];
        if (xtension.value().kind() != value_kind::string) {
            log_.warning(errc::keyword_type, "4.4.1.2",
                         "XTENSION holds no string; the unit is read as an extension of unknown type", at(0));
            return hdu_kind::unknown;
        }
        const auto& type = xtension.value().get<std::string>();
        std::string upper = type;
        std::ranges::transform(upper, upper.begin(), to_upper);
        if (upper != type) {
            log_.warning(errc::invalid_structure, "4.4.1.2",
                         "XTENSION " + quote(type) + " is not in upper case, and is read as " + quote(upper), at(0));
        }
        // The value of XTENSION is padded to eight characters, its closing quote in byte 20 (standard §4.2.1.1).
        if (!xtension.value_text().empty() && xtension.value_text().size() < 10) {
            log_.warning(errc::invalid_structure, "4.2.1.1",
                         "XTENSION " + quote(type) + " is not padded with spaces to eight characters", at(0));
        }
        if (upper == "IMAGE") {
            return hdu_kind::image;
        }
        if (upper == "TABLE") {
            return hdu_kind::table;
        }
        if (upper == "BINTABLE") {
            return hdu_kind::bintable;
        }
        return hdu_kind::unknown;
    }

    std::optional<std::int64_t> read_bitpix()
    {
        const std::optional<std::size_t> position = mandatory("BITPIX", std::size_t{1});
        if (!position) {
            return std::nullopt;
        }
        const std::optional<std::int64_t> bitpix = integer_at(*position);
        if (bitpix && std::ranges::find(bitpix_values, *bitpix) == bitpix_values.end()) {
            log_.error(errc::invalid_structure, "4.4.1.1",
                       "BITPIX is " + std::to_string(*bitpix) + ", which is none of 8, 16, 32, 64, -32 and -64",
                       at(*position));
            return std::nullopt;
        }
        return bitpix;
    }

    axes_read read_axes()
    {
        axes_read result;
        const std::optional<std::size_t> position = mandatory("NAXIS", std::size_t{2});
        if (!position) {
            return result;
        }
        const std::optional<std::int64_t> naxis = integer_at(*position);
        if (!naxis) {
            return result;
        }
        if (*naxis < 0 || *naxis > max_naxis) {
            log_.error(errc::invalid_structure, "4.4.1.1",
                       "NAXIS is " + std::to_string(*naxis) + ", which is not from 0 to 999", at(*position));
            return result;
        }
        result.naxis = static_cast<std::size_t>(*naxis);
        std::vector<std::uint64_t> lengths;
        bool complete = true;
        for (std::size_t n = 1; n <= *result.naxis; ++n) {
            const std::optional<std::size_t> axis = mandatory("NAXIS" + std::to_string(n), 2 + n);
            const std::optional<std::int64_t> length = axis ? integer_at(*axis) : std::nullopt;
            if (!length) {
                complete = false;
            } else if (*length < 0) {
                log_.error(errc::invalid_structure, "4.4.1.1",
                           name_at(*axis) + " is " + std::to_string(*length) + ", which is negative", at(*axis));
                complete = false;
            } else {
                lengths.push_back(static_cast<std::uint64_t>(*length));
            }
        }
        // NAXISn beyond NAXIS, the index of one to three digits that keyword_index() reads, without the match of a
        // pattern for each card.
        for (std::size_t i = 0; i < cards_.size(); ++i) {
            const std::string_view keyword = cards_[i].keyword();
            if (keyword.size() < 6 || keyword.size() > 8 || !keyword.starts_with("NAXIS")) {
                continue;
            }
            const std::optional<std::uint32_t> n = keyword_index(keyword, "NAXIS");
            if (n && *n > *result.naxis && cards_[i].has_value_indicator()) {
                log_.warning(errc::invalid_structure, "4.4.1.1",
                             "NAXIS is " + std::to_string(*result.naxis) + ", and " + name_at(i) + " is ignored",
                             at(i));
            }
        }
        if (complete) {
            result.lengths = std::move(lengths);
        }
        return result;
    }

    // Random groups: GROUPS = T in a primary header whose NAXIS1 is 0 (standard §6.1.1).
    [[nodiscard]] bool is_random_groups(const axes_read& axes) const
    {
        if (!axes.lengths || axes.lengths->empty() || axes.lengths->front() != 0) {
            return false;
        }
        const std::optional<std::size_t> position = cards_.index_of("GROUPS");
        if (!position) {
            return false;
        }
        const card_value& value = cards_[*position].value();
        return value.kind() == value_kind::logical && value.get<bool>();
    }

    // PCOUNT or GCOUNT, which an IMAGE extension may lack, read as fallback with a warning.
    std::optional<std::uint64_t> read_count(std::string_view keyword, std::optional<std::size_t> expected,
                                            std::uint64_t fallback, bool optional)
    {
        if (optional && !cards_.contains(keyword)) {
            log_.warning(errc::missing_keyword, "7.1.1",
                         "the mandatory keyword " + quote(keyword) + " is missing from the IMAGE extension, and is " +
                             std::to_string(fallback),
                         about(keyword));
            return fallback;
        }
        const std::optional<std::size_t> position = mandatory(keyword, expected);
        if (!position) {
            return std::nullopt;
        }
        const std::optional<std::int64_t> count = integer_at(*position);
        if (!count) {
            return std::nullopt;
        }
        if (*count < 0) {
            log_.error(errc::invalid_structure, mandatory_section(),
                       name_at(*position) + " is " + std::to_string(*count) + ", which is negative", at(*position));
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(*count);
    }

    // The values that IMAGE and TABLE extensions must have, PCOUNT = 0 and GCOUNT = 1, and binary tables GCOUNT = 1
    // (standard §7.1.1, §7.2.1, §7.3.1); the data are sized with the values the header gives.
    void check_counts(hdu_kind kind, std::optional<std::uint64_t> pcount, std::optional<std::uint64_t> gcount)
    {
        const bool array_like = kind == hdu_kind::image || kind == hdu_kind::table;
        if (array_like && pcount && *pcount != 0) {
            log_.warning(errc::invalid_structure, mandatory_section(),
                         "PCOUNT is " + std::to_string(*pcount) + ", where the standard requires 0 of this extension",
                         about("PCOUNT"));
        }
        if ((array_like || kind == hdu_kind::bintable) && gcount && *gcount != 1) {
            log_.warning(errc::invalid_structure, mandatory_section(),
                         "GCOUNT is " + std::to_string(*gcount) + ", where the standard requires 1 of this extension",
                         about("GCOUNT"));
        }
    }

    // A binary table that holds a compressed image, ZIMAGE = T, or a compressed table, ZTABLE = T (standard §10).
    [[nodiscard]] hdu_kind compressed_kind() const
    {
        const auto is_true = [this](std::string_view keyword) {
            const std::optional<std::size_t> position = cards_.index_of(keyword);
            return position && cards_[*position].value().kind() == value_kind::logical &&
                   cards_[*position].value().get<bool>();
        };
        if (is_true("ZIMAGE")) {
            return hdu_kind::compressed_image;
        }
        if (is_true("ZTABLE")) {
            return hdu_kind::compressed_table;
        }
        return hdu_kind::bintable;
    }

    // |BITPIX| x GCOUNT x (PCOUNT + NAXIS1 x ... x NAXISm) bits (standard Eq. 1, 2 and 4): no array without axes, and
    // for random groups the product from NAXIS2, which is 1 when there is no other axis.
    std::optional<std::uint64_t> data_size(std::int64_t bitpix, const std::vector<std::uint64_t>& lengths, bool groups,
                                           std::uint64_t pcount, std::uint64_t gcount)
    {
        std::optional<std::uint64_t> elements = groups || !lengths.empty() ? 1 : 0;
        for (std::size_t i = groups ? 1 : 0; i < lengths.size() && elements; ++i) {
            elements = product(*elements, lengths[i]);
        }
        if (elements && pcount <= std::numeric_limits<std::uint64_t>::max() - *elements) {
            const auto bytes = static_cast<std::uint64_t>(bitpix < 0 ? -bitpix : bitpix) / 8;
            elements = product(*elements + pcount, gcount);
            if (elements) {
                elements = product(*elements, bytes);
            }
            if (elements) {
                return elements;
            }
        }
        log_.error(errc::invalid_structure, "", "the size of the data that the header gives does not fit in 64 bits",
                   about("NAXIS"));
        return std::nullopt;
    }

    // The keywords of the columns of tables (standard §7.2, §7.3) and of the parameters of random groups (standard §6).
    void check_contents(hdu_kind kind)
    {
        switch (kind) {
        case hdu_kind::table:
        case hdu_kind::bintable:
        case hdu_kind::compressed_image:
            (void)analyze_table(cards_, kind, index_, header_offset_, log_);
            analyze_compressed_image(cards_, index_, header_offset_, log_);
            break;
        case hdu_kind::compressed_table:
            (void)analyze_table(cards_, kind, index_, header_offset_, log_);
            break;
        case hdu_kind::random_groups:
            (void)analyze_groups(cards_, header_offset_, log_);
            break;
        case hdu_kind::primary:
        case hdu_kind::image:
        case hdu_kind::unknown:
            break;
        }
    }

    // EXTNAME, or else HDUNAME, and EXTVER (standard §4.4.2.6).
    void read_name(unit_structure& result) const
    {
        for (const std::string_view keyword : {std::string_view("EXTNAME"), std::string_view("HDUNAME")}) {
            const std::optional<std::size_t> position = cards_.index_of(keyword);
            if (position && cards_[*position].value().kind() == value_kind::string) {
                const std::string name = joined_string(cards_.cards(), *position);
                result.name = trim_trailing_spaces(name);
                break;
            }
        }
        try {
            result.version = cards_.find<std::int64_t>("EXTVER").value_or(1);
        } catch (const invalid_data_error&) {
            result.version = 1;
        }
    }

    // The keywords that the standard confines to the primary header or to the headers of extensions, and those of
    // extensions and random groups in the header of a primary array, which the reader ignores there: the size of its
    // data follows from NAXISn alone (standard §4.4.1.1, Eq. 1).
    void check_places(hdu_kind kind)
    {
        for (std::size_t i = 1; i < cards_.size(); ++i) {
            const std::string_view keyword = cards_[i].keyword();
            if (index_ == 0) {
                if (keyword == "XTENSION") {
                    log_.warning(errc::keyword_order, "4.4.1.2",
                                 "XTENSION is a keyword of extensions, which must not appear in the primary header",
                                 at(i));
                } else if (keyword == "INHERIT") {
                    log_.warning(errc::reserved_keyword, "Appendix K",
                                 "INHERIT is a keyword of extensions, which must not appear in the primary header",
                                 at(i));
                } else if (kind == hdu_kind::primary &&
                           (keyword == "PCOUNT" || keyword == "GCOUNT" || keyword == "GROUPS")) {
                    log_.warning(errc::reserved_keyword, "4.4.1.1",
                                 name_at(i) +
                                     " is a keyword of extensions and random groups, which the header of a primary "
                                     "array does not take, and is ignored",
                                 at(i));
                }
            } else if (keyword == "SIMPLE" || keyword == "EXTEND") {
                log_.warning(errc::keyword_order, keyword == "SIMPLE" ? "4.4.1.1" : "4.4.2.1",
                             name_at(i) + " is a keyword of the primary header, which must not appear in an extension",
                             at(i));
            }
        }
    }

    // The type of the values of the reserved keywords, and the values of the keywords of arrays.
    void check_reserved(hdu_kind kind, std::optional<std::int64_t> bitpix)
    {
        const bool array = kind == hdu_kind::primary || kind == hdu_kind::image || kind == hdu_kind::random_groups ||
                           kind == hdu_kind::compressed_image;
        for (std::size_t i = 0; i < cards_.size(); ++i) {
            const card& value = cards_[i];
            if (!value.has_value_indicator()) {
                continue;
            }
            // The keyword first, which leaves the values of the keywords that the standard does not reserve unread.
            const std::string_view keyword = value.keyword();
            const bool scaling = keyword == "BSCALE" || keyword == "BZERO";
            const bool other = keyword == "BLANK" || keyword == "EXTEND";
            const std::optional<value_class> expected = scaling || other ? std::nullopt : reserved_value_class(keyword);
            if (!scaling && !other && !expected) {
                continue;
            }
            const value_kind held = value.value().kind();
            if (held == value_kind::undefined || held == value_kind::none) {
                continue;
            }
            if (scaling) {
                if (array) {
                    check_bscale(i);
                }
            } else if (keyword == "BLANK") {
                if (array) {
                    check_blank(i, kind, bitpix);
                }
            } else if (keyword == "EXTEND") {
                if (index_ == 0) {
                    check_class(i, value_class::logical);
                }
            } else if (expected) {
                check_class(i, *expected);
            }
        }
    }

    void check_bscale(std::size_t position)
    {
        if (!check_class(position, value_class::number) || cards_[position].keyword() != "BSCALE") {
            return;
        }
        const card_value& value = cards_[position].value();
        const bool zero = value.kind() == value_kind::real
                              ? value.get<double>() == 0.0
                              : std::holds_alternative<std::int64_t>(value.data()) && value.get<std::int64_t>() == 0;
        if (zero) {
            log_.error(errc::reserved_keyword, "4.4.2.5", "BSCALE is 0, which leaves the data of the unit unavailable",
                       at(position));
        }
    }

    void check_blank(std::size_t position, hdu_kind kind, std::optional<std::int64_t> bitpix)
    {
        if (!check_class(position, value_class::integer)) {
            return;
        }
        // The BITPIX of a compressed image is that of its table; ZBITPIX gives the type of its pixels.
        if (kind != hdu_kind::compressed_image && bitpix && *bitpix < 0) {
            log_.warning(errc::reserved_keyword, "4.4.2.5",
                         "BLANK is ignored, since BITPIX " + std::to_string(*bitpix) + " is of floating-point data",
                         at(position));
        }
    }

    // True when the value of the card at position is of the type expected; a warning otherwise.
    bool check_class(std::size_t position, value_class expected)
    {
        const card& value = cards_[position];
        const value_kind held = value.value().kind();
        switch (expected) {
        case value_class::logical:
            if (held == value_kind::logical) {
                return true;
            }
            if (held == value_kind::string) {
                const std::string text = joined_string(cards_.cards(), position);
                if (text == "T" || text == "F") {
                    log_.warning(errc::keyword_type, reserved_section(cards_[position].keyword()),
                                 name_at(position) + " holds the string " + quote(text) +
                                     ", which is read as the logical",
                                 at(position));
                    return true;
                }
            }
            return wrong_type(position, "a logical");
        case value_class::integer:
            return held == value_kind::integer || wrong_type(position, "an integer");
        case value_class::number:
            return held == value_kind::integer || held == value_kind::real || wrong_type(position, "a number");
        case value_class::string:
            return held == value_kind::string || wrong_type(position, "a string");
        case value_class::date:
            if (held != value_kind::string) {
                return wrong_type(position, "a date");
            }
            check_date(position);
            return true;
        }
        return true;
    }

    bool wrong_type(std::size_t position, std::string_view type)
    {
        log_.warning(errc::reserved_keyword, reserved_section(cards_[position].keyword()),
                     "the reserved keyword " + name_at(position) + " does not hold " + std::string(type) +
                         ", as the standard requires",
                     at(position));
        return false;
    }

    // A date of standard §9.1.1, or of the form DD/MM/YY of files written before 2000 (standard §4.4.2.1), read with a
    // warning.
    void check_date(std::size_t position)
    {
        const std::string text = joined_string(cards_.cards(), position);
        try {
            const date_time date = parse_date(text);
            if (text.size() == 8 && text[2] == '/' && text[5] == '/') {
                log_.warning(errc::invalid_date, "4.4.2.1",
                             name_at(position) + " holds the date " + quote(text) +
                                 " in the form DD/MM/YY of files written before 2000, which is read as " +
                                 format_date(date),
                             at(position));
            }
        } catch (const invalid_data_error&) {
            log_.warning(
                errc::invalid_date, "9.1.1",
                name_at(position) + " holds " + quote(text) +
                    ", which is not a date of the form YYYY-MM-DD[Thh:mm:ss[.s...]] that the standard requires",
                at(position));
        }
    }

    const header& cards_;
    std::uint64_t index_;
    std::uint64_t header_offset_;
    diagnostic_log& log_;
    // The kind of the unit, once its first keyword and its axes are read.
    hdu_kind kind_ = hdu_kind::primary;
};

} // namespace

unit_structure analyze_header(const header& cards, std::uint64_t index, std::uint64_t header_offset,
                              diagnostic_log& log)
{
    return analysis(cards, index, header_offset, log).run();
}

} // namespace openfits::detail
