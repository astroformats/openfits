// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>

#include "core/checked_math.h"
#include "core/utf16.h"
#include "io/native_file.h"
#include "io/paths.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// UTF-8 paths become UTF-16 here, at the boundary of the system calls, and nowhere else.

namespace openfits::detail {

namespace {

std::error_code last_error() noexcept
{
    return {static_cast<int>(GetLastError()), std::system_category()};
}

// The system's description of an error. std::error_code::message() would give it in the ANSI code page.
std::string describe(std::error_code error)
{
    std::array<wchar_t, 512> buffer{};
    const DWORD length =
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                       static_cast<DWORD>(error.value()), 0, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr);
    std::u16string text(buffer.data(), buffer.data() + length);
    while (!text.empty() &&
           (text.back() == u'\r' || text.back() == u'\n' || text.back() == u' ' || text.back() == u'.')) {
        text.pop_back();
    }
    const std::optional<std::string> converted = text.empty() ? std::nullopt : utf16_to_utf8(text);
    return converted ? *converted : "system error " + std::to_string(error.value());
}

// The error of the system is taken before the message is made, by the caller, since the allocations of a message may
// change it.
[[noreturn]] void fail(errc code, const std::string& message, std::error_code system, error_context context = {})
{
    throw io_error(code, message + ": " + describe(system), system, std::move(context));
}

[[noreturn]] void fail_not_regular(const std::string& path)
{
    throw io_error(errc::not_a_regular_file, path + " is not a regular file");
}

// A rename that may not replace its target reports one that exists with either code.
bool names_existing_target(std::error_code error) noexcept
{
    return error.value() == ERROR_ALREADY_EXISTS || error.value() == ERROR_FILE_EXISTS;
}

[[noreturn]] void fail_rename(const std::string& target, std::error_code error)
{
    if (names_existing_target(error)) {
        throw io_error(errc::file_exists, target + " exists, and the file may not replace it", error);
    }
    fail(errc::write_failed, "cannot replace " + target, error);
}

// path, absolute and normalized as Windows normalizes every path. Returns an empty string when GetFullPathNameW()
// fails, and GetLastError() tells why.
std::wstring full_path(const std::string& path)
{
    const std::u16string utf16 = utf8_to_utf16(path);
    const std::wstring given(utf16.begin(), utf16.end());
    DWORD length = GetFullPathNameW(given.c_str(), 0, nullptr, nullptr);
    if (length == 0) {
        return {};
    }
    std::wstring full(length, L'\0');
    length = GetFullPathNameW(given.c_str(), length, full.data(), nullptr);
    if (length == 0 || length >= full.size()) {
        return {};
    }
    full.resize(length);
    return full;
}

// The path for the wide functions of the system: absolute, normalized as Windows normalizes every path, and with the
// \\?\ prefix, which lifts the limit of MAX_PATH (260) characters. Device paths (\\.\) keep their form. Returns an
// empty string when GetFullPathNameW() fails, and GetLastError() tells why.
std::wstring system_path(const std::string& path)
{
    std::wstring full = full_path(path);
    if (full.empty()) {
        return {};
    }
    if (full.starts_with(LR"(\\?\)") || full.starts_with(LR"(\\.\)")) {
        return full;
    }
    if (full.starts_with(LR"(\\)")) {
        return LR"(\\?\UNC\)" + full.substr(2);
    }
    return LR"(\\?\)" + full;
}

OVERLAPPED at(std::uint64_t offset) noexcept
{
    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(offset & 0xFFFF'FFFFU);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32U);
    return overlapped;
}

bool is_directory(const std::wstring& path) noexcept
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// True when wide, a path that system_path() made, names a device (names_a_device()): a path of the device namespace
// that the application gave, such as \\.\COM1 or \\.\pipe\name, or that GetFullPathNameW() made of a device name, such
// as NUL in any directory, and before Windows 11 also CON, COM1 or LPT1, with or without an extension, which becomes
// \\.\NUL. Such a path is refused by its text, before anything opens it or asks for its attributes, since opening a
// device can act on it, as opening a serial port resets some controllers, and asking for the attributes of a named pipe
// opens it.
bool is_device(const std::wstring& wide)
{
    // No valid UTF-8 path becomes a path that is not valid UTF-16.
    const std::optional<std::string> path = utf16_to_utf8(std::u16string(wide.begin(), wide.end()));
    return !path || names_a_device(*path);
}

// True when the reparse point at path is a symbolic link or a junction, which a rename replaces itself. A reparse point
// of another kind, such as the placeholder of a cloud file system, is what its attributes say. False when its kind
// cannot be read.
bool is_link(const std::wstring& path) noexcept
{
    HANDLE handle = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO information{};
    const BOOL read = GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &information, sizeof(information));
    (void)CloseHandle(handle);
    return read != FALSE &&
           (information.ReparseTag == IO_REPARSE_TAG_SYMLINK || information.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT);
}

// Renames an open file over target with POSIX semantics, which replace a target that other programs have open, as a
// rename does on POSIX systems, provided that they opened it with FILE_SHARE_DELETE, as file_source does; the rename
// fails otherwise. Without replace, the rename fails when something exists at target. Windows 10 1607 and later
// support it on NTFS. Returns false where it is not supported.
bool rename_by_handle(HANDLE handle, const std::wstring& target, const std::string& name, bool replace)
{
    // FILE_RENAME_INFO ends with the name, which extends beyond the structure.
    const std::size_t name_bytes = target.size() * sizeof(wchar_t);
    const std::size_t size = offsetof(FILE_RENAME_INFO, FileName) + name_bytes + sizeof(wchar_t);
    std::vector<FILE_RENAME_INFO> buffer((size + sizeof(FILE_RENAME_INFO) - 1) / sizeof(FILE_RENAME_INFO));
    FILE_RENAME_INFO& info = buffer.front();
    info.Flags = FILE_RENAME_FLAG_POSIX_SEMANTICS;
    if (replace) {
        info.Flags |= FILE_RENAME_FLAG_REPLACE_IF_EXISTS;
    }
    info.RootDirectory = nullptr;
    info.FileNameLength = static_cast<DWORD>(name_bytes);
    std::memcpy(reinterpret_cast<std::byte*>(buffer.data()) + offsetof(FILE_RENAME_INFO, FileName), target.c_str(),
                name_bytes + sizeof(wchar_t));

    if (SetFileInformationByHandle(handle, FileRenameInfoEx, &info, static_cast<DWORD>(size)) != FALSE) {
        return true;
    }
    const std::error_code error = last_error();
    if (error.value() == ERROR_INVALID_PARAMETER || error.value() == ERROR_NOT_SUPPORTED ||
        error.value() == ERROR_INVALID_FUNCTION) {
        return false;
    }
    fail_rename(name, error);
}

} // namespace

