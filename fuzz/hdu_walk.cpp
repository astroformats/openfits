// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// A file of many hostile units built from the input (standard §3, §4.4.1): every four bytes describe one unit, its
// type, BITPIX, axes, counts, name, and the ways it departs from the standard, so that the fuzzer reaches deep walks
// without writing blocks. The walk under small limits must give the units in order, each after the data of the one
// before, the same by index, by name and by count; and the conformance report of verify() must hold every diagnostic of
// the walk, each of a unit that the walk reached, without an exception. A failure must be an error of the library's
// kinds; any other exception escapes and fails the run.

#include <openfits/error.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/verify.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using openfits::hdu_info;
using openfits::reader;

void require(bool condition)
{
    if (!condition) {
        std::abort();
    }
}

std::string record(std::string_view text)
{
    std::string result(text.substr(0, 80));
    result.resize(80, ' ');
    return result;
}

std::string card(std::string_view keyword, const std::string& value)
{
    std::string text(keyword);
    text.resize(8, ' ');
    text += "= ";
    if (value.size() < 20 && !value.starts_with('\'')) {
        text.append(20 - value.size(), ' ');
    }
    return record(text + value);
}

// The file that the input describes.
class builder
{
public:
    void unit(std::span<const std::uint8_t> unit, std::size_t index)
    {
        constexpr std::array<int, 8> bitpix{8, 16, 32, 64, -32, -64, 7, 24};
        constexpr std::array<std::string_view, 6> types{"IMAGE", "BINTABLE", "TABLE", "A3DTABLE", "image", "IMAGE"};
        const std::uint8_t kind = unit[0] % 8;
        const bool primary = index == 0;
        if (!primary && kind == 7) {
            // Bytes that are not a header: special records or garbage.
            bytes_.append(static_cast<std::size_t>(unit[1]) * 16, static_cast<char>(unit[2]));
            return;
        }
        const int bits = bitpix[(unit[0] >> 3U) % bitpix.size()];
        const std::size_t naxis = unit[1] % 4;
        const std::uint64_t length = unit[1] >> 2U;
        const std::uint64_t pcount = unit[2] % 8;
        const std::uint64_t gcount = (unit[2] >> 3U) % 4;
        const std::uint8_t flags = unit[3];

        std::vector<std::string> records;
        if (primary) {
            records.push_back(card("SIMPLE", (flags & 1U) != 0 ? "F" : "T"));
        } else {
            std::string type = "'" + std::string(types[kind % types.size()]);
            type.resize(9, ' ');
            records.push_back(card("XTENSION", type + "'"));
        }
        records.push_back(card("BITPIX", std::to_string(bits)));
        records.push_back(card("NAXIS", std::to_string(naxis)));
        for (std::size_t i = 1; i <= naxis; ++i) {
            records.push_back(
                card("NAXIS" + std::to_string(i), std::to_string(i == 1 && (flags & 2U) != 0 ? 0 : length)));
        }
        if (!primary || (flags & 4U) != 0) {
            if ((flags & 8U) == 0) {
                records.push_back(card("PCOUNT", std::to_string(pcount)));
                records.push_back(card("GCOUNT", std::to_string(gcount)));
            }
            if (primary) {
                records.push_back(card("GROUPS", "T"));
            }
        }
        if ((flags & 16U) != 0 && records.size() > 3) {
            std::swap(records[1], records[3]);
        }
        records.push_back(card("EXTNAME", "'U" + std::to_string(index) + "'"));
        const unsigned repeats = static_cast<unsigned>(flags) >> 5U;
        for (unsigned i = 0; i < repeats; ++i) {
            records.push_back(card("LONG", "'continued&'"));
            records.push_back(record("CONTINUE  'more&'"));
        }
        for (const std::string& text : records) {
            bytes_ += text;
        }
        if ((flags & 32U) == 0 || index % 2 == 0) {
            bytes_ += record("END");
        }
        pad(' ');
        // The data the header describes, short by a few bytes when the flags say so.
        std::uint64_t size = naxis == 0 ? 0 : 1;
        for (std::size_t i = 1; i <= naxis; ++i) {
            size *= i == 1 && (flags & 2U) != 0 ? 0 : length;
        }
        const auto bytes = static_cast<std::uint64_t>(bits < 0 ? -bits : bits) / 8;
        size = bytes * (gcount == 0 ? 1 : gcount) * (pcount + size);
        bytes_.append(static_cast<std::size_t>(std::min<std::uint64_t>(size, 1U << 16U)), '\0');
        pad('\0');
    }

    [[nodiscard]] std::vector<std::byte> bytes() const
    {
        std::vector<std::byte> result(bytes_.size());
        for (std::size_t i = 0; i < bytes_.size(); ++i) {
            result[i] = static_cast<std::byte>(bytes_[i]);
        }
        return result;
    }

private:
    void pad(char fill)
    {
        bytes_.append((2880 - (bytes_.size() % 2880)) % 2880, fill);
    }

    std::string bytes_;
};

} // namespace

// The entry point that libFuzzer, and the replay driver, call for every input. Its name is fixed by libFuzzer.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    builder file;
    for (std::size_t offset = 0, index = 0; offset + 4 <= size && index < 64; offset += 4, ++index) {
        file.unit({data + offset, 4}, index);
    }
    openfits::read_options options;
    options.limits.max_hdus = 48;
    options.limits.max_header_bytes = std::uint64_t{8} * 2880;
    options.limits.max_total_header_bytes = std::uint64_t{96} * 2880;
    options.limits.max_diagnostics = 32;
    options.strict = size % 4 == 1;

    std::optional<reader> input;
    try {
        input.emplace(std::make_unique<openfits::memory_source>(file.bytes()), options);
    } catch (const openfits::invalid_data_error&) {
        return 0;
    } catch (const openfits::limit_error&) {
        return 0;
    }

    // Asked for by name first, then by count: the same units.
    std::vector<const hdu_info*> named;
    for (std::size_t index = 0; index < 64; ++index) {
        try {
            const std::optional<std::size_t> found = input->find_hdu("u" + std::to_string(index));
            if (!found) {
                break;
            }
            named.push_back(&input->hdu(*found));
        } catch (const openfits::invalid_data_error&) {
            break;
        } catch (const openfits::limit_error&) {
            break;
        }
    }
    const auto units = input->hdus();
    require(units.size() == input->hdu_count());
    require(named.size() <= units.size());
    std::uint64_t next = 0;
    for (std::size_t i = 0; i < units.size(); ++i) {
        const hdu_info& unit = units[i];
        require(unit.index() == i);
        require(unit.header_offset() == next || i == 0);
        require(unit.header_offset() + unit.header_size() <= input->size());
        if (i < named.size()) {
            require(named[i] == &unit);
        }
        if (!unit.data_size()) {
            require(i + 1 == units.size());
            break;
        }
        next = unit.data_offset() + ((*unit.data_size() + 2879) / 2880 * 2880);
    }

    // The report holds the diagnostics of the walk and of the headers, and those of its own checks, each of a unit of
    // the file or of none; its checks throw nothing over a source in memory.
    const std::vector<openfits::diagnostic> report = openfits::verify(*input);
    std::size_t walked = input->diagnostics().size();
    for (const hdu_info& unit : units) {
        walked += unit.diagnostics().size();
    }
    require(report.size() >= walked);
    for (const openfits::diagnostic& finding : report) {
        require(!finding.context.hdu || *finding.context.hdu <= units.size());
    }
    return 0;
}
