// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>

#include "core/checked_math.h"
#include "core/utf8.h"
#include "io/native_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <string_view>
#include <system_error>
#include <utility>

namespace openfits::detail {

namespace {

std::error_code last_error() noexcept
{
    return {errno, std::system_category()};
}

// The error of the system is taken before the message is made, by the caller, since the allocations of a message may
// change errno.
[[noreturn]] void fail(errc code, const std::string& message, std::error_code system, error_context context = {})
{
    // The text of strerror() follows the locale of the application, which may not be UTF-8.
    std::string reason = system.message();
    if (!is_valid_utf8(reason)) {
        reason = "error " + std::to_string(system.value());
    }
    throw io_error(code, message + ": " + reason, system, std::move(context));
}

[[noreturn]] void fail_not_regular(const std::string& path)
{
    throw io_error(errc::not_a_regular_file, path + " is not a regular file");
}

// The directory that holds the file at path.
std::string directory_of(const std::string& path)
{
    const std::size_t separator = path.find_last_of('/');
    if (separator == std::string::npos) {
        return ".";
    }
    return separator == 0 ? "/" : path.substr(0, separator);
}

// Makes a rename durable. Some file systems cannot flush a directory and say so with EINVAL or ENOTSUP; their renames
// are as durable as they get.
void flush_directory(const std::string& path)
{
    const std::string directory = directory_of(path);
    const int handle = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC);
    if (handle < 0) {
        const std::error_code error = last_error();
        fail(errc::write_failed, "cannot open the directory of " + path, error);
    }
#if defined(__APPLE__)
    // As for a file (flush_to_disk()): fsync() leaves the new name in the cache of the drive, F_FULLFSYNC flushes it
    // too.
    if (::fcntl(handle, F_FULLFSYNC) == 0) {
        (void)::close(handle);
        return;
    }
#endif
    const int result = ::fsync(handle);
    const std::error_code error = last_error();
    (void)::close(handle);
    if (result != 0 && error != std::errc::invalid_argument && error != std::errc::not_supported) {
        fail(errc::write_failed, "cannot flush the directory of " + path + " to disk", error);
    }
}

[[noreturn]] void fail_rename(const std::string& target, std::error_code error)
{
    if (error == std::errc::file_exists) {
        throw io_error(errc::file_exists, target + " exists, and the file may not replace it", error);
    }
    fail(errc::write_failed, "cannot replace " + target, error);
}

// Moves the file at path to target, which must not exist, atomically: the rename of the system that refuses an existing
// target where there is one and the file system supports it, and otherwise a hard link, which fails when the target
// exists, after which the temporary name is removed.
void rename_exclusive(const std::string& path, const std::string& target)
{
#if defined(__linux__)
    if (::renameat2(AT_FDCWD, path.c_str(), AT_FDCWD, target.c_str(), RENAME_NOREPLACE) == 0) {
        return;
    }
    const std::error_code refused = last_error();
    if (refused != std::errc::invalid_argument && refused != std::errc::function_not_supported) {
        fail_rename(target, refused);
    }
#elif defined(__APPLE__)
    if (::renamex_np(path.c_str(), target.c_str(), RENAME_EXCL) == 0) {
        return;
    }
    const std::error_code refused = last_error();
    if (refused != std::errc::not_supported) {
        fail_rename(target, refused);
    }
#endif
    if (::link(path.c_str(), target.c_str()) != 0) {
        const std::error_code error = last_error();
        fail_rename(target, error);
    }
    // The file is in place; a temporary name left behind would only be a second name of it.
    (void)::unlink(path.c_str());
}

} // namespace

std::string absolute_path(const std::string& path)
{
    if (path.starts_with('/')) {
        return path;
    }
    // getcwd() fails with ERANGE until the buffer holds the path.
    std::string directory(256, '\0');
    while (::getcwd(directory.data(), directory.size()) == nullptr) {
        const std::error_code error = last_error();
        if (error != std::errc::result_out_of_range) {
            fail(errc::open_failed, "cannot find the current directory, from which " + path + " is taken", error);
        }
        directory.resize(directory.size() * 2);
    }
    directory.resize(std::char_traits<char>::length(directory.c_str()));
    if (!is_valid_utf8(directory)) {
        throw io_error(errc::open_failed,
                       "the path of the current directory, from which " + path + " is taken, is not valid UTF-8");
    }
    if (!directory.ends_with('/')) {
        directory += '/';
    }
    return directory + path;
}

void check_replaceable(const std::string& path)
{
    // lstat() does not follow a symbolic link. rename() replaces anything but a directory, the node of a device too.
    struct stat status{};
    if (::lstat(path.c_str(), &status) == 0 && !S_ISREG(status.st_mode) && !S_ISLNK(status.st_mode)) {
        fail_not_regular(path);
    }
}

bool exists(const std::string& path)
{
    // lstat() finds a symbolic link whether or not what it points to exists.
    struct stat status{};
    return ::lstat(path.c_str(), &status) == 0;
}

native_file native_file::open_for_reading(const std::string& path)
{
    return open_existing(path, false);
}

native_file native_file::open_for_update(const std::string& path)
{
    return open_existing(path, true);
}

