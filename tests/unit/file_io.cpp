// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>
#include <openfits/io.h>

#include "io/native_file.h"
#include "support/bytes.h"
#include "support/faulty_io.h"
#include "support/files.h"
#include "support/links.h"
#include "support/sparse_file.h"
#include "support/temp_directory.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace {

using openfits::errc;
using openfits::file_sink;
using openfits::file_source;
using openfits::io_error;
using openfits::usage_error;
using openfits::detail::native_file;
using openfits::test::bytes;
using openfits::test::pattern;
using openfits::test::read_file;
using openfits::test::temp_directory;
using openfits::test::text;
using openfits::test::throws;
using openfits::test::utf8;
using openfits::test::working_directory;
using openfits::test::write_file;

// The UTF-8 names of the entries of a directory, sorted.
std::vector<std::string> entries(const std::filesystem::path& directory)
{
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        names.push_back(utf8(entry.path().filename()));
    }
    std::ranges::sort(names);
    return names;
}

std::vector<std::byte> read_all(const openfits::input_source& source)
{
    std::vector<std::byte> data(source.size());
    source.read(0, data);
    return data;
}

void write_through_sink(const std::string& path, std::string_view content)
{
    file_sink sink(path);
    sink.write(bytes(content));
    sink.finish();
}

#if defined(_WIN32)
// A named pipe of one instance, which the first program that opens it takes: a later open fails, with ERROR_PIPE_BUSY,
// so a test can tell that nothing opened it.
class named_pipe
{
public:
    named_pipe()
        : name_(R"(\\.\pipe\openfits-test-)" + std::to_string(GetCurrentProcessId()) + "-" +
                std::to_string(GetTickCount64()))
    {
        server_ = CreateNamedPipeW(wide().c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE, 1, 0, 0, 0, nullptr);
        if (server_ == INVALID_HANDLE_VALUE) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "cannot create a pipe");
        }
    }

    named_pipe(const named_pipe&) = delete;
    named_pipe& operator=(const named_pipe&) = delete;

    ~named_pipe()
    {
        (void)CloseHandle(server_);
    }

    [[nodiscard]] const std::string& name() const noexcept
    {
        return name_;
    }

    // True when the pipe can still be opened, and so was never opened before; the open takes it.
    [[nodiscard]] bool never_opened() const
    {
        HANDLE client = CreateFileW(wide().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (client == INVALID_HANDLE_VALUE) {
            return false;
        }
        (void)CloseHandle(client);
        return true;
    }

private:
    [[nodiscard]] std::wstring wide() const
    {
        return {name_.begin(), name_.end()};
    }

    std::string name_;
    HANDLE server_ = INVALID_HANDLE_VALUE;
};
#endif

TEST(file_source, reads_every_range_of_a_file)
{
    const temp_directory directory;
    const std::vector<std::byte> data = pattern(37);
    write_file(directory.path() / "data.bin", data);
    const file_source source(directory.file("data.bin"));

    ASSERT_EQ(source.size(), data.size());
    for (std::size_t offset = 0; offset <= data.size(); ++offset) {
        for (std::size_t length = 0; offset + length <= data.size(); ++length) {
            std::vector<std::byte> destination(length);
            source.read(offset, destination);
            ASSERT_TRUE(std::ranges::equal(destination, std::span(data).subspan(offset, length)))
                << offset << ' ' << length;
        }
    }
}

TEST(file_source, reads_past_the_end_are_end_of_data)
{
    const temp_directory directory;
    write_file(directory.path() / "data.bin", bytes("0123456789"));
    const file_source source(directory.file("data.bin"));
    std::array<std::byte, 4> destination{};

    EXPECT_TRUE(throws<io_error>(errc::end_of_data, [&] { source.read(7, destination); }));
    EXPECT_TRUE(throws<io_error>(errc::end_of_data, [&] { source.read(11, {}); }));
}

TEST(file_source, empty_file)
{
    const temp_directory directory;
    write_file(directory.path() / "empty.bin", {});
    const file_source source(directory.file("empty.bin"));
    std::array<std::byte, 1> destination{};

    EXPECT_EQ(source.size(), 0U);
    source.read(0, {});
    EXPECT_TRUE(throws<io_error>(errc::end_of_data, [&] { source.read(0, destination); }));
}

TEST(file_source, describes_itself_with_its_path_and_supports_concurrent_reads)
{
    const temp_directory directory;
    write_file(directory.path() / "data.bin", bytes("abc"));
    const std::string path = directory.file("data.bin");
    const file_source source(path);

    EXPECT_EQ(source.description(), path);
    EXPECT_TRUE(source.supports_concurrent_reads());
}

