// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/export.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// @file
/// Sources, sinks and devices: where files are read from, written to, and edited through; and the progress of long
/// operations.

namespace openfits {

/// Reports the progress of a long operation, such as reading pixels or saving a file: done of total units of work, from
/// 0 to total. Returning false cancels the operation, which then throws cancelled_error. It is called on the thread
/// that runs the operation, and its exceptions pass through unchanged.
using progress_function = std::function<bool(std::uint64_t done, std::uint64_t total)>;

#if defined(_MSC_VER)
// The built-in sources, sinks and devices hold standard-library members, which cl reports for an exported class. A C++
// interface requires the same standard library on both sides of the DLL boundary anyway.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

/// Random-access input that files are read from: a file, a buffer, or anything an application implements.
///
/// Reads are positional and exact, so an implementation needs no cursor. The library never asks for bytes beyond
/// size(), and calls read() from one thread at a time unless supports_concurrent_reads() is true. The content must not
/// change while the library reads from the source. Sources are not copyable; the library holds them by pointer.
class OPENFITS_API input_source
{
public:
    input_source() = default;
    input_source(const input_source&) = delete;
    input_source& operator=(const input_source&) = delete;
    virtual ~input_source();

    /// Size of the content, in bytes.
    [[nodiscard]] virtual std::uint64_t size() const = 0;

    /// Fills destination with the bytes that start at offset, or throws io_error. The built-in sources throw it with
    /// errc::end_of_data when the bytes extend beyond size().
    virtual void read(std::uint64_t offset, std::span<std::byte> destination) const = 0;

    /// True when read() may run on several threads at once. False by default.
    [[nodiscard]] virtual bool supports_concurrent_reads() const;

    /// Names the source in error messages, for example with a path. Empty by default.
    [[nodiscard]] virtual std::string description() const;

    /// The content of the source when it lies in memory, such as a buffer or a file mapped into memory, which must then
    /// stay valid as long as the source exists. The library may read there instead of calling read(), from any number
    /// of threads at once, whatever supports_concurrent_reads() says. Empty by default, for a source read through
    /// read() alone; otherwise it has size() bytes, or the library refuses the source with usage_error when it takes
    /// it. A device is read through read() alone.
    [[nodiscard]] virtual std::span<const std::byte> bytes() const;
};

/// Output that files are written to: a file, a buffer, or anything an application implements.
///
/// Bytes are appended with write(). A sink that can also rewrite bytes it already holds lets the writer complete a
/// header once the data that it describes are written. finish() follows the last byte of a complete file; a sink
/// destroyed without it holds an incomplete one, which file_sink discards. Sinks are not copyable.
class OPENFITS_API output_sink
{
public:
    output_sink() = default;
    output_sink(const output_sink&) = delete;
    output_sink& operator=(const output_sink&) = delete;
    virtual ~output_sink();

    /// Appends data, or throws io_error.
    virtual void write(std::span<const std::byte> data) = 0;

    /// Number of bytes written so far, which is the offset of the next one.
    [[nodiscard]] virtual std::uint64_t position() const = 0;

    /// True when rewrite() is supported. False by default.
    [[nodiscard]] virtual bool can_rewrite() const;

    /// Overwrites bytes already written, without changing position(). offset + data.size() must not exceed position().
    /// The default implementation throws usage_error.
    virtual void rewrite(std::uint64_t offset, std::span<const std::byte> data);

    /// Completes the output after its last byte: flushes it, and commits it where that applies. Nothing may be written
    /// afterwards, which file_sink refuses and other sinks need not check. Does nothing by default.
    virtual void finish();
};

/// Storage that is read and written in place, by position, and that can grow or shrink: a source whose bytes can also
/// be overwritten, such as a file, a buffer, or anything an application implements.
///
/// The library checks every range before the device sees it, and never asks for bytes beyond size(), to read or to
/// write: the size changes through resize() alone. It calls the device from one thread at a time, reads included,
/// unless supports_concurrent_writes() is true, and then for ranges that do not overlap; resize() always runs alone.
/// Devices are not copyable.
class OPENFITS_API random_access_device : public input_source
{
public:
    random_access_device() = default;
    ~random_access_device() override;

    /// Overwrites the bytes that start at offset with data, or throws io_error.
    virtual void write(std::uint64_t offset, std::span<const std::byte> data) = 0;

