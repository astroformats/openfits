// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "header/keywords.h"

#include "core/ascii.h"
#include "core/endian.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <string>
#include <utility>

namespace openfits::detail {

namespace {

constexpr auto structural_names = std::to_array<std::string_view>(
    {"SIMPLE", "BITPIX", "NAXIS", "EXTEND", "XTENSION", "PCOUNT", "GCOUNT", "GROUPS", "TFIELDS", "THEAP", "BSCALE",
     "BZERO", "BLANK", "CHECKSUM", "DATASUM", "CONTINUE", "END"});

// TTYPEn, TUNITn and TDISPn among them: the writer of tables writes them from the models of the columns.
constexpr auto structural_roots = std::to_array<std::string_view>(
    {"NAXIS", "TFORM", "TBCOL", "TSCAL", "TZERO", "TNULL", "TDIM", "TTYPE", "TUNIT", "TDISP"});

// The keywords of tiled image and table compression (standard §10), which the library writes from the model.
constexpr auto compression_names = std::to_array<std::string_view>(
    {"ZIMAGE",   "ZCMPTYPE", "ZBITPIX",  "ZNAXIS",  "ZMASKCMP", "ZQUANTIZ", "ZDITHER0", "ZSIMPLE",
     "ZEXTEND",  "ZBLOCKED", "ZTENSION", "ZPCOUNT", "ZGCOUNT",  "ZHECKSUM", "ZDATASUM", "ZTABLE",
     "ZTILELEN", "ZTHEAP",   "ZHEAPPTR", "ZSCALE",  "ZZERO",    "ZBLANK"});

constexpr auto compression_roots =
    std::to_array<std::string_view>({"ZNAXIS", "ZTILE", "ZNAME", "ZVAL", "ZCTYP", "ZFORM"});

// The reserved keywords of fixed name that are not structural: §4.4.2 and §6, the deprecated and global forms of
// Table 22, the global time keywords of §9, and the directives of table compression (§10.3.3). Every DATExxxx is
// reserved too (§4.4.2.2).
constexpr auto reserved_names = std::to_array<std::string_view>(
    {"ORIGIN",   "BLOCKED",  "TELESCOP", "INSTRUME", "OBSERVER", "OBJECT",   "AUTHOR",   "REFERENC",
     "COMMENT",  "HISTORY",  "",         "BUNIT",    "DATAMAX",  "DATAMIN",  "EXTNAME",  "EXTVER",
     "EXTLEVEL", "INHERIT",  "EPOCH",    "RADECSYS", "RESTFREQ", "OBSGEO-X", "OBSGEO-Y", "OBSGEO-Z",
     "OBSGEO-B", "OBSGEO-L", "OBSGEO-H", "OBSORBIT", "MJD-OBS",  "BEPOCH",   "JEPOCH",   "MJD-AVG",
     "MJD-BEG",  "MJD-END",  "TSTART",   "TSTOP",    "XPOSURE",  "TELAPSE",  "TIMESYS",  "MJDREF",
     "MJDREFI",  "MJDREFF",  "JDREF",    "JDREFI",   "JDREFF",   "TREFPOS",  "TREFDIR",  "PLEPHEM",
     "TIMEUNIT", "TIMEOFFS", "TIMSYER",  "TIMRDER",  "TIMEDEL",  "TIMEPIXR", "FZALGOR",  "FZTILELN"});

// The global keywords of Table 22 that take the letter of an alternate description at their end.
constexpr auto alternate_names =
    std::to_array<std::string_view>({"WCSAXES", "WCSNAME", "LONPOLE", "LATPOLE", "EQUINOX", "RADESYS", "RESTFRQ",
                                     "RESTWAV", "SPECSYS", "SSYSOBS", "SSYSSRC", "VELOSYS", "ZSOURCE", "VELANGL"});

// The indexed forms of the standard, with every run of digits written #: the indexed keywords of §6 and §7, the forms
// of Table 22 for images, binary-table vectors and pixel lists, whose alternate letter shape_of() takes off, and the
// directives FZALGn (§10.3.3).
constexpr auto reserved_shapes = std::to_array<std::string_view>(
    {"PTYPE#", "PSCAL#", "PZERO#", "TTYPE#", "TUNIT#", "TDISP#", "TDMIN#", "TDMAX#", "TLMIN#", "TLMAX#", "CTYPE#",
     "CUNIT#", "CRVAL#", "CDELT#", "CRPIX#", "CROTA#", "PC#_#",  "CD#_#",  "PV#_#",  "PS#_#",  "CNAME#", "CRDER#",
     "CSYER#", "CZPHS#", "CPERI#", "#CTYP#", "#CTY#",  "TCTYP#", "TCTY#",  "#CUNI#", "#CUN#",  "TCUNI#", "TCUN#",
     "#CRVL#", "#CRV#",  "TCRVL#", "TCRV#",  "#CDLT#", "#CDE#",  "TCDLT#", "TCDE#",  "#CRPX#", "#CRP#",  "TCRPX#",
     "TCRP#",  "#CROT#", "TCROT#", "#PC#",   "TPC#_#", "TP#_#",  "#CD#",   "TCD#_#", "TC#_#",  "#PV#_#", "#V#_#",
     "TPV#_#", "TV#_#",  "#V#_X",  "#PS#_#", "#S#_#",  "TPS#_#", "TS#_#",  "WCSN#",  "TWCS#",  "WCS#",   "#CNA#",
     "TCNA#",  "#CRD#",  "TCRD#",  "#CSY#",  "TCSY#",  "WCST#",  "WCSX#",  "LONP#",  "LATP#",  "EQUI#",  "RADE#",
     "RFRQ#",  "RWAV#",  "SPEC#",  "SOBS#",  "SSRC#",  "OBSGX#", "OBSGY#", "OBSGZ#", "VSYS#",  "ZSOU#",  "VANG#",
     "DOBS#",  "MJDOB#", "DAVG#",  "MJDA#",  "TRPOS#", "TRDIR#", "#CZPH#", "#CZP#",  "TCZPH#", "TCZP#",  "#CPER#",
     "#CPR#",  "TCPER#", "TCPR#",  "WCAX#",  "#CSYE#", "#CRDE#", "TCSYE#", "TCRDE#", "FZALG#"});

// A name or an indexed form, and the type of its values.
using classified = std::pair<std::string_view, value_class>;

// The reserved keywords of fixed name of §4.4.2, Table 22 and §9 that are not structural, with the type of their
// values; those of alternate_names take the letter of an alternate description at their end.
constexpr auto classified_names = std::to_array<classified>({
    {"BLOCKED", value_class::logical},  {"INHERIT", value_class::logical}, {"EXTVER", value_class::integer},
    {"EXTLEVEL", value_class::integer}, {"WCSAXES", value_class::integer}, {"ORIGIN", value_class::string},
    {"TELESCOP", value_class::string},  {"INSTRUME", value_class::string}, {"OBSERVER", value_class::string},
    {"OBJECT", value_class::string},    {"AUTHOR", value_class::string},   {"REFERENC", value_class::string},
    {"BUNIT", value_class::string},     {"EXTNAME", value_class::string},  {"RADECSYS", value_class::string},
    {"RADESYS", value_class::string},   {"WCSNAME", value_class::string},  {"SPECSYS", value_class::string},
    {"SSYSOBS", value_class::string},   {"SSYSSRC", value_class::string},  {"TIMESYS", value_class::string},
    {"TREFPOS", value_class::string},   {"TREFDIR", value_class::string},  {"PLEPHEM", value_class::string},
    {"TIMEUNIT", value_class::string},  {"OBSORBIT", value_class::string}, {"DATE", value_class::date},
    {"DATE-OBS", value_class::date},    {"DATE-AVG", value_class::date},   {"DATE-BEG", value_class::date},
    {"DATE-END", value_class::date},    {"DATEREF", value_class::date},    {"DATAMAX", value_class::number},
    {"DATAMIN", value_class::number},   {"EQUINOX", value_class::number},  {"EPOCH", value_class::number},
    {"RESTFREQ", value_class::number},  {"RESTFRQ", value_class::number},  {"RESTWAV", value_class::number},
    {"LONPOLE", value_class::number},   {"LATPOLE", value_class::number},  {"VELOSYS", value_class::number},
    {"ZSOURCE", value_class::number},   {"VELANGL", value_class::number},  {"OBSGEO-X", value_class::number},
    {"OBSGEO-Y", value_class::number},  {"OBSGEO-Z", value_class::number}, {"OBSGEO-B", value_class::number},
    {"OBSGEO-L", value_class::number},  {"OBSGEO-H", value_class::number}, {"MJD-OBS", value_class::number},
    {"MJD-AVG", value_class::number},   {"MJD-BEG", value_class::number},  {"MJD-END", value_class::number},
    {"BEPOCH", value_class::number},    {"JEPOCH", value_class::number},   {"TSTART", value_class::number},
    {"TSTOP", value_class::number},     {"XPOSURE", value_class::number},  {"TELAPSE", value_class::number},
    {"MJDREF", value_class::number},    {"MJDREFI", value_class::number},  {"MJDREFF", value_class::number},
    {"JDREF", value_class::number},     {"JDREFI", value_class::number},   {"JDREFF", value_class::number},
    {"TIMEOFFS", value_class::number},  {"TIMSYER", value_class::number},  {"TIMRDER", value_class::number},
    {"TIMEDEL", value_class::number},   {"TIMEPIXR", value_class::number},
});

// The indexed forms of Table 22 and §9, and the reserved keywords of random groups (§6.1.2) and tables (§7.2.2,
// §7.3.2) that are not structural, written as reserved_shapes writes them, with the type of their values.
constexpr auto classified_shapes = std::to_array<classified>({
    {"WCAX#", value_class::integer}, {"CTYPE#", value_class::string}, {"CUNIT#", value_class::string},
    {"PS#_#", value_class::string},  {"CNAME#", value_class::string}, {"#CTYP#", value_class::string},
    {"#CTY#", value_class::string},  {"TCTYP#", value_class::string}, {"TCTY#", value_class::string},
    {"#CUNI#", value_class::string}, {"#CUN#", value_class::string},  {"TCUNI#", value_class::string},
    {"TCUN#", value_class::string},  {"#PS#_#", value_class::string}, {"#S#_#", value_class::string},
    {"TPS#_#", value_class::string}, {"TS#_#", value_class::string},  {"WCSN#", value_class::string},
    {"TWCS#", value_class::string},  {"WCS#", value_class::string},   {"#CNA#", value_class::string},
    {"TCNA#", value_class::string},  {"WCST#", value_class::string},  {"WCSX#", value_class::string},
    {"RADE#", value_class::string},  {"SPEC#", value_class::string},  {"SOBS#", value_class::string},
    {"SSRC#", value_class::string},  {"TRPOS#", value_class::string}, {"TRDIR#", value_class::string},
    {"DOBS#", value_class::date},    {"DAVG#", value_class::date},    {"CRVAL#", value_class::number},
    {"CDELT#", value_class::number}, {"CRPIX#", value_class::number}, {"CROTA#", value_class::number},
    {"PC#_#", value_class::number},  {"CD#_#", value_class::number},  {"PV#_#", value_class::number},
    {"CRDER#", value_class::number}, {"CSYER#", value_class::number}, {"CZPHS#", value_class::number},
    {"CPERI#", value_class::number}, {"#CRVL#", value_class::number}, {"#CRV#", value_class::number},
    {"TCRVL#", value_class::number}, {"TCRV#", value_class::number},  {"#CDLT#", value_class::number},
    {"#CDE#", value_class::number},  {"TCDLT#", value_class::number}, {"TCDE#", value_class::number},
    {"#CRPX#", value_class::number}, {"#CRP#", value_class::number},  {"TCRPX#", value_class::number},
    {"TCRP#", value_class::number},  {"#CROT#", value_class::number}, {"TCROT#", value_class::number},
    {"#PC#", value_class::number},   {"TPC#_#", value_class::number}, {"TP#_#", value_class::number},
    {"#CD#", value_class::number},   {"TCD#_#", value_class::number}, {"TC#_#", value_class::number},
    {"#PV#_#", value_class::number}, {"#V#_#", value_class::number},  {"TPV#_#", value_class::number},
    {"TV#_#", value_class::number},  {"#V#_X", value_class::number},  {"#CRD#", value_class::number},
    {"TCRD#", value_class::number},  {"#CSY#", value_class::number},  {"TCSY#", value_class::number},
    {"LONP#", value_class::number},  {"LATP#", value_class::number},  {"EQUI#", value_class::number},
    {"RFRQ#", value_class::number},  {"RWAV#", value_class::number},  {"OBSGX#", value_class::number},
    {"OBSGY#", value_class::number}, {"OBSGZ#", value_class::number}, {"VSYS#", value_class::number},
    {"ZSOU#", value_class::number},  {"VANG#", value_class::number},  {"MJDOB#", value_class::number},
    {"MJDA#", value_class::number},  {"#CZPH#", value_class::number}, {"#CZP#", value_class::number},
    {"TCZPH#", value_class::number}, {"TCZP#", value_class::number},  {"#CPER#", value_class::number},
    {"#CPR#", value_class::number},  {"TCPER#", value_class::number}, {"TCPR#", value_class::number},
    {"#CSYE#", value_class::number}, {"#CRDE#", value_class::number}, {"TCSYE#", value_class::number},
    {"TCRDE#", value_class::number}, {"TDMIN#", value_class::number}, {"TDMAX#", value_class::number},
    {"TLMIN#", value_class::number}, {"TLMAX#", value_class::number}, {"PTYPE#", value_class::string},
    {"PSCAL#", value_class::number}, {"PZERO#", value_class::number},
});

// The tables above sorted at compile time, for the binary searches of the lookups, which the analysis of a header makes
// for each of its cards; the tables keep the order of the standard, which a reader follows.
template <std::size_t N> constexpr std::array<std::string_view, N> sorted(std::array<std::string_view, N> names)
{
    std::sort(names.begin(), names.end());
    return names;
}

template <std::size_t N> constexpr std::array<classified, N> sorted(std::array<classified, N> names)
{
    std::sort(names.begin(), names.end(), [](const classified& a, const classified& b) { return a.first < b.first; });
    return names;
}

// True when a sorted table holds each name once.
template <typename T, std::size_t N, typename Key> constexpr bool unique(const std::array<T, N>& table, Key key)
{
    for (std::size_t i = 1; i < N; ++i) {
        if (key(table[i - 1]) == key(table[i])) {
            return false;
        }
    }
    return true;
}

constexpr auto name_of = [](std::string_view name) { return name; };
constexpr auto first_of = [](const classified& entry) { return entry.first; };

constexpr auto sorted_structural_names = sorted(structural_names);
constexpr auto sorted_structural_roots = sorted(structural_roots);
constexpr auto sorted_compression_names = sorted(compression_names);
constexpr auto sorted_compression_roots = sorted(compression_roots);
constexpr auto sorted_reserved_names = sorted(reserved_names);
constexpr auto sorted_alternate_names = sorted(alternate_names);
constexpr auto sorted_reserved_shapes = sorted(reserved_shapes);
constexpr auto sorted_classified_names = sorted(classified_names);
constexpr auto sorted_classified_shapes = sorted(classified_shapes);
static_assert(unique(sorted_structural_names, name_of) && unique(sorted_structural_roots, name_of) &&
              unique(sorted_compression_names, name_of) && unique(sorted_compression_roots, name_of) &&
              unique(sorted_reserved_names, name_of) && unique(sorted_alternate_names, name_of) &&
              unique(sorted_reserved_shapes, name_of) && unique(sorted_classified_names, first_of) &&
              unique(sorted_classified_shapes, first_of));

// The first characters of the names, roots and forms of the tables above, a digit for the # that starts a form: a
// keyword that starts with another character is in none of them, which the lookups that the analysis of a header makes
// for each of its cards tell at once for most keywords.
constexpr std::array<bool, 256> first_characters = [] {
    std::array<bool, 256> marked{};
    const auto mark = [&marked](std::string_view name) {
        if (name.empty()) {
            return;
        }
        if (name.front() == '#') {
            for (char digit = '0'; digit <= '9'; ++digit) {
                marked[static_cast<unsigned char>(digit)] = true;
            }
        } else {
            marked[static_cast<unsigned char>(name.front())] = true;
        }
    };
    for (const std::string_view name : structural_names) {
        mark(name);
    }
    for (const std::string_view name : structural_roots) {
        mark(name);
    }
    for (const std::string_view name : compression_names) {
        mark(name);
    }
    for (const std::string_view name : compression_roots) {
        mark(name);
    }
    for (const std::string_view name : reserved_names) {
        mark(name);
    }
    for (const std::string_view name : alternate_names) {
        mark(name);
    }
    for (const std::string_view name : reserved_shapes) {
        mark(name);
    }
    for (const classified& entry : classified_names) {
        mark(entry.first);
    }
    for (const classified& entry : classified_shapes) {
        mark(entry.first);
    }
    mark("DATE");
    return marked;
}();

// True when name may be in one of the tables, a name of DATE.. among them: false for a keyword that starts with a
// character that starts none of them. The blank keyword is in reserved_names.
bool may_be_listed(std::string_view name) noexcept
{
    return name.empty() || first_characters[static_cast<unsigned char>(name.front())];
}

template <std::size_t N> bool contains(const std::array<std::string_view, N>& names, std::string_view name) noexcept
{
    return std::binary_search(names.begin(), names.end(), name);
}

template <std::size_t N>
std::optional<value_class> class_of(const std::array<classified, N>& names, std::string_view name) noexcept
{
    const auto found =
        std::lower_bound(names.begin(), names.end(), name,
                         [](const classified& entry, std::string_view key) { return entry.first < key; });
    if (found == names.end() || found->first != name) {
        return std::nullopt;
    }
    return found->second;
}

// The root of an indexed keyword: name without the one to three digits that end it, the first of them not 0, as
// keyword_index() takes them; empty for a name that does not end so. No root of the tables ends with a digit.
std::string_view indexed_root(std::string_view name) noexcept
{
    std::size_t end = name.size();
    while (end > 0 && is_digit(name[end - 1])) {
        --end;
    }
    const std::size_t digits = name.size() - end;
    if (end == 0 || digits == 0 || digits > 3 || name[end] == '0') {
        return {};
    }
    return name.substr(0, end);
}

// name with every run of digits written #, without the letter A to Z of an alternate description that follows a digit
// at its end: in a buffer of the size of a record, which a name longer than it, no shape of the standard, leaves empty.
class keyword_shape
{
public:
    explicit keyword_shape(std::string_view name) noexcept
    {
        if (name.size() > chars_.size()) {
            return;
        }
        for (const char c : name) {
            if (!is_digit(c)) {
                chars_[size_++] = c;
            } else if (size_ == 0 || chars_[size_ - 1] != '#') {
                chars_[size_++] = '#';
            }
        }
        if (size_ >= 2 && is_upper(chars_[size_ - 1]) && chars_[size_ - 2] == '#') {
            --size_;
        }
    }