TEST(file_source, a_file_that_shrinks_after_opening_is_end_of_data)
{
    const temp_directory directory;
    write_file(directory.path() / "data.bin", pattern(100));
    const file_source source(directory.file("data.bin"));
    std::filesystem::resize_file(directory.path() / "data.bin", 50);
    std::array<std::byte, 20> destination{};

    source.read(30, destination);
    EXPECT_TRUE(throws<io_error>(errc::end_of_data, [&] { source.read(40, destination); }));
}

TEST(file_source, reads_beyond_4_gib)
{
    const temp_directory directory;
    const std::filesystem::path path = directory.path() / "large.bin";
    const std::uint64_t boundary = std::uint64_t{1} << 32;
    const std::uint64_t size = (std::uint64_t{5} << 30) + 3;
    if (!openfits::test::create_sparse_file(path, size)) {
        GTEST_SKIP() << "the file system cannot create sparse files";
    }
    openfits::test::write_at(path, boundary - 4, bytes("ABCDEFGH"));
    openfits::test::write_at(path, size - 3, bytes("xyz"));
    const file_source source(utf8(path));
    std::array<std::byte, 8> across{};
    std::array<std::byte, 4> beyond{};
    std::array<std::byte, 3> end{};

    EXPECT_EQ(source.size(), size);
    source.read(boundary - 4, across);
    source.read(boundary, beyond);
    source.read(size - 3, end);
    EXPECT_EQ(text(across), "ABCDEFGH");
    EXPECT_EQ(text(beyond), "EFGH");
    EXPECT_EQ(text(end), "xyz");
}

TEST(file_source, reads_more_than_1_gib_in_one_call)
{
    // Windows transfers at most 1 GiB in a call of the system, and POSIX systems may transfer less than asked for, so
    // one read of 1 GiB and a byte takes several.
    const temp_directory directory;
    const std::filesystem::path path = directory.path() / "large.bin";
    constexpr std::size_t gib = std::size_t{1} << 30;
    if (!openfits::test::create_sparse_file(path, gib + 1)) {
        GTEST_SKIP() << "the file system cannot create sparse files";
    }
    openfits::test::write_at(path, 0, bytes("AB"));
    openfits::test::write_at(path, gib - 2, bytes("CDE"));
    const file_source source(utf8(path));
    std::vector<std::byte> data(gib + 1, std::byte{0xEE});

    source.read(0, data);
    EXPECT_EQ(text(std::span(data).first(2)), "AB");
    EXPECT_EQ(text(std::span(data).last(3)), "CDE");
    // Every byte between them was read: the zeros of the file replaced the bytes of the buffer.
    EXPECT_TRUE(std::ranges::find(data, std::byte{0xEE}) == data.end());
}

// The loops of the platform layer move at most a transfer size in each call of the system, 1 GiB in the library: a
// transfer of a few bytes takes them through every turn, the last short one included, without a file of gigabytes.
TEST(native_file, reads_and_writes_in_calls_of_the_transfer_size)
{
    const temp_directory directory;
    write_file(directory.path() / "data.bin", std::vector<std::byte>(100));
    const native_file file = native_file::open_for_update(directory.file("data.bin"));
    std::uint8_t mark = 0;
    for (const std::size_t transfer : {std::size_t{1}, std::size_t{7}, std::size_t{99}, std::size_t{100},
                                       std::size_t{101}, openfits::detail::max_transfer}) {
        std::vector<std::byte> data = pattern(100);
        std::ranges::for_each(data, [&mark](std::byte& b) { b ^= static_cast<std::byte>(mark); });
        ++mark;
        file.write_at(0, data, transfer);
        std::vector<std::byte> read(100);
        file.read_at(0, read, transfer);
        EXPECT_EQ(read, data) << transfer;
        EXPECT_EQ(bytes(read_file(directory.path() / "data.bin")), data) << transfer;
    }
}