    /// True when resize() is supported. False by default.
    [[nodiscard]] virtual bool can_resize() const;

    /// Changes the size of the device to size bytes, or throws io_error: the bytes beyond it are gone, and those that
    /// it adds read as zeros. The default implementation throws usage_error.
    virtual void resize(std::uint64_t size);

    /// Makes the bytes written so far durable, as a file is flushed to its storage device, or throws io_error. Does
    /// nothing by default.
    virtual void flush();

    /// True when write() may run on several threads at once, and while read() runs, for ranges that do not overlap; the
    /// reads then follow supports_concurrent_reads(). False by default.
    [[nodiscard]] virtual bool supports_concurrent_writes() const;
};

/// A regular file, read with positional I/O, so that any number of threads can read it at once.
///
/// The file stays open until the source is destroyed. Other programs may still read, write, rename or delete it, but
/// its content must not change while the library reads from it.
class OPENFITS_API file_source final : public input_source
{
public:
    /// Opens the file at path, which is UTF-8 on every platform, as the system opens it: the `.` and `..` steps and the
    /// links of the path are the system's to resolve, and the path is the name of the file, brackets included, as in
    /// `M31 [Ha].fits`. What is not a regular file is refused before it is opened, since opening a device can act on
    /// it: on Windows a path of the device namespace that names no file, such as `\\.\COM1` or `\\.\pipe\name`, and a
    /// device name such as NUL in any directory, by their text, and on POSIX systems what stat() says is not one.
    /// @throws usage_error when path is empty or not valid UTF-8.
    /// @throws io_error when the file cannot be opened (errc::open_failed) or is not a regular file, such as a
    ///         directory, the root of a drive or a share among them, or a device (errc::not_a_regular_file).
    explicit file_source(std::string_view path);
    ~file_source() override;

    [[nodiscard]] std::uint64_t size() const override;
    void read(std::uint64_t offset, std::span<std::byte> destination) const override;
    /// True.
    [[nodiscard]] bool supports_concurrent_reads() const override;
    /// The path.
    [[nodiscard]] std::string description() const override;

private:
    struct state;
    std::unique_ptr<const state> state_;
};

/// Options of file_sink.
struct file_sink_options
{
    /// Flush the file to the storage device before it replaces the target, so that it survives a power failure.
    /// Slower.
    bool flush_to_disk = false;
    /// Replace a target that exists. With false, the sink refuses one, when it is made and again in finish(), whose
    /// rename then never replaces a file that another program created meanwhile.
    bool replace = true;
};

/// A file that is replaced atomically. The output goes to a temporary file in the target's directory, which replaces
/// the target when finish() succeeds. Until then the target keeps its old content, or does not exist. A sink destroyed
/// before finish(), after a failure or a cancellation, removes its temporary file, and so does a finish() that fails.
///
/// The temporary file is named `.openfits-<16 hexadecimal digits>.tmp`. Only a process that ends abruptly leaves one
/// behind. Supports rewrite().
///
/// The new file is created like any new file: it has the permissions that the system gives one, not those of the file
/// it replaces. A target that exists must be a regular file or a link: a directory, a FIFO, a socket or a device is
/// refused before anything is created, and again by finish(), since another program can change the target meanwhile.
/// A target that is a symbolic link, or a junction on Windows, is itself replaced by the new file; what it points to is
/// left as it is. On Windows a directory that is a reparse point of another kind, such as the placeholder of a cloud
/// file system, is a directory, and a target that another program has open is replaced only when that program let
/// others delete it (`FILE_SHARE_DELETE`), as file_source does, and a target with the read-only attribute is not
/// replaced; otherwise finish() fails. That needs a file system that renames with POSIX semantics, as NTFS does from
/// Windows 10 1607 on: on others, such as FAT32, exFAT and some network shares, finish() fails while any program has
/// the target open, a reader of the file being replaced included. POSIX systems replace a target that cannot be
/// written, as the directory allows.
class OPENFITS_API file_sink final : public output_sink
{
public:
    /// Creates the temporary file for a target at path, which is UTF-8 on every platform and the name of the file,
    /// brackets included. A relative path is taken from the current directory now: finish() replaces that target,
    /// whatever the current directory is by then.
    /// @throws usage_error when path is empty or not valid UTF-8.
    /// @throws io_error when the target is a directory, a FIFO, a socket or a device, such as /dev/null, or on Windows
    ///         NUL in any directory or a path of the device namespace that names no file (errc::not_a_regular_file);
    ///         when the temporary file cannot be created, or the current directory of a relative path cannot be found
    ///         (errc::open_failed); when the system provides no random data for its name
    ///         (errc::entropy_unavailable); or when something exists at path and options.replace is false
    ///         (errc::file_exists).
    explicit file_sink(std::string_view path, file_sink_options options = {});
    ~file_sink() override;