native_file native_file::open_existing(const std::string& path, bool writable)
{
    const std::string what = writable ? "cannot open " + path + " for reading and writing" : "cannot open " + path;
    // What is not a regular file is refused before it is opened, since opening a device can act on it, as opening a
    // serial port resets some controllers; and again once it is open, since the path can name another file by then.
    struct stat named{};
    if (::stat(path.c_str(), &named) == 0 && !S_ISREG(named.st_mode)) {
        fail_not_regular(path);
    }
    // O_NONBLOCK keeps the open of a FIFO from waiting for a writer, and O_NOCTTY the open of a terminal from making it
    // the controlling terminal of the process. Neither has an effect on regular files, which are the only ones
    // accepted.
    const int handle = ::open(path.c_str(), (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    if (handle < 0) {
        fail(errc::open_failed, what, last_error());
    }
    native_file file(handle, path);
    struct stat status{};
    if (::fstat(handle, &status) != 0) {
        fail(errc::open_failed, what, last_error());
    }
    if (!S_ISREG(status.st_mode)) {
        fail_not_regular(path);
    }
    return file;
}

std::optional<native_file> native_file::create_new(const std::string& path, std::string name)
{
    // Read and write permissions for everyone the umask allows, as for any new file.
    const int handle = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (handle < 0) {
        const std::error_code error = last_error();
        if (error == std::errc::file_exists) {
            return std::nullopt;
        }
        fail(errc::open_failed, "cannot create a temporary file next to " + name, error);
    }
    return native_file(handle, std::move(name));
}

std::optional<native_file> native_file::create_scratch(const std::string& path)
{
    // Readable and writable by the owner alone, and removed from its directory at once: the open handle keeps the
    // file until it is closed, by the process or at its end.
    const int handle = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (handle < 0) {
        const std::error_code error = last_error();
        if (error == std::errc::file_exists) {
            return std::nullopt;
        }
        fail(errc::open_failed, "cannot create the temporary file " + path, error);
    }
    native_file file(handle, path);
    if (::unlink(path.c_str()) != 0) {
        const std::error_code error = last_error();
        fail(errc::open_failed, "cannot remove the temporary file " + path + " from its directory", error);
    }
    return file;
}

std::uint64_t native_file::size() const
{
    struct stat status{};
    if (::fstat(handle_, &status) != 0) {
        const std::error_code error = last_error();
        fail(errc::read_failed, "cannot read the size of " + name_, error);
    }
    return checked_cast<std::uint64_t>(status.st_size);
}

void native_file::read_at(std::uint64_t offset, std::span<std::byte> destination, std::size_t transfer) const
{
    std::uint64_t position = offset;
    while (!destination.empty()) {
        const std::size_t chunk = std::min({destination.size(), transfer, max_transfer});
        const ssize_t count = ::pread(handle_, destination.data(), chunk, checked_cast<off_t>(position));
        if (count < 0) {
            const std::error_code error = last_error();
            if (error == std::errc::interrupted) {
                continue;
            }
            fail(errc::read_failed, "cannot read " + name_, error, {.offset = position});
        }
        if (count == 0) {
            throw io_error(errc::end_of_data, name_ + " ends before the data that was asked for", {},
                           {.offset = position});
        }
        const auto transferred = static_cast<std::size_t>(count);
        destination = destination.subspan(transferred);
        position += transferred;
    }
}

void native_file::write_at(std::uint64_t offset, std::span<const std::byte> data, std::size_t transfer) const
{
    std::uint64_t position = offset;
    while (!data.empty()) {
        const std::size_t chunk = std::min({data.size(), transfer, max_transfer});
        const ssize_t count = ::pwrite(handle_, data.data(), chunk, checked_cast<off_t>(position));
        if (count < 0) {
            const std::error_code error = last_error();
            if (error == std::errc::interrupted) {
                continue;
            }
            fail(errc::write_failed, "cannot write " + name_, error, {.offset = position});
        }
        const auto transferred = static_cast<std::size_t>(count);
        data = data.subspan(transferred);
        position += transferred;
    }
}

void native_file::resize(std::uint64_t size) const
{
    // The bytes that extend a file read as zeros, and take no space on file systems with holes.
    const auto length = checked_cast<off_t>(size);
    while (::ftruncate(handle_, length) != 0) {
        const std::error_code error = last_error();
        if (error != std::errc::interrupted) {
            fail(errc::write_failed, "cannot resize " + name_ + " to " + std::to_string(size) + " bytes", error);
        }
    }
}

void native_file::flush_to_disk() const
{
#if defined(__APPLE__)
    // On macOS fsync() leaves the data in the cache of the drive. F_FULLFSYNC flushes it too, where the file system
    // supports it.
    if (::fcntl(handle_, F_FULLFSYNC) == 0) {
        return;
    }
#endif
    if (::fsync(handle_) != 0) {
        const std::error_code error = last_error();
        fail(errc::write_failed, "cannot flush " + name_ + " to disk", error);
    }
}

void native_file::close()
{
    // POSIX leaves the descriptor unspecified after a failed close(), and Linux closes it in every case, so it is
    // never closed twice.
    const int handle = std::exchange(handle_, closed_handle);
    if (handle != closed_handle && ::close(handle) != 0) {
        const std::error_code error = last_error();
        fail(errc::write_failed, "cannot write " + name_, error);
    }
}

void native_file::reset() noexcept
{
    const int handle = std::exchange(handle_, closed_handle);
    if (handle != closed_handle) {
        (void)::close(handle);
    }
}

void native_file::commit(const std::string& path, const std::string& target, bool flush_to_disk, bool replace)
{
    if (flush_to_disk) {
        this->flush_to_disk();
    }
    close();
    if (!replace) {
        rename_exclusive(path, target);
    } else if (std::rename(path.c_str(), target.c_str()) != 0) {
        const std::error_code error = last_error();
        fail(errc::write_failed, "cannot replace " + target, error);
    }
    if (flush_to_disk) {
        flush_directory(target);
    }
}

void native_file::discard(const std::string& path) noexcept
{
    // After a successful rename nothing is left at path, and unlink() fails harmlessly.
    reset();
    (void)::unlink(path.c_str());
}

} // namespace openfits::detail