// The temporary file of an inflated gzip file: read and written, removed by the system once closed, and never made
// where something exists.
TEST(native_file, a_scratch_file_is_removed_once_it_is_closed)
{
    const temp_directory directory;
    const std::string path = directory.file("scratch.tmp");
    {
        std::optional<native_file> file = native_file::create_scratch(path);
        ASSERT_TRUE(file.has_value());
        if (file) {
            const std::vector<std::byte> data = pattern(5000);
            file->write_at(0, data);
            file->write_at(5000, data);
            EXPECT_EQ(file->size(), 10'000U);
            std::vector<std::byte> read(5000);
            file->read_at(5000, read);
            EXPECT_EQ(read, data);
        }
    }
    EXPECT_EQ(directory.entries(), 0U);
    write_file(directory.path() / "taken.tmp", pattern(10));
    EXPECT_FALSE(native_file::create_scratch(directory.file("taken.tmp")).has_value());
    EXPECT_EQ(bytes(read_file(directory.path() / "taken.tmp")), pattern(10));
    EXPECT_TRUE(throws<io_error>(errc::open_failed,
                                 [&] { (void)native_file::create_scratch(directory.file("missing/scratch.tmp")); }));
}

TEST(native_file, a_read_that_meets_the_end_in_a_later_turn_is_end_of_data_where_the_file_ends)
{
    // 30 bytes at 80 of a file of 100, 7 at a time: 80, 87, then a turn at 94 that gets 6 bytes, and one at 100 that
    // gets none.
    const temp_directory directory;
    const std::vector<std::byte> data = pattern(100);
    write_file(directory.path() / "data.bin", data);
    const native_file file = native_file::open_for_reading(directory.file("data.bin"));
    std::vector<std::byte> destination(30);
    try {
        file.read_at(80, destination, 7);
        ADD_FAILURE() << "no exception";
    } catch (const io_error& failure) {
        EXPECT_EQ(failure.code(), errc::end_of_data);
        EXPECT_EQ(failure.context().offset, 100U);
    }
    EXPECT_TRUE(std::ranges::equal(std::span(destination).first(20), std::span(data).subspan(80, 20)));
}

TEST(file_source, a_missing_file_is_open_failed)
{
    const temp_directory directory;

    try {
        const file_source source(directory.file("missing.fits"));
        ADD_FAILURE() << "no exception";
    } catch (const io_error& failure) {
        EXPECT_EQ(failure.code(), errc::open_failed);
        EXPECT_EQ(failure.system_code(), std::errc::no_such_file_or_directory);
        EXPECT_NE(std::string_view(failure.what()).find(directory.file("missing.fits")), std::string_view::npos);
    }
}

TEST(file_source, refuses_directories_and_devices)
{
    const temp_directory directory;
#if defined(_WIN32)
    const std::string device = "NUL";
#else
    const std::string device = "/dev/null";
#endif

    EXPECT_TRUE(throws<io_error>(errc::not_a_regular_file, [&] { file_source source(utf8(directory.path())); }));
    EXPECT_TRUE(throws<io_error>(errc::not_a_regular_file, [&] { file_source source(device); }));
}

#if defined(_WIN32)
TEST(file_source, refuses_the_device_names_of_windows_in_any_directory)
{
    // Such a name is refused before it is opened, as a device that cannot be opened is too, such as a serial port that
    // the machine does not have; one that Windows does not take for a device is a file like any other.
    const temp_directory directory;
    int devices = 0;
    for (const std::string_view name :
         {"NUL", "nul", "NUL.fits", "CON", "AUX", "PRN", "COM1", "COM9", "COM9.fits", "LPT1", "LPT9"}) {
        const std::string path = directory.file(name);
        if (openfits::test::is_device_name(path)) {
            ++devices;
            EXPECT_TRUE(throws<io_error>(errc::not_a_regular_file, [&path] { file_source source(path); })) << name;
        } else {
            EXPECT_TRUE(throws<io_error>(errc::open_failed, [&path] { file_source source(path); })) << name;
        }
    }
    EXPECT_GE(devices, 2);
}

TEST(file_source, refuses_a_device_of_the_device_namespace_before_it_opens_it)
{
    // A path of the device namespace that no file system follows names a device, refused by its text: a named pipe,
    // which an open would connect to, a serial port, the volume of a drive and a device of the system's namespace.
    const temp_directory directory;
    write_file(directory.path() / "image.fits", bytes("content"));
    const std::string local = directory.file("image.fits");
    const named_pipe pipe;
    for (const std::string& path : {pipe.name(), std::string(R"(\\.\COM1)"), std::string(R"(\\.\NUL)"),
                                    R"(\\.\)" + local.substr(0, 2), std::string(R"(\\?\GLOBALROOT\Device\Null)")}) {
        EXPECT_TRUE(throws<io_error>(errc::not_a_regular_file, [&path] { file_source source(path); })) << path;
    }
    EXPECT_TRUE(pipe.never_opened());
    // A drive after the prefix names a file, as without it.
    EXPECT_EQ(read_all(file_source(R"(\\.\)" + local)), bytes("content"));
}

TEST(file_source, refuses_the_root_of_a_drive_or_a_share)
{
    // The system refuses to open a root with other errors than for a directory, which it is.
    const temp_directory directory;
    const std::string local = utf8(directory.path());
    if (local.size() < 3 || local[1] != ':' || local[2] != '\\') {
        GTEST_SKIP() << "the temporary directory is not on a drive";
    }
    const std::string drive = local.substr(0, 3);
    for (const std::string& root : {drive, R"(\\?\)" + drive}) {
        EXPECT_TRUE(throws<io_error>(errc::not_a_regular_file, [&root] { file_source source(root); })) << root;
    }
    const std::string share = R"(\\localhost\)" + std::string(1, local[0]) + R"($\)";
    std::error_code error;
    if (std::filesystem::exists(openfits::test::path_of(share), error)) {
        for (const std::string& root : {share, R"(\\?\UNC\localhost\)" + std::string(1, local[0]) + R"($\)"}) {
            EXPECT_TRUE(throws<io_error>(errc::not_a_regular_file, [&root] { file_source source(root); })) << root;
        }
    }
}
#endif

TEST(file_source, refuses_empty_and_invalid_utf8_paths)
{
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [] { file_source source(""); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_utf8, [] { file_source source("a\xC3.fits"); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_utf8, [] { file_source source(std::string_view("a\0b", 3)); }));
}

// A file name, and the name of its test.
struct named_file
{
    std::string_view test{};
    std::string_view file{};
};

// GoogleTest prints the parameter in messages, and CTest names the test after it. The name is fixed by GoogleTest.
// NOLINTNEXTLINE(readability-identifier-naming)
void PrintTo(const named_file& value, std::ostream* out)
{
    *out << value.test;
}

class file_name : public testing::TestWithParam<named_file>
{};

// The name is checked through std::filesystem, so a path converted wrongly on the way to the system shows as a missing
// entry, not as a file that the library alone can find again.
TEST_P(file_name, is_written_and_read_under_its_name)
{
    const temp_directory directory;
    const std::string path = directory.file(GetParam().file);

    write_through_sink(path, "content");
    const file_source source(path);

    EXPECT_EQ(text(read_all(source)), "content");
    EXPECT_EQ(entries(directory.path()), std::vector<std::string>{std::string(GetParam().file)});
}

// Characters outside the BMP take surrogate pairs in UTF-16. Brackets are part of a name, which a path always is:
// nothing reads them as the extended file names of other libraries.
INSTANTIATE_TEST_SUITE_P(file_io, file_name,
                         testing::Values(named_file{.test = "latin", .file = "Nebulosa del Ñandú.fits"},
                                         named_file{.test = "cjk", .file = "星雲.fits"},
                                         named_file{.test = "outside_the_bmp", .file = "𝔛 😀.fits"},
                                         named_file{.test = "brackets", .file = "M31 [Ha].fits"},
                                         named_file{.test = "unit_selector", .file = "image.fits[SCI,2]"}),
                         [](const testing::TestParamInfo<named_file>& instance) {
                             return std::string(instance.param.test);
                         });

TEST(file_io, paths_longer_than_260_characters)
{
    const temp_directory directory;
    const std::filesystem::path folder = directory.path() / std::string(100, 'd') / std::string(100, 'e');
    std::filesystem::create_directory(openfits::test::long_form(folder.parent_path()));
    std::filesystem::create_directory(openfits::test::long_form(folder));
    const std::string path = utf8(folder / (std::string(100, 'f') + ".fits"));
    ASSERT_GT(path.size(), 300U);

    write_through_sink(path, "content");
    const file_source source(path);

    EXPECT_EQ(text(read_all(source)), "content");
}

TEST(file_sink, a_relative_target_is_taken_from_the_current_directory_when_the_sink_is_made)
{
    // The sink replaces the file that the path named when it was made, and leaves nothing in the current directory of
    // its finish().
    const temp_directory directory;
    std::filesystem::create_directory(directory.path() / "made");
    std::filesystem::create_directory(directory.path() / "finished");
    std::unique_ptr<file_sink> sink;
    {
        const working_directory moved(directory.path() / "made");
        sink = std::make_unique<file_sink>("out.fits");
    }
    const working_directory moved(directory.path() / "finished");
    sink->write(bytes("content"));
    sink->finish();
    EXPECT_EQ(entries(directory.path() / "made"), std::vector<std::string>{"out.fits"});
    EXPECT_EQ(read_file(directory.path() / "made" / "out.fits"), "content");
    EXPECT_TRUE(entries(directory.path() / "finished").empty());
}

TEST(file_io, relative_paths_longer_than_260_characters)
{
    // From the working directory, which the test changes while it runs.
    const temp_directory directory;
    const std::filesystem::path folder = directory.path() / std::string(100, 'd') / std::string(100, 'e');
    std::filesystem::create_directory(openfits::test::long_form(folder.parent_path()));
    std::filesystem::create_directory(openfits::test::long_form(folder));
    const working_directory moved(directory.path());
    const std::string path =
        std::string(100, 'd') + "/" + std::string(100, 'e') + "/" + std::string(100, 'f') + ".fits";

    write_through_sink(path, "content");
    const file_source source(path);

    EXPECT_EQ(text(read_all(source)), "content");
}

#if !defined(_WIN32)
TEST(file_io, relative_paths_from_a_current_directory_longer_than_256_bytes)
{
    // getcwd() gives the current directory only in a buffer that holds it, which grows from 256 bytes; Windows makes no
    // such directory current without a manifest for long paths.
    const temp_directory directory;
    const std::filesystem::path folder =
        directory.path() / std::string(100, 'd') / std::string(100, 'e') / std::string(100, 'g');
    std::filesystem::create_directories(folder);
    ASSERT_GT(utf8(folder).size(), 300U);
    const working_directory moved(folder);

    write_through_sink("image.fits", "content");

    EXPECT_EQ(read_file(folder / "image.fits"), "content");
}
#endif

#if defined(_WIN32)
TEST(file_io, network_paths)
{
    // The temporary directory through the administrative share of its drive, \\localhost\C$\..., and the same path in
    // the form that the system takes beyond 260 characters, \\?\UNC\localhost\C$\....
    const temp_directory directory;
    const std::string local = utf8(directory.path());
    if (local.size() < 3 || local[1] != ':' || local[2] != '\\') {
        GTEST_SKIP() << "the temporary directory is not on a drive";
    }
    // localhost\C$\Users\...
    const std::string share = R"(localhost\)" + std::string(1, local[0]) + "$" + local.substr(2);
    std::error_code error;
    if (!std::filesystem::exists(openfits::test::path_of(R"(\\)" + share), error)) {
        GTEST_SKIP() << "the administrative share of the drive is not available";
    }

    write_through_sink(R"(\\)" + share + R"(\unc.fits)", "through the share");
    EXPECT_EQ(read_file(directory.path() / "unc.fits"), "through the share");
    const file_source source(R"(\\?\UNC\)" + share + R"(\unc.fits)");
    EXPECT_EQ(text(read_all(source)), "through the share");
}
#endif

TEST(file_sink, creates_the_target_only_on_finish)
{
    const temp_directory directory;
    const std::string target = directory.file("image.fits");
    file_sink sink(target);
    sink.write(bytes("0123456789"));

    EXPECT_FALSE(std::filesystem::exists(directory.path() / "image.fits"));
    ASSERT_EQ(entries(directory.path()).size(), 1U);
    const std::string temporary = entries(directory.path())[0];
    EXPECT_TRUE(temporary.starts_with(".openfits-") && temporary.ends_with(".tmp") && temporary.size() == 30)
        << temporary;

    sink.finish();

    EXPECT_EQ(entries(directory.path()), std::vector<std::string>{"image.fits"});
    EXPECT_EQ(read_file(directory.path() / "image.fits"), "0123456789");
}

TEST(file_sink, rewrites_bytes_already_written)
{
    const temp_directory directory;
    file_sink sink(directory.file("image.fits"));
    EXPECT_TRUE(sink.can_rewrite());

    sink.write(bytes("0123456789"));
    sink.rewrite(2, bytes("ab"));
    sink.write(bytes("!"));
    EXPECT_EQ(sink.position(), 11U);
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { sink.rewrite(10, bytes("xy")); }));
    sink.finish();

    EXPECT_EQ(read_file(directory.path() / "image.fits"), "01ab456789!");
}