    [[nodiscard]] std::string_view view() const noexcept
    {
        return {chars_.data(), size_};
    }

private:
    std::array<char, 80> chars_{};
    std::size_t size_ = 0;
};

} // namespace

std::uint64_t keyword_prefix(std::string_view keyword) noexcept
{
    // The bytes in an integer of eight, the first the most significant: one load, swapped on a little-endian host.
    std::array<unsigned char, 8> bytes{};
    std::copy_n(keyword.begin(), std::min(keyword.size(), bytes.size()), bytes.begin());
    std::uint64_t prefix = 0;
    std::memcpy(&prefix, bytes.data(), bytes.size());
    if constexpr (std::endian::native == std::endian::little) {
        prefix = byte_swap(prefix);
    }
    return prefix;
}

std::optional<std::uint32_t> keyword_index(std::string_view name, std::string_view root) noexcept
{
    if (!name.starts_with(root)) {
        return std::nullopt;
    }
    const std::string_view digits = name.substr(root.size());
    if (digits.empty() || digits.size() > 3 || digits.front() == '0' || !std::ranges::all_of(digits, is_digit)) {
        return std::nullopt;
    }
    std::uint32_t index = 0;
    for (const char c : digits) {
        index = (index * 10) + static_cast<std::uint32_t>(c - '0');
    }
    return index;
}

bool is_structural_keyword(std::string_view name) noexcept
{
    if (!may_be_listed(name)) {
        return false;
    }
    if (contains(sorted_structural_names, name) || contains(sorted_compression_names, name)) {
        return true;
    }
    const std::string_view root = indexed_root(name);
    return !root.empty() && (contains(sorted_structural_roots, root) || contains(sorted_compression_roots, root));
}

bool is_reserved_keyword(std::string_view name)
{
    if (!may_be_listed(name)) {
        return false;
    }
    if (is_structural_keyword(name) || contains(sorted_reserved_names, name) || name.starts_with("DATE")) {
        return true;
    }
    if (contains(sorted_alternate_names, name) || (name.size() > 1 && is_upper(name.back()) &&
                                                   contains(sorted_alternate_names, name.substr(0, name.size() - 1)))) {
        return true;
    }
    return std::ranges::any_of(name, is_digit) && contains(sorted_reserved_shapes, keyword_shape(name).view());
}

std::optional<value_class> reserved_value_class(std::string_view name)
{
    if (!may_be_listed(name) || is_structural_keyword(name)) {
        return std::nullopt;
    }
    if (const std::optional<value_class> kind = class_of(sorted_classified_names, name)) {
        return kind;
    }
    if (name.size() > 1 && is_upper(name.back())) {
        const std::string_view root = name.substr(0, name.size() - 1);
        if (contains(sorted_alternate_names, root)) {
            return class_of(sorted_classified_names, root);
        }
    }
    if (!std::ranges::any_of(name, is_digit)) {
        return std::nullopt;
    }
    return class_of(sorted_classified_shapes, keyword_shape(name).view());
}

std::string_view reserved_section(std::string_view name)
{
    using section_of = std::pair<std::string_view, std::string_view>;
    static constexpr auto named = std::to_array<section_of>({
        {"DATE", "4.4.2.1"},     {"ORIGIN", "4.4.2.1"},   {"EXTEND", "4.4.2.1"},     {"BLOCKED", "4.4.2.1"},
        {"DATE-OBS", "4.4.2.2"}, {"TELESCOP", "4.4.2.2"}, {"INSTRUME", "4.4.2.2"},   {"OBSERVER", "4.4.2.2"},
        {"OBJECT", "4.4.2.2"},   {"AUTHOR", "4.4.2.3"},   {"REFERENC", "4.4.2.3"},   {"COMMENT", "4.4.2.4"},
        {"HISTORY", "4.4.2.4"},  {"", "4.4.2.4"},         {"CONTINUE", "4.4.2.4"},   {"BSCALE", "4.4.2.5"},
        {"BZERO", "4.4.2.5"},    {"BUNIT", "4.4.2.5"},    {"BLANK", "4.4.2.5"},      {"DATAMAX", "4.4.2.5"},
        {"DATAMIN", "4.4.2.5"},  {"EXTNAME", "4.4.2.6"},  {"EXTVER", "4.4.2.6"},     {"EXTLEVEL", "4.4.2.6"},
        {"CHECKSUM", "4.4.2.7"}, {"DATASUM", "4.4.2.7"},  {"INHERIT", "Appendix K"}, {"FZALGOR", "10.3.3"},
        {"FZTILELN", "10.3.3"},
    });
    static constexpr auto shaped = std::to_array<section_of>({
        {"PTYPE#", "6.1.2"},
        {"PSCAL#", "6.1.2"},
        {"PZERO#", "6.1.2"},
        {"TDMIN#", "7.3.2"},
        {"TDMAX#", "7.3.2"},
        {"TLMIN#", "7.3.2"},
        {"TLMAX#", "7.3.2"},
        {"FZALG#", "10.3.3"},
    });
    for (const section_of& entry : named) {
        if (entry.first == name) {
            return entry.second;
        }
    }
    if (std::ranges::any_of(name, is_digit)) {
        const keyword_shape shape_of_name(name);
        const std::string_view shape = shape_of_name.view();
        for (const section_of& entry : shaped) {
            if (entry.first == shape) {
                return entry.second;
            }
        }
    }
    // The keywords of world coordinates and of time, which Table 22 lists with the types of their values.
    return "8.2";
}

} // namespace openfits::detail