    void write(std::span<const std::byte> data) override;
    [[nodiscard]] std::uint64_t position() const override;
    /// True.
    [[nodiscard]] bool can_rewrite() const override;
    void rewrite(std::uint64_t offset, std::span<const std::byte> data) override;
    /// Replaces the target with the temporary file. When it throws, the temporary file is gone, and the target keeps
    /// its old content unless the failure was the flush to disk that follows the replacement.
    /// @throws io_error when the target has become a directory, a FIFO, a socket or a device since the sink was made
    ///         (errc::not_a_regular_file), cannot be replaced (errc::write_failed), or exists while the options of the
    ///         sink refuse to replace it (errc::file_exists).
    void finish() override;

private:
    struct state;
    std::unique_ptr<state> state_;
};

/// An existing regular file, read and written in place with positional I/O, so that any number of threads can read and
/// write it at once, at ranges that do not overlap, and made shorter or longer with resize().
///
/// The file stays open until the device is destroyed. Other programs may still read, write, rename or delete it, as for
/// file_source.
class OPENFITS_API file_device final : public random_access_device
{
public:
    /// Opens the file at path, which is UTF-8 on every platform, for reading and writing. What the system says is not
    /// a regular file is refused before it is opened, as file_source refuses it.
    /// @throws usage_error when path is empty or not valid UTF-8.
    /// @throws io_error when the file cannot be opened for reading and writing, such as a file that does not exist or
    ///         that the process may not write (errc::open_failed), or is not a regular file, such as a directory or a
    ///         device (errc::not_a_regular_file).
    explicit file_device(std::string_view path);
    ~file_device() override;

    [[nodiscard]] std::uint64_t size() const override;
    void read(std::uint64_t offset, std::span<std::byte> destination) const override;
    /// Throws usage_error when the bytes go past the end of the file, which the file then never sees.
    void write(std::uint64_t offset, std::span<const std::byte> data) override;
    /// True.
    [[nodiscard]] bool can_resize() const override;
    /// Truncates or extends the file; the bytes that extend it read as zeros.
    /// @throws io_error (errc::write_failed) when the system refuses the size, which the file then keeps.
    void resize(std::uint64_t size) override;
    /// Flushes the file to the storage device, so that what was written survives a power failure.
    void flush() override;
    /// True.
    [[nodiscard]] bool supports_concurrent_reads() const override;
    /// True.
    [[nodiscard]] bool supports_concurrent_writes() const override;
    /// The path.
    [[nodiscard]] std::string description() const override;

private:
    struct state;
    std::unique_ptr<state> state_;
};

/// Bytes in memory, borrowed from the caller or owned by the source. Any number of threads can read it at once.
class OPENFITS_API memory_source final : public input_source
{
public:
    /// Borrows data, which must outlive the source.
    explicit memory_source(std::span<const std::byte> data) noexcept;
    /// Owns data. A vector that is not moved in is copied, so pass a std::span to borrow it instead.
    explicit memory_source(std::vector<std::byte> data) noexcept;

    [[nodiscard]] std::uint64_t size() const override;
    void read(std::uint64_t offset, std::span<std::byte> destination) const override;
    /// True.
    [[nodiscard]] bool supports_concurrent_reads() const override;
    /// The bytes, borrowed or owned.
    [[nodiscard]] std::span<const std::byte> bytes() const override;

private:
    std::vector<std::byte> owned_;
    std::span<const std::byte> data_;
};

/// Bytes in memory, in a buffer that grows as needed. Supports rewrite().
class OPENFITS_API memory_sink final : public output_sink
{
public:
    void write(std::span<const std::byte> data) override;
    [[nodiscard]] std::uint64_t position() const override;
    /// True.
    [[nodiscard]] bool can_rewrite() const override;
    void rewrite(std::uint64_t offset, std::span<const std::byte> data) override;

