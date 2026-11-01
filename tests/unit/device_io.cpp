// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The devices, read and written in place, and the checks that the library makes before a device sees a call.

#include <openfits/error.h>
#include <openfits/io.h>

#include "io/thread_safe_device.h"
#include "support/bytes.h"
#include "support/faulty_io.h"
#include "support/files.h"
#include "support/sparse_file.h"
#include "support/temp_directory.h"
#include "support/threads.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/resource.h>
#include <unistd.h>

#include <csignal>
#endif

namespace {

using openfits::errc;
using openfits::file_device;
using openfits::io_error;
using openfits::memory_device;
using openfits::usage_error;
using openfits::detail::thread_safe_device;
using openfits::test::bytes;
using openfits::test::pattern;
using openfits::test::read_file;
using openfits::test::temp_directory;
using openfits::test::text;
using openfits::test::throws;
using openfits::test::utf8;
using openfits::test::write_file;

// Writes every range of device, each with bytes of its own, and reads it back; expected holds the content of the
// device, which it follows.
void write_and_read_every_range(openfits::random_access_device& device, std::vector<std::byte>& expected)
{
    std::uint8_t mark = 0;
    for (std::size_t offset = 0; offset <= expected.size(); ++offset) {
        for (std::size_t length = 0; offset + length <= expected.size(); ++length) {
            const std::vector<std::byte> data(length, static_cast<std::byte>(++mark));
            device.write(offset, data);
            std::ranges::copy(data, expected.begin() + static_cast<std::ptrdiff_t>(offset));
            std::vector<std::byte> read(length);
            device.read(offset, read);
            ASSERT_EQ(read, data) << offset << ' ' << length;
        }
    }
    std::vector<std::byte> whole(expected.size());
    device.read(0, whole);
    EXPECT_EQ(whole, expected);
}

// The whole content of a device.
std::vector<std::byte> content_of(const openfits::random_access_device& device)
{
    std::vector<std::byte> content(device.size());
    device.read(0, content);
    return content;
}

// The bytes of text followed by zeros, size bytes in all.
std::vector<std::byte> zero_padded(std::string_view text, std::size_t size)
{
    std::vector<std::byte> padded = bytes(text);
    padded.resize(size);
    return padded;
}

// Grows device from the 10 bytes of "0123456789" to 16, writes the bytes it added, and cuts it to 4: the bytes added
// read as zeros, and every check follows the size.
void resize_both_ways(openfits::random_access_device& device)
{
    ASSERT_TRUE(device.can_resize());
    ASSERT_EQ(text(content_of(device)), "0123456789");

    device.resize(16);
    EXPECT_EQ(device.size(), 16U);
    EXPECT_EQ(content_of(device), zero_padded("0123456789", 16));
    device.write(10, bytes("abcdef"));
    EXPECT_EQ(text(content_of(device)), "0123456789abcdef");

    device.resize(4);
    std::vector<std::byte> destination(2);
    EXPECT_EQ(text(content_of(device)), "0123");
    EXPECT_TRUE(throws<io_error>(errc::end_of_data, [&] { device.read(3, destination); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { device.write(3, bytes("xy")); }));

    // Growing again does not bring the bytes that the cut dropped back.
    device.resize(6);
    EXPECT_EQ(content_of(device), zero_padded("0123", 6));
    device.resize(0);
    EXPECT_EQ(device.size(), 0U);
}

TEST(file_device, reads_and_writes_every_range_of_a_file)
{
    const temp_directory directory;
    std::vector<std::byte> expected = pattern(37);
    write_file(directory.path() / "data.bin", expected);
    {
        file_device device(directory.file("data.bin"));
        ASSERT_EQ(device.size(), expected.size());
        write_and_read_every_range(device, expected);
    }
    EXPECT_EQ(bytes(read_file(directory.path() / "data.bin")), expected);
}

TEST(file_device, writes_past_the_end_are_refused_before_the_file_sees_them)
{
    const temp_directory directory;
    write_file(directory.path() / "data.bin", bytes("0123456789"));
    file_device device(directory.file("data.bin"));
    const std::vector<std::byte> data = bytes("abcd");

    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { device.write(7, data); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { device.write(11, {}); }));
    device.write(10, {});
    EXPECT_EQ(read_file(directory.path() / "data.bin"), "0123456789");
    EXPECT_EQ(device.size(), 10U);
}

TEST(file_device, reads_past_the_end_are_end_of_data)
{
    const temp_directory directory;
    write_file(directory.path() / "data.bin", bytes("0123456789"));
    const file_device device(directory.file("data.bin"));
    std::vector<std::byte> destination(4);

    EXPECT_TRUE(throws<io_error>(errc::end_of_data, [&] { device.read(7, destination); }));
    EXPECT_TRUE(throws<io_error>(errc::end_of_data, [&] { device.read(11, {}); }));
}

TEST(file_device, describes_itself_with_its_path_and_supports_concurrent_access_and_resizing)
{
    const temp_directory directory;
    write_file(directory.path() / "data.bin", bytes("0123456789"));
    const file_device device(directory.file("data.bin"));

    EXPECT_EQ(device.description(), directory.file("data.bin"));
    EXPECT_TRUE(device.supports_concurrent_reads());
    EXPECT_TRUE(device.supports_concurrent_writes());
    EXPECT_TRUE(device.can_resize());
}

TEST(file_device, resizes_the_file_both_ways)
{
    const temp_directory directory;
    const std::filesystem::path path = directory.path() / "data.bin";
    write_file(path, bytes("0123456789"));
    {
        file_device device(directory.file("data.bin"));
        resize_both_ways(device);
        device.resize(5);
        device.write(0, bytes("ABCDE"));
        EXPECT_EQ(openfits::test::size_of_file(path), 5U);
    }
    EXPECT_EQ(read_file(path), "ABCDE");
}

TEST(file_device, grows_beyond_4_gib)
{
    // The size of an extended file takes 64 bits on the way to the system. The file is sparse, so that the bytes before
    // the last ones take no space: NTFS writes the zeros of a gap that a write leaves in a file that is not.
    const temp_directory directory;
    const std::filesystem::path path = directory.path() / "data.bin";
    if (!openfits::test::create_sparse_file(path, 10)) {
        GTEST_SKIP() << "the file system cannot create sparse files";
    }
    openfits::test::write_at(path, 0, bytes("0123456789"));
    file_device device(directory.file("data.bin"));
    const std::uint64_t size = (std::uint64_t{5} << 30) + 3;
    try {
        device.resize(size);
    } catch (const io_error& failure) {
        GTEST_SKIP() << "the file system cannot extend a file to 5 GiB: " << failure.what();
    }
    device.write(size - 3, bytes("xyz"));
    std::vector<std::byte> end(5);
    device.read(size - 5, end);
    EXPECT_EQ(device.size(), size);
    EXPECT_EQ(openfits::test::size_of_file(path), size);
    EXPECT_EQ(text(end), std::string("\0\0xyz", 5));
    device.resize(10);
    EXPECT_EQ(text(content_of(device)), "0123456789");
}

#if defined(_WIN32)
// A view of a file mapped into memory through a handle of its own, released on destruction.
class mapped_view
{
public:
    explicit mapped_view(const std::filesystem::path& path)
        : file_(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr))
    {
        if (file_ == INVALID_HANDLE_VALUE) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "cannot open the file");
        }
        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        view_ = mapping_ == nullptr ? nullptr : MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0);
        if (view_ == nullptr) {
            const auto error = static_cast<int>(GetLastError());
            release();
            throw std::system_error(error, std::system_category(), "cannot map the file");
        }
    }

    mapped_view(const mapped_view&) = delete;
    mapped_view& operator=(const mapped_view&) = delete;

    ~mapped_view()
    {
        release();
    }