// Zeros make the file longer, which the system fills with them, and the bytes after them follow; they can be
// rewritten as any others.
TEST(file_sink, makes_its_file_longer_for_zeros)
{
    const temp_directory directory;
    file_sink sink(directory.file("zeros.bin"));
    sink.write(bytes("ab"));
    sink.write_zeros(100000);
    sink.write(bytes("z"));
    sink.write_zeros(0);
    EXPECT_EQ(sink.position(), 100003U);
    sink.rewrite(50000, bytes("m"));
    sink.finish();
    const std::string content = read_file(directory.path() / "zeros.bin");
    ASSERT_EQ(content.size(), 100003U);
    EXPECT_EQ(content.substr(0, 2), "ab");
    EXPECT_EQ(content.back(), 'z');
    EXPECT_EQ(content[50000], 'm');
    EXPECT_EQ(std::ranges::count(content, '\0'), 99999);
    // A finished sink takes nothing more.
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { sink.write_zeros(1); }));
}

TEST(file_sink, a_sink_destroyed_before_finish_leaves_the_target_as_it_was)
{
    const temp_directory directory;
    write_file(directory.path() / "old.fits", bytes("old content"));
    {
        file_sink replacing(directory.file("old.fits"));
        file_sink creating(directory.file("new.fits"));
        replacing.write(bytes("new content"));
        creating.write(bytes("new content"));
    }

    EXPECT_EQ(entries(directory.path()), std::vector<std::string>{"old.fits"});
    EXPECT_EQ(read_file(directory.path() / "old.fits"), "old content");
}