std::string absolute_path(const std::string& path)
{
    const std::wstring full = full_path(path);
    if (full.empty()) {
        const std::error_code error = last_error();
        fail(errc::open_failed, "cannot make " + path + " absolute", error);
    }
    std::optional<std::string> converted = utf16_to_utf8(std::u16string(full.begin(), full.end()));
    if (!converted) {
        throw io_error(errc::open_failed, "the absolute path of " + path + " is not valid UTF-16");
    }
    return std::move(*converted);
}

void check_replaceable(const std::string& path)
{
    const std::wstring wide = system_path(path);
    if (wide.empty()) {
        return;
    }
    if (is_device(wide)) {
        fail_not_regular(path);
    }
    // The attributes of a symbolic link or a junction are its own, with FILE_ATTRIBUTE_REPARSE_POINT: a directory that
    // is one is a link, which a rename replaces, and a directory that is a reparse point of another kind a directory.
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return;
    }
    const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                           ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 || !is_link(wide));
    if (directory || (attributes & FILE_ATTRIBUTE_DEVICE) != 0) {
        fail_not_regular(path);
    }
}

bool exists(const std::string& path)
{
    const std::wstring wide = system_path(path);
    // The attributes of a link are its own, whether or not its target exists.
    return !wide.empty() && GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES;
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
    const std::wstring wide = system_path(path);
    if (wide.empty()) {
        fail(errc::open_failed, what, last_error());
    }
    if (is_device(wide)) {
        fail_not_regular(path);
    }
    // Other programs may go on using the file, as they could on POSIX systems. A file with the read-only attribute
    // refuses write access (ERROR_ACCESS_DENIED).
    const DWORD access = writable ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
    HANDLE handle = CreateFileW(wide.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const std::error_code error = last_error();
        // Opening a directory needs FILE_FLAG_BACKUP_SEMANTICS. Without it, the failure is a denied access, but for the
        // root of a drive (ERROR_PATH_NOT_FOUND) or of a share (ERROR_INVALID_NAME).
        if (is_directory(wide)) {
            fail_not_regular(path);
        }
        fail(errc::open_failed, what, error);
    }
    native_file file(handle, path);
    // A device such as NUL or a named pipe is not a disk file.
    if (GetFileType(handle) != FILE_TYPE_DISK) {
        fail_not_regular(path);
    }
    return file;
}