private:
    void release() noexcept
    {
        if (view_ != nullptr) {
            (void)UnmapViewOfFile(view_);
        }
        if (mapping_ != nullptr) {
            (void)CloseHandle(mapping_);
        }
        (void)CloseHandle(file_);
    }

    HANDLE file_;
    HANDLE mapping_ = nullptr;
    void* view_ = nullptr;
};

TEST(file_device, a_size_that_the_system_refuses_is_write_failed_and_the_file_keeps_its_size)
{
    // Windows refuses to cut a file that a view maps into memory (ERROR_USER_MAPPED_FILE).
    const temp_directory directory;
    const std::filesystem::path path = directory.path() / "data.bin";
    write_file(path, pattern(100));
    file_device device(directory.file("data.bin"));
    {
        const mapped_view view(path);
        EXPECT_TRUE(throws<io_error>(errc::write_failed, [&] { device.resize(50); }));
    }
    EXPECT_EQ(device.size(), 100U);
    EXPECT_EQ(openfits::test::size_of_file(path), 100U);
    EXPECT_EQ(content_of(device), pattern(100));
}
#else
// A limit of the size of the files of the process (RLIMIT_FSIZE), with its signal ignored, so that a system call that
// would pass it fails with EFBIG; both restored on destruction. Some systems, such as Cygwin, set no such limit.
class file_size_limit
{
public:
    explicit file_size_limit(rlim_t size)
    {
        if (::getrlimit(RLIMIT_FSIZE, &previous_) != 0) {
            return;
        }
        previous_handler_ = std::signal(SIGXFSZ, SIG_IGN);
        rlimit lowered = previous_;
        lowered.rlim_cur = size;
        active_ = ::setrlimit(RLIMIT_FSIZE, &lowered) == 0;
        if (!active_) {
            (void)std::signal(SIGXFSZ, previous_handler_);
        }
    }