// The pattern of a writer that fails part of the way through a file.
TEST(file_sink, a_write_that_fails_part_of_the_way_leaves_the_target_as_it_was)
{
    const temp_directory directory;
    write_file(directory.path() / "image.fits", bytes("old content"));
    for (const openfits::test::fault kind : {openfits::test::fault::error, openfits::test::fault::short_transfer,
                                             openfits::test::fault::foreign_exception}) {
        {
            file_sink sink(directory.file("image.fits"));
            openfits::test::faulty_sink faulty(sink, 3, kind);
            EXPECT_ANY_THROW({
                for (int block = 0; block < 5; ++block) {
                    faulty.write(pattern(2880));
                }
                faulty.finish();
            });
        }

        EXPECT_EQ(entries(directory.path()), std::vector<std::string>{"image.fits"});
        EXPECT_EQ(read_file(directory.path() / "image.fits"), "old content");
    }
}

TEST(file_sink, a_finish_that_fails_removes_the_temporary_file)
{
    // A name longer than the file system takes, 255 characters or bytes, which only the rename finds out: the
    // temporary file has a short one.
    const temp_directory directory;
    file_sink sink(directory.file(std::string(300, 'a')));
    sink.write(bytes("content"));

    EXPECT_TRUE(throws<io_error>(errc::write_failed, [&] { sink.finish(); }));
    EXPECT_EQ(directory.entries(), 0U);
}

