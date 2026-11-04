// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Files compressed whole with gzip (RFC 1952), a convention outside the standard: their inflation by gzip_source, into
// memory or into a temporary file, the members and the bytes after them, the limits, and their compression by
// gzip_sink, through the writer too.

#include <openfits/error.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/writer.h>

#include "compress/gzip.h"
#include "support/bytes.h"
#include "support/compressed_fixtures.h"
#include "support/files.h"
#include "support/fixture_builder.h"
#include "support/sequence.h"
#include "support/temp_directory.h"
#include "support/throws.h"
#include "support/written_file.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using openfits::errc;
using openfits::gzip_sink;
using openfits::gzip_source;
using openfits::integrity_error;
using openfits::limit_error;
using openfits::memory_sink;
using openfits::memory_source;
using openfits::usage_error;
using openfits::test::gzip;
using openfits::test::pattern;
using openfits::test::temp_directory;
using openfits::test::throws;

// Bytes that compress, as the data of a file do: a pattern of a few values.
std::vector<std::byte> content(std::size_t size)
{
    std::vector<std::byte> data(size);
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<std::byte>((i / 7) % 13);
    }
    return data;
}

// Bytes that do not compress.
std::vector<std::byte> noise(std::size_t size)
{
    openfits::test::splitmix64 random(size);
    std::vector<std::byte> data(size);
    for (std::byte& value : data) {
        value = static_cast<std::byte>(random.next() >> 56U);
    }
    return data;
}

std::vector<std::byte> joined(std::initializer_list<std::span<const std::byte>> parts)
{
    std::vector<std::byte> result;
    for (const std::span<const std::byte> part : parts) {
        result.insert(result.end(), part.begin(), part.end());
    }
    return result;
}

// Everything that a source gives, read through read().
std::vector<std::byte> read_all(const openfits::input_source& source)
{
    const std::size_t size = source.size();
    std::vector<std::byte> data(size);
    source.read(0, data);
    return data;
}

// A source whose content does not lie in memory, read through read() alone, as a file.
std::unique_ptr<openfits::callback_source> unmapped(const std::vector<std::byte>& data)
{
    return std::make_unique<openfits::callback_source>(
        data.size(), [&data](std::uint64_t offset, std::span<std::byte> destination) {
            std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(offset), destination.size(), destination.begin());
        });
}

TEST(gzip_file_inflater, inflates_input_and_output_given_a_byte_at_a_time)
{
    const std::vector<std::byte> data = content(3000);
    const std::vector<std::byte> stream = joined({gzip(data), gzip(std::span(data).first(100))});
    openfits::detail::gzip_file_inflater inflater(1);
    std::vector<std::byte> output;
    std::array<std::byte, 1> one{};
    for (const std::byte& input : stream) {
        inflater.give(std::span(&input, 1));
        while (!inflater.needs_input()) {
            if (inflater.inflate(one) == 1) {
                output.push_back(one[0]);
            }
        }
    }
    inflater.finish();
    EXPECT_EQ(output, joined({data, std::span(data).first(100)}));
}

