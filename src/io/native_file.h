// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <mutex>
#include <vector>
#endif

// The platform layer of file_source, file_sink and file_device: native_file_win32.cpp or native_file_posix.cpp. Paths
// are UTF-8 and valid, which the public classes check. Every failure is an io_error whose message names the file.

namespace openfits::detail {

/// The most that one call of the system transfers in the loops of read_at() and write_at(): ReadFile() and WriteFile()
/// take a 32-bit length, pread() and pwrite() at most SSIZE_MAX bytes, and Linux transfers at most 2 GiB - 4 KiB in one
/// call.
inline constexpr std::size_t max_transfer = std::size_t{1} << 30;

/// path made absolute from the current directory, so that it names the same file after the current directory changes:
/// on Windows as the system makes a path absolute, which normalizes it as every path (GetFullPathNameW()), and on POSIX
/// systems the current directory followed by a relative path. Throws io_error with errc::open_failed when the current
/// directory cannot be found, or its path is not valid UTF-8.
[[nodiscard]] std::string absolute_path(const std::string& path);

/// Throws io_error with errc::not_a_regular_file when what the absolute path names is something that a file must not
/// replace: a directory, a FIFO, a socket or a device, which on Windows includes a device name such as NUL in any
/// directory. A link, a symbolic link or a junction on Windows, is not followed, since a file that replaces it replaces
/// the link alone. Nothing at path, and what cannot be looked at, is left to create_new() and commit(), which fail
/// where the system refuses them.
void check_replaceable(const std::string& path);

/// True when something exists at the absolute path: a file, a directory, a device or a link, whether or not what the
/// link points to exists. What cannot be looked at is taken for nothing, and left to commit().
[[nodiscard]] bool exists(const std::string& path);

/// A file of the operating system, closed on destruction. Reads and writes take explicit offsets, so that any number
/// of threads can read and write one file at once.
class native_file
{
public:
#if defined(_WIN32)
    using handle_type = void*;
    static constexpr handle_type closed_handle = nullptr;
#else
    using handle_type = int;
    static constexpr handle_type closed_handle = -1;
#endif

    /// Opens a regular file for reading. What the system tells is not one is refused before it is opened, since opening
    /// a device can act on it: on Windows a device name that the system makes a path of its device namespace, such as
    /// NUL in any directory, and on POSIX systems what stat() does not call a regular file. Throws io_error with
    /// errc::open_failed or errc::not_a_regular_file, which a directory, the root of a drive or a share included, is.
    [[nodiscard]] static native_file open_for_reading(const std::string& path);

    /// Opens an existing regular file for reading and writing, with the checks of open_for_reading(). A file that the
    /// process may not write is errc::open_failed.
    [[nodiscard]] static native_file open_for_update(const std::string& path);

    /// Creates a file for writing, or returns nothing when something exists at path already. name is the path of the
    /// file it becomes, which error messages use. Throws io_error with errc::open_failed.
    [[nodiscard]] static std::optional<native_file> create_new(const std::string& path, std::string name);

    /// Creates a file for reading and writing at path, which the system removes once it is closed, also when the
    /// process ends abruptly: on Windows a file deleted on close, and on POSIX systems a file removed from its
    /// directory as soon as it is created. Returns nothing when something exists at path already. Throws io_error with
    /// errc::open_failed.
    [[nodiscard]] static std::optional<native_file> create_scratch(const std::string& path);

    native_file(native_file&& other) noexcept
        : handle_(std::exchange(other.handle_, closed_handle)), name_(std::move(other.name_))
#if defined(_WIN32)
          ,
          readers_(std::move(other.readers_))
#endif
    {}

    native_file& operator=(native_file&& other) noexcept
    {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, closed_handle);
            name_ = std::move(other.name_);
#if defined(_WIN32)
            readers_ = std::move(other.readers_);
#endif
        }
        return *this;
    }

    native_file(const native_file&) = delete;
    native_file& operator=(const native_file&) = delete;

    ~native_file()
    {
        reset();
    }

    /// The path, or the name given to create_new().
    [[nodiscard]] const std::string& name() const noexcept
    {
        return name_;
    }

    [[nodiscard]] std::uint64_t size() const;

    /// Fills destination with the bytes at offset, in calls of the system of at most transfer bytes, which is not zero.
    /// A file that ends first is an io_error with errc::end_of_data. Any number of threads may read at once, and their
    /// reads run at once on every system.
    void read_at(std::uint64_t offset, std::span<std::byte> destination, std::size_t transfer = max_transfer) const;

    /// Writes data at offset, in calls of the system of at most transfer bytes, which is not zero.
    void write_at(std::uint64_t offset, std::span<const std::byte> data, std::size_t transfer = max_transfer) const;

    /// Truncates or extends the file to size bytes, which the system fills with zeros. Throws io_error with
    /// errc::write_failed.
    void resize(std::uint64_t size) const;

    /// Flushes what was written to the storage device. Throws io_error with errc::write_failed.
    void flush_to_disk() const;

    /// Closes the file, which create_new() made at path, and moves it over target, which it replaces atomically, also
    /// while other programs read target. With flush_to_disk, the content and the rename reach the storage device
    /// before it returns. Without replace, the rename never replaces what exists at target, and fails with
    /// errc::file_exists then, atomically. Throws io_error with errc::write_failed otherwise. The file is then either
    /// still at path, to be discarded, or already in place of the target.
    void commit(const std::string& path, const std::string& target, bool flush_to_disk, bool replace = true);

    /// Closes the file, which create_new() made at path, and removes it, unless commit() moved it to its target.
    void discard(const std::string& path) noexcept;

    /// Closes the file, ignoring failures.
    void reset() noexcept;

private:
#if defined(_WIN32)
    // The handles of the reads that run while another one does. Windows runs the calls on a synchronous handle one at a
    // time, so that a read that would wait for another one takes a handle of its own to the same file, from
    // ReOpenFile(), which it leaves for the next ones; they close with the file.
    struct readers
    {
        std::mutex mutex;
        bool primary_busy = false;
        std::vector<handle_type> idle;
    };
    class reading;

    // The handle is the object's from the start: a list of readers that cannot be made closes it.
    native_file(handle_type handle, std::string name) : handle_(handle), name_(std::move(name))
    {
        try {
            readers_ = std::make_unique<readers>();
        } catch (...) {
            reset();
            throw;
        }
    }

    // A file whose list of readers was made before it: create_new() makes the list before the file, since closing the
    // handle of a new file leaves the file behind.
    native_file(handle_type handle, std::string name, std::unique_ptr<readers> list) noexcept
        : handle_(handle), name_(std::move(name)), readers_(std::move(list))
    {}
#else
    native_file(handle_type handle, std::string name) noexcept : handle_(handle), name_(std::move(name)) {}
#endif

    // Opens an existing regular file with the given access, after the checks of open_for_reading().
    [[nodiscard]] static native_file open_existing(const std::string& path, bool writable);

    // Closes the file and reports a failure, which may be a delayed write error.
    void close();

    handle_type handle_;
    std::string name_;
#if defined(_WIN32)
    std::unique_ptr<readers> readers_;
#endif
};

} // namespace openfits::detail