TEST(file_sink, nothing_can_be_written_after_finish)
{
    const temp_directory directory;
    file_sink sink(directory.file("image.fits"));
    sink.write(bytes("abc"));
    sink.finish();

    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { sink.write(bytes("d")); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { sink.rewrite(0, bytes("d")); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { sink.finish(); }));
    EXPECT_EQ(read_file(directory.path() / "image.fits"), "abc");
}

TEST(file_sink, flushes_to_disk_when_asked)
{
    const temp_directory directory;
    file_sink sink(directory.file("image.fits"), {.flush_to_disk = true});
    sink.write(bytes("durable"));
    sink.finish();

    EXPECT_EQ(read_file(directory.path() / "image.fits"), "durable");
}

// A file read, changed and saved under the same path.
TEST(file_sink, replaces_a_file_that_a_source_has_open)
{
    const temp_directory directory;
    const std::string path = directory.file("image.fits");
    write_file(directory.path() / "image.fits", bytes("old content"));
    const file_source source(path);

    write_through_sink(path, "new content");

    EXPECT_EQ(text(read_all(source)), "old content");
    EXPECT_EQ(read_file(directory.path() / "image.fits"), "new content");
}

TEST(file_sink, replaces_a_symbolic_link_itself)
{
    const temp_directory directory;
    write_file(directory.path() / "real.fits", bytes("old content"));
    if (!openfits::test::create_symbolic_link(directory.path() / "real.fits", directory.path() / "link.fits")) {
        GTEST_SKIP() << "the system does not let this process create symbolic links";
    }

    write_through_sink(directory.file("link.fits"), "new content");

    EXPECT_FALSE(std::filesystem::is_symlink(directory.path() / "link.fits"));
    EXPECT_EQ(read_file(directory.path() / "link.fits"), "new content");
    EXPECT_EQ(read_file(directory.path() / "real.fits"), "old content");
}