    /// The bytes written so far.
    [[nodiscard]] std::span<const std::byte> data() const noexcept;
    /// Moves the bytes written so far out of the sink, which is empty afterwards.
    [[nodiscard]] std::vector<std::byte> release() noexcept;

private:
    std::vector<std::byte> data_;
};

/// Bytes in memory, read and written in place: a buffer of the caller, whose size never changes, or a vector that the
/// device owns, which resize() makes shorter or longer. Any number of threads can read and write it at once, at ranges
/// that do not overlap.
class OPENFITS_API memory_device final : public random_access_device
{
public:
    /// Borrows data, which must outlive the device, and which the device reads and writes.
    explicit memory_device(std::span<std::byte> data) noexcept;
    /// Owns data, which must be moved in: a vector given by name is borrowed, as a std::span, and keeps its size.
    explicit memory_device(std::vector<std::byte>&& data) noexcept;

    [[nodiscard]] std::uint64_t size() const override;
    void read(std::uint64_t offset, std::span<std::byte> destination) const override;
    /// Throws usage_error when the bytes go past the end of the buffer, which then stays as it was.
    void write(std::uint64_t offset, std::span<const std::byte> data) override;
    /// True when the device owns its bytes.
    [[nodiscard]] bool can_resize() const override;
    /// Throws usage_error when the device borrows its bytes, and std::bad_alloc when memory runs out; the bytes stay as
    /// they were then.
    void resize(std::uint64_t size) override;
    /// True.
    [[nodiscard]] bool supports_concurrent_reads() const override;
    /// True.
    [[nodiscard]] bool supports_concurrent_writes() const override;

    /// The bytes, borrowed or owned.
    [[nodiscard]] std::span<const std::byte> data() const noexcept;
    /// Moves the bytes out of a device that owns them, which is empty afterwards. A device that borrows its bytes keeps
    /// them, and gives an empty vector.
    [[nodiscard]] std::vector<std::byte> release() noexcept;

private:
    std::vector<std::byte> owned_;
    std::span<std::byte> data_;
    bool owns_ = false;
};

/// Options of callback_source.
struct callback_source_options
{
    /// The read function may run on several threads at once.
    bool concurrent_reads = false;
    /// Names the source in error messages.
    std::string description{};
};

/// A source that calls a function of the application to read. The function is never asked for bytes beyond the size.
class OPENFITS_API callback_source final : public input_source
{
public:
    /// Fills destination with the bytes that start at offset, as input_source::read() does.
    using read_function = std::function<void(std::uint64_t offset, std::span<std::byte> destination)>;

    /// A source of size bytes that reads with read.
    /// @throws usage_error when read is empty.
    callback_source(std::uint64_t size, read_function read, callback_source_options options = {});

    [[nodiscard]] std::uint64_t size() const override;
    /// Calls the read function, whose exceptions pass through unchanged.
    void read(std::uint64_t offset, std::span<std::byte> destination) const override;
    [[nodiscard]] bool supports_concurrent_reads() const override;
    [[nodiscard]] std::string description() const override;

private:
    std::uint64_t size_;
    read_function read_;
    callback_source_options options_;
};

/// A sink that calls functions of the application to write. Their exceptions pass through unchanged, and position()
/// counts the bytes of the write calls that returned.
class OPENFITS_API callback_sink final : public output_sink
{
public:
    /// Appends data, as output_sink::write() does.
    using write_function = std::function<void(std::span<const std::byte> data)>;
    /// Overwrites bytes already written, as output_sink::rewrite() does. Never asked for bytes beyond position().
    using rewrite_function = std::function<void(std::uint64_t offset, std::span<const std::byte> data)>;
    /// Completes the output, as output_sink::finish() does.
    using finish_function = std::function<void()>;

    /// A sink that appends with write. It supports rewrite() when a rewrite function is given, and finish() calls the
    /// finish function when one is given.
    /// @throws usage_error when write is empty.
    explicit callback_sink(write_function write, rewrite_function rewrite = {}, finish_function finish = {});

    void write(std::span<const std::byte> data) override;
    [[nodiscard]] std::uint64_t position() const override;
    [[nodiscard]] bool can_rewrite() const override;
    void rewrite(std::uint64_t offset, std::span<const std::byte> data) override;
    void finish() override;

private:
    write_function write_;
    rewrite_function rewrite_;
    finish_function finish_;
    std::uint64_t position_ = 0;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace openfits