    file_size_limit(const file_size_limit&) = delete;
    file_size_limit& operator=(const file_size_limit&) = delete;

    ~file_size_limit()
    {
        if (active_) {
            (void)::setrlimit(RLIMIT_FSIZE, &previous_);
            (void)std::signal(SIGXFSZ, previous_handler_);
        }
    }

    // True when the limit is in force.
    [[nodiscard]] bool active() const noexcept
    {
        return active_;
    }

private:
    rlimit previous_{};
    void (*previous_handler_)(int) = nullptr;
    bool active_ = false;
};

TEST(file_device, a_size_that_the_system_refuses_is_write_failed_and_the_file_keeps_its_size)
{
    const temp_directory directory;
    const std::filesystem::path path = directory.path() / "data.bin";
    write_file(path, pattern(100));
    file_device device(directory.file("data.bin"));
    {
        const file_size_limit limit(1000);
        if (!limit.active()) {
            GTEST_SKIP() << "the system sets no limit of the size of files";
        }
        try {
            device.resize(2000);
            ADD_FAILURE() << "no exception";
        } catch (const io_error& failure) {
            EXPECT_EQ(failure.code(), errc::write_failed);
            EXPECT_EQ(failure.system_code(), std::errc::file_too_large);
        }
    }
    EXPECT_EQ(device.size(), 100U);
    EXPECT_EQ(openfits::test::size_of_file(path), 100U);
    EXPECT_EQ(content_of(device), pattern(100));
}
#endif

TEST(file_device, a_missing_file_is_open_failed_and_is_not_created)
{
    const temp_directory directory;

    try {
        const file_device device(directory.file("missing.fits"));
        ADD_FAILURE() << "no exception";
    } catch (const io_error& failure) {
        EXPECT_EQ(failure.code(), errc::open_failed);
        EXPECT_EQ(failure.system_code(), std::errc::no_such_file_or_directory);
        EXPECT_NE(std::string_view(failure.what()).find(directory.file("missing.fits")), std::string_view::npos);
    }
    EXPECT_EQ(directory.entries(), 0U);
}

TEST(file_device, refuses_directories_and_devices)
{
    const temp_directory directory;
#if defined(_WIN32)
    const std::string device = "NUL";
#else
    const std::string device = "/dev/null";
#endif

    EXPECT_TRUE(throws<io_error>(errc::not_a_regular_file, [&] { file_device opened(utf8(directory.path())); }));
    EXPECT_TRUE(throws<io_error>(errc::not_a_regular_file, [&] { file_device opened(device); }));
}

#if defined(_WIN32)
TEST(file_device, refuses_the_device_names_of_windows_in_any_directory)
{
    // As file_source refuses them, before they are opened.
    const temp_directory directory;
    for (const std::string_view name : {"NUL", "nul.fits", "CON", "COM9", "LPT1"}) {
        const std::string path = directory.file(name);
        if (openfits::test::is_device_name(path)) {
            EXPECT_TRUE(throws<io_error>(errc::not_a_regular_file, [&path] { file_device opened(path); })) << name;
        } else {
            EXPECT_TRUE(throws<io_error>(errc::open_failed, [&path] { file_device opened(path); })) << name;
        }
    }
}
#endif

TEST(file_device, a_file_that_cannot_be_written_is_open_failed)
{
#if !defined(_WIN32)
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root may write any file";
    }
#endif
    const temp_directory directory;
    const std::filesystem::path path = directory.path() / "data.bin";
    write_file(path, bytes("0123456789"));
    constexpr std::filesystem::perms writable = std::filesystem::perms::owner_write |
                                                std::filesystem::perms::group_write |
                                                std::filesystem::perms::others_write;
    std::filesystem::permissions(path, writable, std::filesystem::perm_options::remove);

    // On Windows, the read-only attribute refuses write access.
    EXPECT_TRUE(throws<io_error>(errc::open_failed, [&] { file_device device(directory.file("data.bin")); }));
    std::filesystem::permissions(path, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
    EXPECT_EQ(read_file(path), "0123456789");
}