TEST(file_sink, replaces_a_read_only_target_except_on_windows)
{
    const temp_directory directory;
    const std::filesystem::path target = directory.path() / "image.fits";
    write_file(target, bytes("old content"));
    constexpr std::filesystem::perms writable = std::filesystem::perms::owner_write |
                                                std::filesystem::perms::group_write |
                                                std::filesystem::perms::others_write;
    std::filesystem::permissions(target, writable, std::filesystem::perm_options::remove);

#if defined(_WIN32)
    // The read-only attribute keeps a file from being replaced, as it keeps it from being deleted.
    file_sink sink(directory.file("image.fits"));
    sink.write(bytes("new content"));
    EXPECT_TRUE(throws<io_error>(errc::write_failed, [&] { sink.finish(); }));
    EXPECT_EQ(read_file(target), "old content");
    std::filesystem::permissions(target, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
#else
    // Replacing a file takes write access to its directory, not to the file.
    write_through_sink(directory.file("image.fits"), "new content");
    EXPECT_EQ(read_file(target), "new content");
#endif
    EXPECT_EQ(entries(directory.path()), std::vector<std::string>{"image.fits"});
}

TEST(file_sink, two_sinks_of_one_target_replace_it_in_turn)
{
    const temp_directory directory;
    file_sink first(directory.file("image.fits"));
    file_sink second(directory.file("image.fits"));
    first.write(bytes("first"));
    second.write(bytes("second"));

    first.finish();
    EXPECT_EQ(read_file(directory.path() / "image.fits"), "first");
    second.finish();
    EXPECT_EQ(read_file(directory.path() / "image.fits"), "second");
    EXPECT_EQ(entries(directory.path()), std::vector<std::string>{"image.fits"});
}

TEST(file_sink, that_may_not_replace_creates_a_target_that_does_not_exist)
{
    const temp_directory directory;
    file_sink sink(directory.file("image.fits"), {.replace = false});
    sink.write(bytes("new content"));
    sink.finish();

    EXPECT_EQ(read_file(directory.path() / "image.fits"), "new content");
    EXPECT_EQ(entries(directory.path()), std::vector<std::string>{"image.fits"});
}

TEST(file_sink, that_may_not_replace_refuses_an_existing_target_before_it_creates_a_file)
{
    const temp_directory directory;
    write_file(directory.path() / "image.fits", bytes("old content"));

    EXPECT_TRUE(
        throws<io_error>(errc::file_exists, [&] { file_sink sink(directory.file("image.fits"), {.replace = false}); }));
    EXPECT_EQ(read_file(directory.path() / "image.fits"), "old content");
    EXPECT_EQ(entries(directory.path()), std::vector<std::string>{"image.fits"});
}

// Another program creates the file while the sink writes: the rename of finish() never replaces it.
TEST(file_sink, that_may_not_replace_leaves_a_target_created_meanwhile)
{
    const temp_directory directory;
    file_sink sink(directory.file("image.fits"), {.replace = false});
    sink.write(bytes("new content"));
    write_file(directory.path() / "image.fits", bytes("another program's"));

    EXPECT_TRUE(throws<io_error>(errc::file_exists, [&] { sink.finish(); }));
    EXPECT_EQ(read_file(directory.path() / "image.fits"), "another program's");
    EXPECT_EQ(entries(directory.path()), std::vector<std::string>{"image.fits"});
}

TEST(file_sink, that_may_not_replace_refuses_a_link_whose_target_does_not_exist)
{
    const temp_directory directory;
    if (!openfits::test::create_symbolic_link(directory.path() / "missing.fits", directory.path() / "link.fits")) {
        GTEST_SKIP() << "the system does not let this process create symbolic links";
    }

    EXPECT_TRUE(
        throws<io_error>(errc::file_exists, [&] { file_sink sink(directory.file("link.fits"), {.replace = false}); }));
    EXPECT_FALSE(std::filesystem::exists(directory.path() / "missing.fits"));
}

#if defined(_WIN32)
TEST(file_sink, that_may_not_replace_refuses_a_junction)
{
    const temp_directory directory;
    std::filesystem::create_directory(directory.path() / "real");
    openfits::test::create_junction(directory.path() / "real", directory.path() / "image.fits");

    EXPECT_TRUE(
        throws<io_error>(errc::file_exists, [&] { file_sink sink(directory.file("image.fits"), {.replace = false}); }));
    EXPECT_TRUE(std::filesystem::is_directory(directory.path() / "real"));
}
#endif

// A file_sink writes into a temporary file of a random name, which is created only where nothing exists, so that a
// file of that name is not truncated, nor a link followed; the sink tries another name then.
TEST(native_file, creates_a_file_only_where_nothing_exists)
{
    const temp_directory directory;
    write_file(directory.path() / "taken", bytes("old content"));
    EXPECT_FALSE(native_file::create_new(directory.file("taken"), "image.fits").has_value());
    EXPECT_EQ(read_file(directory.path() / "taken"), "old content");

    // A link to a file, and a link to nothing, through which a file would be created.
    if (openfits::test::create_symbolic_link(directory.path() / "taken", directory.path() / "link")) {
        EXPECT_FALSE(native_file::create_new(directory.file("link"), "image.fits").has_value());
        EXPECT_EQ(read_file(directory.path() / "taken"), "old content");
        ASSERT_TRUE(openfits::test::create_symbolic_link(directory.path() / "missing", directory.path() / "dangling"));
        EXPECT_FALSE(native_file::create_new(directory.file("dangling"), "image.fits").has_value());
        EXPECT_FALSE(std::filesystem::exists(directory.path() / "missing"));
    }
    // A directory, and on Windows a junction to nothing, which every user can create there, unlike a symbolic link.
    std::filesystem::create_directory(directory.path() / "folder");
    EXPECT_FALSE(native_file::create_new(directory.file("folder"), "image.fits").has_value());
#if defined(_WIN32)
    openfits::test::create_junction(directory.path() / "nowhere", directory.path() / "junction");
    EXPECT_FALSE(native_file::create_new(directory.file("junction"), "image.fits").has_value());
    EXPECT_FALSE(std::filesystem::exists(directory.path() / "nowhere"));
#endif

    std::optional<native_file> created = native_file::create_new(directory.file("new"), "image.fits");
    if (created) {
        created->discard(directory.file("new"));
    } else {
        ADD_FAILURE() << "no file was created where nothing exists";
    }
}

TEST(file_sink, refuses_a_directory_before_it_creates_a_file)
{
    // No rename replaces a directory, which finish() found only once the whole file was written.
    const temp_directory directory;
    std::filesystem::create_directory(directory.path() / "target");
    write_file(directory.path() / "target" / "inside.txt", bytes("kept"));

    for (const std::string& target : {directory.file("target"), directory.file("target") + "/"}) {
        EXPECT_TRUE(throws<io_error>(errc::not_a_regular_file, [&target] { file_sink sink(target); })) << target;
    }
    EXPECT_EQ(entries(directory.path()), std::vector<std::string>{"target"});
    EXPECT_EQ(entries(directory.path() / "target"), std::vector<std::string>{"inside.txt"});
}

TEST(file_sink, refuses_a_device_and_a_fifo)
{
    const temp_directory directory;
#if defined(_WIN32)
    // Windows makes a device name, in any directory, a path of its device namespace (\\.\NUL), where no temporary file
    // can be created.
    for (const std::string& device : {std::string("NUL"), directory.file("NUL")}) {
        EXPECT_TRUE(throws<io_error>(errc::not_a_regular_file, [&device] { file_sink sink(device); })) << device;
    }
    EXPECT_EQ(directory.entries(), 0U);
#else
    // A rename replaces them: a FIFO named like the file, and the node of a device, which root can replace.
    const std::string fifo = directory.file("image.fits");
    ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
    EXPECT_TRUE(throws<io_error>(errc::not_a_regular_file, [&fifo] { file_sink sink(fifo); }));
    EXPECT_TRUE(std::filesystem::is_fifo(std::filesystem::symlink_status(directory.path() / "image.fits")));
    EXPECT_EQ(entries(directory.path()), std::vector<std::string>{"image.fits"});

    EXPECT_TRUE(throws<io_error>(errc::not_a_regular_file, [] { file_sink sink("/dev/null"); }));
    EXPECT_TRUE(std::filesystem::is_character_file(std::filesystem::symlink_status("/dev/null")));
#endif
}

#if defined(_WIN32)
TEST(file_sink, refuses_a_device_of_the_device_namespace_before_it_opens_it)
{
    const named_pipe pipe;
    EXPECT_TRUE(throws<io_error>(errc::not_a_regular_file, [&pipe] { file_sink sink(pipe.name()); }));
    EXPECT_TRUE(pipe.never_opened());
}

TEST(file_sink, refuses_a_directory_that_is_a_reparse_point_of_another_kind_than_a_link)
{
    // Such as the placeholder of a cloud file system: only a symbolic link or a junction is replaced itself.
    const temp_directory directory;
    openfits::test::create_foreign_reparse_point(directory.path() / "placeholder");
    EXPECT_TRUE(
        throws<io_error>(errc::not_a_regular_file, [&directory] { file_sink sink(directory.file("placeholder")); }));
    EXPECT_EQ(entries(directory.path()), std::vector<std::string>{"placeholder"});
}
#endif

TEST(file_sink, looks_at_its_target_again_before_it_replaces_it)
{
    // Another program can change the target while the file is written: a directory made meanwhile is refused, where
    // the rename would fail, and so is a FIFO, which the rename would replace.
    const temp_directory directory;
    file_sink sink(directory.file("image.fits"));
    sink.write(bytes("image"));
    std::filesystem::create_directory(directory.path() / "image.fits");
    write_file(directory.path() / "image.fits" / "inside.txt", bytes("kept"));
    EXPECT_TRUE(throws<io_error>(errc::not_a_regular_file, [&sink] { sink.finish(); }));
    EXPECT_EQ(entries(directory.path()), std::vector<std::string>{"image.fits"});
    EXPECT_EQ(read_file(directory.path() / "image.fits" / "inside.txt"), "kept");
#if !defined(_WIN32)
    file_sink second(directory.file("fifo.fits"));
    ASSERT_EQ(::mkfifo(directory.file("fifo.fits").c_str(), 0600), 0);
    EXPECT_TRUE(throws<io_error>(errc::not_a_regular_file, [&second] { second.finish(); }));
    EXPECT_TRUE(std::filesystem::is_fifo(std::filesystem::symlink_status(directory.path() / "fifo.fits")));
    EXPECT_EQ(entries(directory.path()), (std::vector<std::string>{"fifo.fits", "image.fits"}));
#endif
}

TEST(file_sink, replaces_a_link_to_a_directory_itself)
{
    // The target is looked at without following a link, which the new file replaces, as it replaces a link to a file.
    const temp_directory directory;
    std::filesystem::create_directory(directory.path() / "real");
#if defined(_WIN32)
    // Windows lets any user create a junction, and a symbolic link only with a privilege.
    openfits::test::create_junction(directory.path() / "real", directory.path() / "link");
#else
    ASSERT_TRUE(openfits::test::create_symbolic_link(directory.path() / "real", directory.path() / "link"));
#endif

    write_through_sink(directory.file("link"), "new content");

    EXPECT_EQ(read_file(directory.path() / "link"), "new content");
    EXPECT_EQ(entries(directory.path()), (std::vector<std::string>{"link", "real"}));
    EXPECT_TRUE(entries(directory.path() / "real").empty());
}

TEST(file_sink, a_missing_directory_is_open_failed)
{
    const temp_directory directory;

    EXPECT_TRUE(throws<io_error>(errc::open_failed, [&] { file_sink sink(directory.file("missing/image.fits")); }));
}

TEST(file_sink, refuses_empty_and_invalid_utf8_paths)
{
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [] { file_sink sink(""); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_utf8, [] { file_sink sink("a\xC3.fits"); }));
}

} // namespace