std::optional<native_file> native_file::create_new(const std::string& path, std::string name)
{
    const std::wstring wide = system_path(path);
    if (wide.empty()) {
        const std::error_code error = last_error();
        fail(errc::open_failed, "cannot create a temporary file next to " + name, error);
    }
    // DELETE access lets commit() rename the file and discard() delete it through the handle, which stays open until
    // then. FILE_SHARE_DELETE lets MoveFileExW() move the file while the handle is open. A link of any kind at the name
    // is not followed, so that it counts as a taken name, as O_EXCL makes it on POSIX systems: CreateFileW() created
    // the target of a dangling symbolic link.
    HANDLE handle = CreateFileW(wide.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const std::error_code error = last_error();
        // A directory at the name, a junction included, refuses the access rather than exist: taken too.
        if (error.value() == ERROR_FILE_EXISTS || error.value() == ERROR_ALREADY_EXISTS ||
            (error.value() == ERROR_ACCESS_DENIED && is_directory(wide))) {
            return std::nullopt;
        }
        fail(errc::open_failed, "cannot create a temporary file next to " + name, error);
    }
    return native_file(handle, std::move(name));
}

std::uint64_t native_file::size() const
{
    LARGE_INTEGER size{};
    if (GetFileSizeEx(handle_, &size) == FALSE) {
        const std::error_code error = last_error();
        fail(errc::read_failed, "cannot read the size of " + name_, error);
    }
    return static_cast<std::uint64_t>(size.QuadPart);
}

void native_file::read_at(std::uint64_t offset, std::span<std::byte> destination, std::size_t transfer) const
{
    // Each call names its offset in an OVERLAPPED structure, so concurrent reads do not share a file pointer. The
    // handle is synchronous, so ReadFile() returns when the read is complete.
    std::uint64_t position = offset;
    while (!destination.empty()) {
        const auto chunk = static_cast<DWORD>(std::min({destination.size(), transfer, max_transfer}));
        OVERLAPPED overlapped = at(position);
        DWORD transferred = 0;
        if (ReadFile(handle_, destination.data(), chunk, &transferred, &overlapped) == FALSE) {
            const std::error_code error = last_error();
            if (error.value() != ERROR_HANDLE_EOF) {
                fail(errc::read_failed, "cannot read " + name_, error, {.offset = position});
            }
            transferred = 0;
        }
        if (transferred == 0) {
            throw io_error(errc::end_of_data, name_ + " ends before the data that was asked for", {},
                           {.offset = position});
        }
        destination = destination.subspan(transferred);
        position += transferred;
    }
}