TEST(file_device, refuses_empty_and_invalid_utf8_paths)
{
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [] { file_device device(""); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_utf8, [] { file_device device("a\xC3.fits"); }));
}

TEST(file_device, flushes_what_it_wrote_to_the_storage_device)
{
    const temp_directory directory;
    write_file(directory.path() / "data.bin", bytes("0123456789"));
    {
        file_device device(directory.file("data.bin"));
        device.write(2, bytes("ab"));
        device.flush();
        device.write(8, bytes("yz"));
        device.flush();
    }
    EXPECT_EQ(read_file(directory.path() / "data.bin"), "01ab4567yz");
}

TEST(file_device, other_programs_can_read_and_write_the_file_while_it_is_open)
{
    // As a reader of the same file does, before and after the device opens it.
    const temp_directory directory;
    write_file(directory.path() / "data.bin", bytes("0123456789"));
    const openfits::file_source before(directory.file("data.bin"));
    file_device device(directory.file("data.bin"));
    const openfits::file_source after(directory.file("data.bin"));
    file_device other(directory.file("data.bin"));

    device.write(0, bytes("ab"));
    other.write(8, bytes("yz"));
    for (const openfits::input_source* source :
         {static_cast<const openfits::input_source*>(&before), static_cast<const openfits::input_source*>(&after)}) {
        std::vector<std::byte> content(10);
        source->read(0, content);
        EXPECT_EQ(text(content), "ab234567yz");
    }
}

TEST(memory_device, reads_and_writes_every_range_of_a_buffer)
{
    std::vector<std::byte> buffer = pattern(37);
    std::vector<std::byte> expected = buffer;
    memory_device device(buffer);
    ASSERT_EQ(device.size(), buffer.size());
    write_and_read_every_range(device, expected);
    EXPECT_EQ(buffer, expected);
}

TEST(memory_device, writes_past_the_end_are_refused_and_reads_past_it_are_end_of_data)
{
    std::vector<std::byte> buffer = bytes("0123456789");
    memory_device device(buffer);
    std::vector<std::byte> destination(4);

    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { device.write(7, bytes("abcd")); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { device.write(11, {}); }));
    EXPECT_TRUE(throws<io_error>(errc::end_of_data, [&] { device.read(7, destination); }));
    EXPECT_EQ(text(buffer), "0123456789");
}

TEST(memory_device, supports_concurrent_access_and_flushes_without_doing_anything)
{
    std::vector<std::byte> buffer = bytes("0123456789");
    memory_device device(buffer);

    EXPECT_TRUE(device.supports_concurrent_reads());
    EXPECT_TRUE(device.supports_concurrent_writes());
    EXPECT_EQ(device.description(), "");
    device.flush();
    EXPECT_EQ(text(buffer), "0123456789");
}

TEST(memory_device, borrows_a_buffer_given_by_name_which_keeps_its_size)
{
    // A vector given by name is borrowed: what the device writes is in it, and nothing changes its size.
    std::vector<std::byte> buffer = bytes("0123456789");
    memory_device device(buffer);
    device.write(2, bytes("ab"));

    EXPECT_EQ(text(buffer), "01ab456789");
    EXPECT_EQ(device.data().data(), buffer.data());
    EXPECT_FALSE(device.can_resize());
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { device.resize(20); }));
    EXPECT_TRUE(device.release().empty());
    EXPECT_EQ(device.size(), 10U);
    EXPECT_EQ(text(buffer), "01ab456789");
}

TEST(memory_device, owns_a_vector_moved_in_which_it_resizes_and_releases)
{
    std::vector<std::byte> moved = bytes("0123456789");
    memory_device device(std::move(moved));
    resize_both_ways(device);

    device.resize(3);
    device.write(0, bytes("xyz"));
    EXPECT_EQ(text(device.data()), "xyz");
    EXPECT_EQ(text(device.release()), "xyz");
    EXPECT_EQ(device.size(), 0U);
    EXPECT_TRUE(device.data().empty());
    // The device owns an empty vector then, which grows again.
    device.resize(2);
    EXPECT_EQ(content_of(device), zero_padded("", 2));
}

