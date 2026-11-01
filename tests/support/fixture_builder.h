// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// FITS files built from records and bytes by the tests themselves, without the library's writer, so that the tests of
// the reader do not depend on it: blocks padded and offsets counted here.

namespace openfits::test {

inline constexpr std::size_t record_size = 80;
inline constexpr std::size_t block_bytes = 2880;

/// A record of 80 characters: text padded with spaces.
[[nodiscard]] inline std::string record(std::string_view text)
{
    assert(text.size() <= record_size);
    std::string result(text);
    result.resize(record_size, ' ');
    return result;
}

/// The record of a keyword with a value in the fixed format: the name in bytes 1 to 8, "= ", and the value as given, a
/// string from byte 11, and any other value right-justified in bytes 11 to 30; the comment after " / ".
[[nodiscard]] inline std::string card_record(std::string_view keyword, std::string_view value,
                                             std::string_view comment = {})
{
    std::string text(keyword);
    text.resize(8, ' ');
    text += "= ";
    if (!value.starts_with('\'') && value.size() < 20) {
        text.append(20 - value.size(), ' ');
    }
    text += value;
    if (!comment.empty()) {
        text += " / ";
        text += comment;
    }
    return record(text);
}

/// The mandatory records of a primary header (standard §4.4.1.1): SIMPLE = T, BITPIX, NAXIS and NAXISn.
[[nodiscard]] inline std::vector<std::string> primary_records(int bitpix = 8, std::vector<std::uint64_t> axes = {})
{
    std::vector<std::string> records{card_record("SIMPLE", "T"), card_record("BITPIX", std::to_string(bitpix)),
                                     card_record("NAXIS", std::to_string(axes.size()))};
    for (std::size_t i = 0; i < axes.size(); ++i) {
        records.push_back(card_record("NAXIS" + std::to_string(i + 1), std::to_string(axes[i])));
    }
    return records;
}

/// The mandatory records of an extension (standard §4.4.1.2): XTENSION, BITPIX, NAXIS, NAXISn, PCOUNT and GCOUNT.
[[nodiscard]] inline std::vector<std::string> extension_records(std::string_view type = "IMAGE", int bitpix = 8,
                                                                std::vector<std::uint64_t> axes = {},
                                                                std::uint64_t pcount = 0, std::uint64_t gcount = 1)
{
    std::string xtension = "'" + std::string(type);
    xtension.resize(9, ' ');
    xtension += "'";
    std::vector<std::string> records{card_record("XTENSION", xtension), card_record("BITPIX", std::to_string(bitpix)),
                                     card_record("NAXIS", std::to_string(axes.size()))};
    for (std::size_t i = 0; i < axes.size(); ++i) {
        records.push_back(card_record("NAXIS" + std::to_string(i + 1), std::to_string(axes[i])));
    }
    records.push_back(card_record("PCOUNT", std::to_string(pcount)));
    records.push_back(card_record("GCOUNT", std::to_string(gcount)));
    return records;
}

/// The bytes of values as a file stores them, big-endian, whatever the byte order of the host (standard §5).
template <typename T> [[nodiscard]] std::vector<std::byte> big_endian(const std::vector<T>& values)
{
    std::vector<std::byte> bytes(values.size() * sizeof(T));
    for (std::size_t i = 0; i < values.size(); ++i) {
        std::byte* at = bytes.data() + (i * sizeof(T));
        std::memcpy(at, &values[i], sizeof(T));
        if constexpr (std::endian::native == std::endian::little) {
            std::reverse(at, at + sizeof(T));
        }
    }
    return bytes;
}

/// The records with more after them.
[[nodiscard]] inline std::vector<std::string> with(std::vector<std::string> records,
                                                   const std::vector<std::string>& more)
{
    records.insert(records.end(), more.begin(), more.end());
    return records;
}

/// A file built unit by unit.
class fixture
{
public:
    /// A header of the records, followed by END unless end is false, and spaces to the end of its last block.
    fixture& header(const std::vector<std::string>& records, bool end = true)
    {
        for (const std::string& text : records) {
            append(text);
        }
        if (end) {
            append(record("END"));
        }
        return pad(' ');
    }

    /// size bytes of data, each its index modulo 251, and zeros to the end of the last block.
    fixture& data(std::uint64_t size)
    {
        for (std::uint64_t i = 0; i < size; ++i) {
            bytes_.push_back(static_cast<std::byte>(i % 251));
        }
        return pad('\0');
    }

    /// The bytes given as data, and zeros to the end of the last block.
    fixture& data(std::span<const std::byte> bytes)
    {
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
        return pad('\0');
    }

    /// A primary header without data, of the records given after the mandatory ones.
    fixture& primary(const std::vector<std::string>& more = {})
    {
        return header(with(primary_records(), more));
    }

    /// An IMAGE extension of 8-bit pixels and its data, with the records given after the mandatory ones.
    fixture& image(std::vector<std::uint64_t> axes, const std::vector<std::string>& more = {})
    {
        std::uint64_t size = axes.empty() ? 0 : 1;
        for (const std::uint64_t axis : axes) {
            size *= axis;
        }
        header(with(extension_records("IMAGE", 8, std::move(axes)), more));
        return data(size);
    }

    /// Text appended as it is, without padding.
    fixture& append(std::string_view text)
    {
        for (const char c : text) {
            bytes_.push_back(static_cast<std::byte>(c));
        }
        return *this;
    }

    /// The file cut to its first size bytes.
    fixture& truncate(std::size_t size)
    {
        bytes_.resize(size);
        return *this;
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return bytes_.size();
    }

    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept
    {
        return bytes_;
    }

private:
    fixture& pad(char fill)
    {
        while (bytes_.size() % block_bytes != 0) {
            bytes_.push_back(static_cast<std::byte>(fill));
        }
        return *this;
    }

    std::vector<std::byte> bytes_{};
};

} // namespace openfits::test