void native_file::write_at(std::uint64_t offset, std::span<const std::byte> data, std::size_t transfer) const
{
    std::uint64_t position = offset;
    while (!data.empty()) {
        const auto chunk = static_cast<DWORD>(std::min({data.size(), transfer, max_transfer}));
        OVERLAPPED overlapped = at(position);
        DWORD transferred = 0;
        if (WriteFile(handle_, data.data(), chunk, &transferred, &overlapped) == FALSE) {
            const std::error_code error = last_error();
            fail(errc::write_failed, "cannot write " + name_, error, {.offset = position});
        }
        data = data.subspan(transferred);
        position += transferred;
    }
}

void native_file::resize(std::uint64_t size) const
{
    // Through the handle, which has no file pointer to move, unlike SetEndOfFile(). NTFS reads the bytes that extend a
    // file as zeros without writing them.
    FILE_END_OF_FILE_INFO end{};
    end.EndOfFile.QuadPart = checked_cast<LONGLONG>(size);
    if (SetFileInformationByHandle(handle_, FileEndOfFileInfo, &end, sizeof(end)) == FALSE) {
        const std::error_code error = last_error();
        fail(errc::write_failed, "cannot resize " + name_ + " to " + std::to_string(size) + " bytes", error);
    }
}

void native_file::flush_to_disk() const
{
    if (FlushFileBuffers(handle_) == FALSE) {
        const std::error_code error = last_error();
        fail(errc::write_failed, "cannot flush " + name_ + " to disk", error);
    }
}

void native_file::commit(const std::string& path, const std::string& target, bool flush_to_disk, bool replace)
{
    if (flush_to_disk) {
        this->flush_to_disk();
    }
    const std::wstring wide_target = system_path(target);
    if (wide_target.empty()) {
        const std::error_code error = last_error();
        fail(errc::write_failed, "cannot replace " + target, error);
    }
    // Once the file is in place of the target, the handle moves out of this object, so that a failure afterwards
    // leaves discard() nothing to delete.
    if (rename_by_handle(handle_, wide_target, target, replace)) {
        native_file renamed = std::move(*this);
        if (flush_to_disk) {
            renamed.flush_to_disk();
        }
        renamed.close();
        return;
    }

    // Where POSIX semantics are not supported, such as on FAT file systems, the file is moved by path, which fails
    // while another program has the target open.
    const std::wstring wide_path = system_path(path);
    if (wide_path.empty()) {
        const std::error_code error = last_error();
        fail(errc::write_failed, "cannot replace " + target, error);
    }
    DWORD flags = replace ? MOVEFILE_REPLACE_EXISTING : 0;
    if (flush_to_disk) {
        flags |= MOVEFILE_WRITE_THROUGH;
    }
    if (MoveFileExW(wide_path.c_str(), wide_target.c_str(), flags) == FALSE) {
        const std::error_code error = last_error();
        fail_rename(target, error);
    }
    native_file moved = std::move(*this);
    moved.close();
}

void native_file::discard(const std::string& /*path*/) noexcept
{
    // Through the handle: no path to convert, and no file that took the name meanwhile can be hit.
    if (handle_ != closed_handle) {
        FILE_DISPOSITION_INFO disposition{};
        disposition.DeleteFile = TRUE;
        (void)SetFileInformationByHandle(handle_, FileDispositionInfo, &disposition, sizeof(disposition));
    }
    reset();
}

void native_file::close()
{
    HANDLE handle = std::exchange(handle_, closed_handle);
    if (handle != closed_handle && CloseHandle(handle) == FALSE) {
        const std::error_code error = last_error();
        fail(errc::write_failed, "cannot write " + name_, error);
    }
}

void native_file::reset() noexcept
{
    HANDLE handle = std::exchange(handle_, closed_handle);
    if (handle != closed_handle) {
        (void)CloseHandle(handle);
    }
}

} // namespace openfits::detail