TEST(gzip_source, inflates_a_member_into_memory)
{
    const std::vector<std::byte> data = content(100'000);
    const std::vector<std::byte> stream = gzip(data);
    const memory_source compressed{std::span<const std::byte>(stream)};
    const gzip_source source(compressed);

    EXPECT_EQ(source.size(), data.size());
    EXPECT_TRUE(std::ranges::equal(source.bytes(), data));
    EXPECT_EQ(read_all(source), data);
    std::array<std::byte, 10> piece{};
    source.read(5000, piece);
    EXPECT_TRUE(std::ranges::equal(piece, std::span(data).subspan(5000, 10)));
    EXPECT_TRUE(throws<openfits::io_error>(errc::end_of_data, [&] { source.read(data.size() - 5, piece); }));
    EXPECT_TRUE(source.supports_concurrent_reads());
    EXPECT_EQ(source.description(), "a gzip-compressed source, inflated");
}

TEST(gzip_source, reads_a_source_that_is_not_in_memory_a_chunk_at_a_time)
{
    // More than the 4 MiB that a read of the compressed source takes, so that it takes two.
    const std::vector<std::byte> data = noise(std::size_t{5} << 20);
    const std::vector<std::byte> stream = gzip(data);
    ASSERT_GT(stream.size(), std::size_t{4} << 20);
    const gzip_source source(*unmapped(stream));
    EXPECT_TRUE(std::ranges::equal(source.bytes(), data));
}

TEST(gzip_source, inflates_every_member_one_after_the_other)
{
    // The last member is the smallest, so that the size that its trailer gives is not that of the content.
    const std::vector<std::byte> first = content(200'000);
    const std::vector<std::byte> second = pattern(1000);
    const std::vector<std::byte> third = content(10);
    const std::vector<std::byte> stream = joined({gzip(first), gzip(second), gzip(third)});
    const memory_source compressed{std::span<const std::byte>(stream)};
    EXPECT_EQ(read_all(gzip_source(compressed)), joined({first, second, third}));
    // An empty member adds nothing.
    const std::vector<std::byte> with_empty = joined({gzip(first), gzip({})});
    EXPECT_EQ(read_all(gzip_source(memory_source{std::span<const std::byte>(with_empty)})), first);
}

TEST(gzip_source, ignores_zeros_after_the_last_member)
{
    const std::vector<std::byte> data = content(5000);
    const std::vector<std::byte> stream = joined({gzip(data), std::vector<std::byte>(512)});
    EXPECT_EQ(read_all(gzip_source(memory_source{std::span<const std::byte>(stream)})), data);
}

TEST(gzip_source, refuses_other_bytes_after_the_last_member)
{
    const std::vector<std::byte> data = content(5000);
    for (const std::vector<std::byte>& after :
         {openfits::test::bytes("garbage"), joined({std::vector<std::byte>(16), openfits::test::bytes("x")})}) {
        const std::vector<std::byte> stream = joined({gzip(data), after});
        EXPECT_TRUE(throws<integrity_error>(errc::corrupt_compressed_data, [&] {
            (void)gzip_source(memory_source{std::span<const std::byte>(stream)});
        }));
    }
}

TEST(gzip_source, refuses_a_member_cut_anywhere)
{
    const std::vector<std::byte> data = content(20'000);
    const std::vector<std::byte> stream = gzip(data);
    for (std::size_t size = 1; size < stream.size(); ++size) {
        const std::span<const std::byte> cut = std::span(stream).first(size);
        ASSERT_TRUE(throws<integrity_error>(errc::corrupt_compressed_data, [&] {
            (void)gzip_source(memory_source(cut));
        })) << size;
    }
}

TEST(gzip_source, refuses_a_member_that_does_not_match_its_crc_or_its_length)
{
    const std::vector<std::byte> data = content(20'000);
    for (const std::size_t from_end : {8U, 5U, 4U, 1U}) {
        std::vector<std::byte> stream = gzip(data);
        stream[stream.size() - from_end] ^= std::byte{0x01};
        try {
            (void)gzip_source(memory_source{std::span<const std::byte>(stream)});
            ADD_FAILURE() << "no exception";
        } catch (const integrity_error& failure) {
            EXPECT_EQ(failure.code(), errc::corrupt_compressed_data);
            // Where zlib stopped, in the trailer.
            EXPECT_GE(failure.context().offset.value_or(0), stream.size() - 8) << failure.what();
        }
    }
}

TEST(gzip_source, refuses_corrupt_deflate_data_and_sources_without_a_member)
{
    std::vector<std::byte> stream = gzip(pattern(1000));
    stream[12] = std::byte{0xFF};
    stream[13] = std::byte{0xFF};
    for (const std::vector<std::byte>& input :
         {stream, std::vector<std::byte>{}, openfits::test::bytes("SIMPLE  =                    T"),
          std::vector<std::byte>(100), std::vector<std::byte>{std::byte{0x1F}, std::byte{0x8B}}}) {
        EXPECT_TRUE(throws<integrity_error>(errc::corrupt_compressed_data, [&] {
            (void)gzip_source(memory_source{std::span<const std::byte>(input)});
        }));
    }
}

TEST(gzip_source, refuses_a_content_beyond_the_limits_in_memory)
{
    const std::vector<std::byte> data = content(100'000);
    const std::vector<std::byte> stream = gzip(data);
    const memory_source compressed{std::span<const std::byte>(stream)};
    EXPECT_TRUE(throws<limit_error>(errc::inflated_too_large,
                                    [&] { (void)gzip_source(compressed, {.max_inflated_bytes = data.size() - 1}); }));
    EXPECT_TRUE(throws<limit_error>(errc::allocation_too_large, [&] {
        (void)gzip_source(compressed, {.max_allocation = data.size() - 1, .max_inflated_bytes = 0});
    }));
    // Exactly at the limit.
    EXPECT_EQ(gzip_source(compressed, {.max_inflated_bytes = data.size()}).size(), data.size());
    // The size that the trailer gives says nothing: a member that claims less inflates to the limit all the same.
    const std::vector<std::byte> members = joined({stream, gzip(content(1))});
    EXPECT_TRUE(throws<limit_error>(errc::inflated_too_large, [&] {
        (void)gzip_source(memory_source{std::span<const std::byte>(members)}, {.max_inflated_bytes = data.size()});
    }));
}

// The temporary files that a directory shows while a source keeps one open: Windows removes the file when it is closed,
// and POSIX systems remove its name at once.
#ifdef _WIN32
constexpr std::size_t open_temporary_files = 1;
#else
constexpr std::size_t open_temporary_files = 0;
#endif

// The entries of a directory whose names start with .openfits-.
std::size_t temporary_files(const temp_directory& directory)
{
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory.path())) {
        if (openfits::test::utf8(entry.path().filename()).starts_with(".openfits-")) {
            ++count;
        }
    }
    return count;
}

TEST(gzip_source, takes_a_content_beyond_the_limit_to_a_temporary_file_in_the_directory_given)
{
    const temp_directory directory;
    const std::vector<std::byte> data = content(3'000'000);
    const std::vector<std::byte> stream = gzip(data);
    const memory_source compressed{std::span<const std::byte>(stream)};
    {
        const gzip_source source(compressed, {.max_inflated_bytes = 100'000},
                                 {.temporary_directory = openfits::test::utf8(directory.path())});
        EXPECT_EQ(temporary_files(directory), open_temporary_files);
        EXPECT_TRUE(source.bytes().empty());
        EXPECT_EQ(source.size(), data.size());
        EXPECT_EQ(read_all(source), data);
        std::array<std::byte, 100> piece{};
        source.read(2'999'900, piece);
        EXPECT_TRUE(std::ranges::equal(piece, std::span(data).last(100)));
        EXPECT_TRUE(throws<openfits::io_error>(errc::end_of_data, [&] { source.read(2'999'901, piece); }));
    }
    EXPECT_EQ(temporary_files(directory), 0U);
}

TEST(gzip_source, takes_a_content_beyond_the_threshold_to_a_temporary_file)
{
    const temp_directory directory;
    const std::string path = openfits::test::utf8(directory.path());
    const std::vector<std::byte> data = content(1'000'000);
    const std::vector<std::byte> stream = gzip(data);
    const memory_source compressed{std::span<const std::byte>(stream)};
    for (const std::uint64_t threshold : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{999'999}}) {
        const gzip_source source(compressed, {}, {.temporary_directory = path, .memory_threshold = threshold});
        EXPECT_TRUE(source.bytes().empty()) << threshold;
        EXPECT_EQ(read_all(source), data) << threshold;
    }
    // At the threshold, the content stays in memory.
    const gzip_source source(compressed, {}, {.temporary_directory = path, .memory_threshold = 1'000'000});
    EXPECT_EQ(source.bytes().size(), data.size());
    EXPECT_EQ(temporary_files(directory), 0U);
}

TEST(gzip_source, a_temporary_directory_that_cannot_hold_the_file_is_an_io_error)
{
    const temp_directory directory;
    const std::vector<std::byte> stream = gzip(content(1000));
    const memory_source compressed{std::span<const std::byte>(stream)};
    EXPECT_TRUE(throws<openfits::io_error>(errc::open_failed, [&] {
        (void)gzip_source(compressed, {}, {.temporary_directory = directory.file("missing"), .memory_threshold = 0});
    }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_utf8, [&] {
        (void)gzip_source(compressed, {}, {.temporary_directory = "\xFF", .memory_threshold = 0});
    }));
}

// The inflation of a gzip member with the header that the bytes of the stream give.
std::vector<std::byte> inflated(std::span<const std::byte> stream)
{
    return read_all(gzip_source(memory_source(stream)));
}

TEST(gzip_sink, compresses_what_is_written_into_one_member_of_the_target)
{
    const std::vector<std::byte> data = content(1'000'000);
    memory_sink target;
    gzip_sink sink(target, 6);
    sink.write(std::span(data).first(1000));
    sink.write({});
    sink.write(std::span(data).subspan(1000));
    EXPECT_EQ(sink.position(), data.size());
    EXPECT_FALSE(sink.can_rewrite());
    sink.finish();
    const std::span<const std::byte> stream = target.data();
    EXPECT_LT(stream.size(), data.size() / 10);
    EXPECT_EQ(inflated(stream), data);
    // No name and no time, and an unknown system (RFC 1952, §2.3.1), so that a file is the same on every host.
    ASSERT_GT(stream.size(), 10U);
    EXPECT_EQ(stream[0], std::byte{0x1F});
    EXPECT_EQ(stream[1], std::byte{0x8B});
    EXPECT_EQ(stream[3], std::byte{0});
    EXPECT_TRUE(std::ranges::all_of(stream.subspan(4, 4), [](std::byte value) { return value == std::byte{0}; }));
    EXPECT_EQ(stream[9], std::byte{0xFF});
}

TEST(gzip_sink, every_level_compresses_and_a_wrong_one_is_refused)
{
    const std::vector<std::byte> data = content(100'000);
    std::vector<std::byte> fastest;
    for (int level = -1; level <= 9; ++level) {
        memory_sink target;
        gzip_sink sink(target, level);
        sink.write(data);
        sink.finish();
        EXPECT_EQ(inflated(target.data()), data) << level;
        if (level == 1) {
            fastest.assign(target.data().begin(), target.data().end());
        }
        if (level == 0) {
            EXPECT_GT(target.data().size(), data.size());
        }
    }
    // The same bytes and level give the same member.
    memory_sink again;
    gzip_sink sink(again, 1);
    sink.write(data);
    sink.finish();
    EXPECT_TRUE(std::ranges::equal(again.data(), fastest));
    memory_sink target;
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { gzip_sink wrong(target, 10); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { gzip_sink wrong(target, -2); }));
}

TEST(gzip_sink, an_empty_output_is_an_empty_member_and_finish_finishes_the_target)
{
    bool finished = false;
    std::vector<std::byte> stream;
    openfits::callback_sink target(
        [&stream](std::span<const std::byte> data) { stream.insert(stream.end(), data.begin(), data.end()); }, {},
        [&finished] { finished = true; });
    gzip_sink sink(target);
    sink.finish();
    EXPECT_TRUE(finished);
    EXPECT_TRUE(inflated(stream).empty());
}

// A file of an image, which the tests compress whole.
openfits::writer image_file(const std::vector<std::int16_t>& pixels)
{
    openfits::writer output;
    openfits::image_info image;
    image.geometry = {.axes = {50, 40}};
    image.header.set("OBJECT", "M 31");
    (void)output.add_image(image, std::span(pixels));
    return output;
}

std::vector<std::int16_t> image_values()
{
    std::vector<std::int16_t> values(2000);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = static_cast<std::int16_t>((i * 37) % 1000);
    }
    return values;
}

TEST(reader, opens_a_file_compressed_with_gzip_by_its_first_bytes)
{
    const std::vector<std::int16_t> pixels = image_values();
    const openfits::writer output = image_file(pixels);
    const std::vector<std::byte> plain = openfits::test::written(output);
    const std::vector<std::byte> stream = gzip(plain);
    const openfits::reader file(std::make_unique<memory_source>(std::vector<std::byte>(stream.begin(), stream.end())));
    EXPECT_EQ(file.size(), plain.size());
    EXPECT_EQ(file.hdu(0).header().value<std::string>("OBJECT"), "M 31");
    EXPECT_EQ(file.read_pixels<std::int16_t>(0), pixels);

    // From a path, whatever its name, and with the options of the reader.
    const temp_directory directory;
    const std::string path = directory.file("image.fits");
    openfits::test::write_file(directory.path() / "image.fits", stream);
    EXPECT_EQ(openfits::reader(path).read_pixels<std::int16_t>(0), pixels);
    EXPECT_TRUE(throws<limit_error>(errc::inflated_too_large,
                                    [&] { (void)openfits::reader(path, {.limits = {.max_inflated_bytes = 2880}}); }));
    const openfits::reader spilled(path, {.limits = {.max_inflated_bytes = 2880},
                                          .inflate = {.temporary_directory = openfits::test::utf8(directory.path())}});
    EXPECT_EQ(spilled.read_pixels<std::int16_t>(0), pixels);
    EXPECT_EQ(temporary_files(directory), open_temporary_files);
}

TEST(reader, refuses_a_file_that_starts_as_gzip_and_is_not)
{
    std::vector<std::byte> stream = gzip(openfits::test::fixture().primary().bytes());
    stream.resize(stream.size() - 3);
    EXPECT_TRUE(throws<integrity_error>(errc::corrupt_compressed_data, [&] {
        (void)openfits::reader(std::make_unique<memory_source>(std::move(stream)));
    }));
}

TEST(writer, saves_a_file_compressed_with_gzip_at_the_level_given)
{
    const std::vector<std::int16_t> pixels = image_values();
    const openfits::writer output = image_file(pixels);
    const std::vector<std::byte> plain = openfits::test::written(output);
    for (const openfits::test::sink_kind kind :
         {openfits::test::sink_kind::rewritable, openfits::test::sink_kind::append_only}) {
        const std::vector<std::byte> stream = openfits::test::written(output, {.gzip_level = 1}, kind);
        ASSERT_GT(stream.size(), 2U);
        EXPECT_EQ(stream[0], std::byte{0x1F});
        EXPECT_EQ(inflated(stream), plain);
    }
    // Checksums of borrowed data are summed before the file is written.
    const std::vector<std::byte> summed = openfits::test::written(output, {.checksums = true, .gzip_level = 9});
    const openfits::reader file(std::make_unique<memory_source>(std::vector<std::byte>(summed.begin(), summed.end())));
    EXPECT_EQ(file.verify(0).datasum, openfits::checksum_state::valid);
    EXPECT_EQ(file.verify(0).checksum, openfits::checksum_state::valid);
    EXPECT_EQ(output.size({.gzip_level = 1}), plain.size());

    const temp_directory directory;
    const std::string path = directory.file("image.fits.gz");
    output.save(path, {.gzip_level = 6});
    EXPECT_EQ(openfits::reader(path).read_pixels<std::int16_t>(0), pixels);
    const std::string refused = directory.file("refused.fits.gz");
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { output.save(refused, {.gzip_level = 10}); }));
    memory_sink sink;
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { output.save(sink, {.gzip_level = -2}); }));
    EXPECT_EQ(sink.position(), 0U);
    EXPECT_EQ(directory.entries(), 1U);
}

TEST(writer, refuses_checksums_of_streamed_data_compressed_with_gzip)
{
    openfits::writer output;
    openfits::image_info image;
    image.geometry = {.axes = {10, 10}};
    (void)output.add_image<std::int16_t>(image, [](std::uint64_t first, std::span<std::int16_t> values) {
        for (std::size_t i = 0; i < values.size(); ++i) {
            values[i] = static_cast<std::int16_t>(first + i);
        }
    });
    memory_sink sink;
    EXPECT_TRUE(
        throws<usage_error>(errc::invalid_argument, [&] { output.save(sink, {.checksums = true, .gzip_level = 6}); }));
    EXPECT_EQ(sink.position(), 0U);
    // Without checksums, the streamed data are compressed as they come.
    output.save(sink, {.gzip_level = 6});
    const openfits::reader file(std::make_unique<memory_source>(sink.release()));
    EXPECT_EQ(file.read_pixels<std::int16_t>(0)[57], 57);
}

} // namespace
