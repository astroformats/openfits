// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Custom I/O. Files are read from an openfits::input_source and written to an openfits::output_sink, which an
// application can implement over its own storage. This program implements a source over the function table of a
// storage library written in C, and a sink that writes into a container of the application. It writes a small FITS
// file through the sink the way a streaming writer does, completing a keyword of its header once the data are written,
// then reads the header and the data back through the source.

#include <openfits/openfits.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

namespace {

// The interface of a storage library written in C: an opaque blob and a table of functions. read_at returns 0 on
// success.
struct blob_functions
{
    std::uint64_t (*size)(const void* blob);
    int (*read_at)(const void* blob, std::uint64_t offset, void* buffer, std::size_t length);
};

// A source over the function table. It does not declare concurrent reads, so the library calls read() from one thread
// at a time and the C functions need no locking.
class blob_source final : public openfits::input_source
{
public:
    blob_source(const blob_functions& functions, const void* blob) : functions_(functions), blob_(blob) {}

    std::uint64_t size() const override
    {
        return functions_.size(blob_);
    }

    void read(std::uint64_t offset, std::span<std::byte> destination) const override
    {
        if (functions_.read_at(blob_, offset, destination.data(), destination.size()) != 0) {
            throw openfits::io_error(openfits::errc::read_failed, "cannot read the blob", {}, {.offset = offset});
        }
    }

    std::string description() const override
    {
        return "the blob";
    }

private:
    blob_functions functions_;
    const void* blob_;
};

// A sink into a std::deque that the application owns. It supports rewrite(), which lets a writer complete a header
// once the data that it describes are written.
class deque_sink final : public openfits::output_sink
{
public:
    explicit deque_sink(std::deque<std::byte>& bytes) : bytes_(bytes) {}

    void write(std::span<const std::byte> data) override
    {
        bytes_.insert(bytes_.end(), data.begin(), data.end());
    }

    std::uint64_t position() const override
    {
        return bytes_.size();
    }

    bool can_rewrite() const override
    {
        return true;
    }

    void rewrite(std::uint64_t offset, std::span<const std::byte> data) override
    {
        std::ranges::copy(data, bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
    }

private:
    std::deque<std::byte>& bytes_;
};

// The storage library, played here by a std::deque.
std::uint64_t deque_size(const void* blob)
{
    return static_cast<const std::deque<std::byte>*>(blob)->size();
}

int deque_read_at(const void* blob, std::uint64_t offset, void* buffer, std::size_t length)
{
    const auto& bytes = *static_cast<const std::deque<std::byte>*>(blob);
    if (offset > bytes.size() || length > bytes.size() - offset) {
        return -1;
    }
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), length, static_cast<std::byte*>(buffer));
    return 0;
}

// FITS files are made of blocks of 2880 bytes, and headers of keyword records of 80 characters (standard §3.1, §4.1).
constexpr std::size_t block_size = 2880;
constexpr std::size_t record_size = 80;

// A keyword record with an integer value, in the fixed format: the name in bytes 1 to 8, "= " in 9 and 10, and the
// value right-justified in bytes 11 to 30 (standard §4.2.3).
std::string integer_record(std::string_view keyword, std::uint64_t value)
{
    std::string record(keyword);
    record.resize(8, ' ');
    record += "= ";
    const std::string digits = std::to_string(value);
    record.append(20 - digits.size(), ' ');
    record += digits;
    record.resize(record_size, ' ');
    return record;
}

std::span<const std::byte> bytes_of(std::string_view text)
{
    return std::as_bytes(std::span(text));
}

} // namespace

int main()
{
    try {
        std::deque<std::byte> storage;
        deque_sink sink(storage);

        // A primary header of one block, for one axis of 8-bit values whose length NAXIS1 is known only once the data
        // are written: its record is written with 0 first, at a place the program keeps, and rewritten at the end.
        std::string header = "SIMPLE  =                    T";
        header.resize(record_size, ' ');
        header += integer_record("BITPIX", 8);
        header += integer_record("NAXIS", 1);
        const std::uint64_t naxis1_offset = header.size();
        header += integer_record("NAXIS1", 0);
        header += "END";
        header.resize(block_size, ' ');
        sink.write(bytes_of(header));

        // The data, then the zeros that fill their last block.
        const std::string_view data = "written through a custom sink, read through a custom source";
        sink.write(bytes_of(data));
        const std::string fill(block_size - (data.size() % block_size), '\0');
        sink.write(bytes_of(fill));

        sink.rewrite(naxis1_offset, bytes_of(integer_record("NAXIS1", data.size())));
        sink.finish();

        // The header and the data read back through the C functions.
        const blob_functions functions{.size = deque_size, .read_at = deque_read_at};
        const blob_source source(functions, &storage);
        std::string record(record_size, ' ');
        source.read(naxis1_offset, std::as_writable_bytes(std::span(record)));
        const openfits::card naxis1 = openfits::card::parse(record);
        const auto length = naxis1.value().get<std::int64_t>();
        std::string stored(static_cast<std::size_t>(length), '\0');
        source.read(block_size, std::as_writable_bytes(std::span(stored)));

        std::cout << source.description() << " holds " << source.size() << " bytes, " << naxis1.keyword() << " = "
                  << length << ": " << stored << '\n';
        return stored == data && source.size() == 2 * block_size ? 0 : 1;
    } catch (const std::exception& failure) {
        (void)std::fputs(failure.what(), stderr);
        (void)std::fputs("\n", stderr);
        return 1;
    }
}