TEST(memory_device, a_size_beyond_what_memory_holds_is_bad_alloc_and_the_bytes_stay)
{
    memory_device device(bytes("0123456789"));

    EXPECT_THROW(device.resize(std::uint64_t{1} << 63U), std::bad_alloc);
    EXPECT_EQ(text(device.data()), "0123456789");
}

// A device that records its calls, and tells when two of them overlapped in time.
class recording_device final : public openfits::random_access_device
{
public:
    explicit recording_device(std::size_t size, bool concurrent = false, bool resizable = true)
        : content_(size), concurrent_(concurrent), resizable_(resizable)
    {}

    std::uint64_t size() const override
    {
        return content_.size();
    }

    void read(std::uint64_t offset, std::span<std::byte> destination) const override
    {
        const call_scope scope(*this);
        record("read", offset, destination.size());
        std::copy_n(content_.begin() + static_cast<std::ptrdiff_t>(offset), destination.size(), destination.begin());
    }

    void write(std::uint64_t offset, std::span<const std::byte> data) override
    {
        const call_scope scope(*this);
        record("write", offset, data.size());
        std::ranges::copy(data, content_.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    bool can_resize() const override
    {
        return resizable_;
    }

    void resize(std::uint64_t size) override
    {
        const call_scope scope(*this);
        record("resize", size, 0);
        content_.resize(size);
    }

    void flush() override
    {
        const call_scope scope(*this);
        record("flush", 0, 0);
    }

    bool supports_concurrent_reads() const override
    {
        return concurrent_;
    }

    bool supports_concurrent_writes() const override
    {
        return concurrent_;
    }

    std::string description() const override
    {
        return "the recording device";
    }

    [[nodiscard]] std::vector<std::string> calls() const
    {
        const std::scoped_lock lock(mutex_);
        return calls_;
    }

    [[nodiscard]] bool overlapped() const noexcept
    {
        return overlapped_;
    }

private:
    // Counts the calls in progress for its lifetime, and waits a little inside, so that two threads that are let in at
    // once overlap. A sleep would last a whole tick of the timer on Windows.
    class call_scope
    {
    public:
        explicit call_scope(const recording_device& device) : device_(device)
        {
            if (++device_.active_ > 1) {
                device_.overlapped_ = true;
            }
            const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(100);
            while (std::chrono::steady_clock::now() < until) {
            }
        }

        call_scope(const call_scope&) = delete;
        call_scope& operator=(const call_scope&) = delete;

        ~call_scope()
        {
            --device_.active_;
        }

    private:
        const recording_device& device_;
    };

    void record(std::string_view what, std::uint64_t offset, std::size_t length) const
    {
        const std::scoped_lock lock(mutex_);
        calls_.push_back(std::string(what) + ' ' + std::to_string(offset) + ' ' + std::to_string(length));
    }

    std::vector<std::byte> content_;
    bool concurrent_;
    bool resizable_;
    mutable std::mutex mutex_;
    mutable std::vector<std::string> calls_;
    mutable std::atomic<int> active_{0};
    mutable std::atomic<bool> overlapped_{false};
};

TEST(thread_safe_device, refuses_a_null_device)
{
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [] { thread_safe_device device(nullptr); }));
}

TEST(thread_safe_device, checks_every_range_before_the_device_sees_it)
{
    auto owned = std::make_unique<recording_device>(10);
    const recording_device& recorded = *owned;
    const thread_safe_device device(std::move(owned));
    std::vector<std::byte> destination(4);
    const std::vector<std::byte> data = bytes("abcd");

    EXPECT_EQ(device.size(), 10U);
    EXPECT_EQ(device.description(), "the recording device");
    EXPECT_TRUE(throws<io_error>(errc::end_of_data, [&] { device.read(7, destination); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { device.write(7, data); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { device.write(11, {}); }));
    // Empty calls within the device reach it not at all.
    device.read(10, {});
    device.write(10, {});
    device.write(6, data);
    device.read(6, destination);
    device.flush();
    EXPECT_EQ(destination, data);
    EXPECT_EQ(recorded.calls(), (std::vector<std::string>{"write 6 4", "read 6 4", "flush 0 0"}));
}

TEST(thread_safe_device, checks_the_calls_after_a_resize_against_the_new_size)
{
    auto owned = std::make_unique<recording_device>(10);
    const recording_device& recorded = *owned;
    thread_safe_device device(std::move(owned));
    std::vector<std::byte> destination(4);
    EXPECT_TRUE(device.can_resize());

    device.resize(20);
    EXPECT_EQ(device.size(), 20U);
    device.write(16, bytes("abcd"));
    device.resize(8);
    EXPECT_EQ(device.size(), 8U);
    EXPECT_TRUE(throws<io_error>(errc::end_of_data, [&] { device.read(6, destination); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { device.write(6, bytes("abcd")); }));
    EXPECT_EQ(recorded.calls(), (std::vector<std::string>{"resize 20 0", "write 16 4", "resize 8 0"}));
}

TEST(thread_safe_device, refuses_to_resize_a_device_that_cannot_before_the_device_sees_it)
{
    auto owned = std::make_unique<recording_device>(10, false, false);
    const recording_device& recorded = *owned;
    thread_safe_device device(std::move(owned));

    EXPECT_FALSE(device.can_resize());
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { device.resize(20); }));
    EXPECT_EQ(device.size(), 10U);
    EXPECT_TRUE(recorded.calls().empty());
}

TEST(thread_safe_device, its_source_reads_through_the_device)
{
    auto owned = std::make_unique<recording_device>(10);
    const recording_device& recorded = *owned;
    thread_safe_device device(std::move(owned));
    device.write(0, bytes("0123456789"));
    const std::unique_ptr<openfits::input_source> source = device.source();
    std::vector<std::byte> destination(3);

    EXPECT_EQ(source->size(), 10U);
    EXPECT_EQ(source->description(), "the recording device");
    // The device decides which calls run at once.
    EXPECT_TRUE(source->supports_concurrent_reads());
    source->read(2, destination);
    EXPECT_EQ(text(destination), "234");
    EXPECT_TRUE(throws<io_error>(errc::end_of_data, [&] { source->read(8, destination); }));
    // The source follows the size of the device.
    device.resize(12);
    EXPECT_EQ(source->size(), 12U);
    source->read(9, destination);
    EXPECT_EQ(text(destination), std::string("9\0\0", 3));
    EXPECT_EQ(recorded.calls(), (std::vector<std::string>{"write 0 10", "read 2 3", "resize 12 0", "read 9 3"}));
}

// Reads and writes from 8 threads at once, each at its own range of 4 bytes, and checks what each read.
void use_from_threads(const thread_safe_device& device)
{
    std::atomic<int> wrong{0};
    openfits::test::run_threads(8, [&device, &wrong](std::size_t t) {
        const std::vector<std::byte> data(4, static_cast<std::byte>(t));
        std::vector<std::byte> read(4);
        for (int i = 0; i < 20; ++i) {
            device.write(t * 4, data);
            device.read(t * 4, read);
            if (read != data) {
                ++wrong;
            }
        }
    });
    EXPECT_EQ(wrong, 0);
}

TEST(thread_safe_device, calls_a_device_without_concurrent_writes_one_at_a_time)
{
    auto owned = std::make_unique<recording_device>(32);
    const recording_device& recorded = *owned;
    const thread_safe_device device(std::move(owned));
    use_from_threads(device);
    EXPECT_FALSE(recorded.overlapped());
    EXPECT_EQ(recorded.calls().size(), 8U * 40U);
}

TEST(thread_safe_device, calls_a_device_with_concurrent_writes_from_several_threads_at_once)
{
    auto owned = std::make_unique<recording_device>(32, true);
    const recording_device& recorded = *owned;
    const thread_safe_device device(std::move(owned));
    use_from_threads(device);
    // 320 calls of 100 µs each, from 8 threads that start together: some overlap, unless the device were serialized.
    EXPECT_TRUE(recorded.overlapped());
}

TEST(thread_safe_device, passes_the_exceptions_of_the_device_through_and_keeps_its_size_when_a_resize_fails)
{
    for (const openfits::test::fault kind : {openfits::test::fault::error, openfits::test::fault::foreign_exception}) {
        std::vector<std::byte> moved(10);
        memory_device inner(std::move(moved));
        thread_safe_device device(std::make_unique<openfits::test::faulty_device>(inner, 1, kind));
        if (kind == openfits::test::fault::error) {
            EXPECT_TRUE(throws<io_error>(errc::write_failed, [&] { device.write(0, bytes("ab")); }));
        } else {
            EXPECT_THROW(device.write(0, bytes("ab")), openfits::test::injected_fault);
        }

        thread_safe_device resized(std::make_unique<openfits::test::faulty_device>(inner, 1, kind));
        EXPECT_ANY_THROW(resized.resize(20));
        EXPECT_EQ(resized.size(), 10U);
        EXPECT_EQ(inner.size(), 10U);
        resized.resize(12);
        EXPECT_EQ(inner.size(), 12U);
    }
}

} // namespace
