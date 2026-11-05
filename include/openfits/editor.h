// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/card.h>
#include <openfits/error.h>
#include <openfits/export.h>
#include <openfits/hdu.h>
#include <openfits/header.h>
#include <openfits/io.h>
#include <openfits/limits.h>
#include <openfits/reader.h>
#include <openfits/table.h>
#include <openfits/types.h>
#include <openfits/writer.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

/// @file
/// Editing FITS files: keywords, values, rows and units changed, then committed together, in place where the layout of
/// the file allows it.

namespace openfits {

/// How a commit that changes the layout of a file, where its units lie, rewrites it.
enum class relayout : std::uint8_t
{
    /// Through a copy: the new file is written to a temporary file in the directory of the file, which replaces it by
    /// an atomic rename once it is complete, so that a failure, a cancellation or a crash leaves the old file or the
    /// new one, never a mix. An editor of a device has no directory: it writes the new file into memory, within
    /// limits::max_allocation, and then over the device, which a failure from then on leaves damaged.
    safe_copy,
    /// In place: the units after the first change are moved within the file, in chunks, which needs no room for a copy,
    /// for files too large to copy. A failure, a cancellation or a crash while they move leaves the file damaged, and
    /// the editor refuses every later change (errc::not_editable).
    in_place,
};

/// Options of an editor.
struct editor_options
{
    /// The safety limits of the file, which is untrusted input.
    openfits::limits limits{};
    /// How a commit that changes the layout of the file rewrites it.
    openfits::relayout relayout = openfits::relayout::safe_copy;
    /// Apply every commit through a copy, as relayout::safe_copy applies a change of layout, so that a failure, a
    /// cancellation or a crash leaves the old file or the new one for the changes made in place too.
    bool atomic = false;
    /// Update CHECKSUM and DATASUM of a unit whose header or data change, those of them that it has, from the bytes
    /// changed and what the keywords recorded (standard §4.4.2.7, Appendix J.4), so that a unit that matched them still
    /// does, and one that did not still does not. A DATASUM that records no sum, and a blank one, stay as they are.
    bool update_checksums = true;
    /// How the records of the cards that a change makes are written, and the units that the editor adds: the HIERARCH
    /// convention, LONGSTRN, comments cut, long strings of reserved keywords, bytes outside printable ASCII, the
    /// checksums and the compression of the units added, the time of their checksums, and a flush to the storage device
    /// at each commit. if_exists, gzip_level and progress are not used: a commit takes its own progress.
    write_options write{};
};

/// Options of a commit.
struct commit_options
{
    /// Called as the file is written, and able to cancel the commit. Its units of work are the bytes that the commit
    /// writes, those of a copy of the file when it makes one.
    progress_function progress{};
};

/// Options of the writes of the values of an image (editor::write_pixels()). Each member has the meaning of its
/// counterpart in pixel_read_options, for the values given instead of a destination.
struct pixel_write_options
{
    /// The region of the image to write: its elements alone, in the order of their axes, the first varying fastest, an
    /// axis of a negative step in reverse. The whole image when empty.
    std::optional<openfits::region> region{};
    /// The values from the start of one row of the values given to the start of the next, at least the length of a row
    /// of the region; 0 for rows that follow each other.
    std::uint64_t pitch = 0;
    /// What the values are: physical values, which the scaling of the image converts into stored ones; the stored
    /// values themselves; or values that override_scaling converts into stored ones.
    openfits::scaling_policy scaling = openfits::scaling_policy::apply;
    /// The scaling of scaling_policy::override.
    openfits::scaling override_scaling{};
    /// What a value beyond the range of the stored type does.
    openfits::overflow_policy overflow = openfits::overflow_policy::refuse;
};

/// Options of the writes of the fields of a table column (editor::write_cells()).
struct cell_write_options
{
    /// The row of the first field written, counted from 0.
    std::uint64_t first_row = 0;
    /// What the numbers are: physical values, which TSCALn and TZEROn convert into stored ones (scaling_policy::apply),
    /// or the stored values themselves (scaling_policy::raw).
    openfits::scaling_policy scaling = openfits::scaling_policy::apply;
    /// What a value beyond the range of the stored type, or of the field of an ASCII table, does.
    openfits::overflow_policy overflow = openfits::overflow_policy::refuse;
};

/// Options of the rows appended to a table (editor::append_rows()).
struct row_append_options
{
    /// What the numbers are, as cell_write_options::scaling says.
    openfits::scaling_policy scaling = openfits::scaling_policy::apply;
    /// What a value beyond the range of the stored type, or of the field of an ASCII table, does.
    openfits::overflow_policy overflow = openfits::overflow_policy::refuse;
};

#if defined(_MSC_VER)
// The editor holds a standard-library member, which cl reports for an exported class. A C++ interface requires the
// same standard library on both sides of the DLL boundary anyway.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

/// A FITS file opened to be changed: the keywords of its headers, the values of its images and tables, its rows, and
/// its units, added, inserted, replaced and erased (standard §3).
///
/// Every change is pending until commit(), which applies them all at once, and every index is that of a unit of the
/// file as the editor opened it or as the last commit left it, whatever changes are pending: a unit inserted before
/// another one leaves its index as it is until the commit. file() reads the file as it is, the pending changes left
/// out. discard() drops the pending changes.
///
/// A commit writes in place what keeps the layout of the file: a header that fits its blocks, the blank cards before
/// END taking the cards added (standard §4.4.2.4), values written into images and tables, rows appended to the table
/// that ends the file when it has no heap, and units appended at the end. Each such write touches the bytes it changes,
/// and a header the blocks that change. Any other change moves units: a header that grows by a block, a unit inserted,
/// replaced or erased, rows appended to another table, values written into a compressed image, whose tiles change size.
/// Such a commit rewrites the file as editor_options::relayout says, through a copy by default, which replaces the file
/// at once. Writes in place are not atomic: a failure, a cancellation or a crash may leave some of them applied, which
/// editor_options::atomic avoids by applying every commit through a copy. The pending changes stay when a commit
/// throws, and the next commit applies them again, which completes what a failed commit left half done.
///
/// A unit whose header or data change has its CHECKSUM and DATASUM updated as editor_options::update_checksums says,
/// from the old and new bytes alone, with no pass over the unit; a unit that a commit builds again, such as a
/// compressed image whose tiles are encoded again or a table whose rows it rewrites, is summed as it is written. DATE
/// is never changed.
///
/// The editor walks every unit when it opens the file, since it needs the whole layout, and refuses a file whose walk
/// stops at a unit whose header gives no size, and a gzip-compressed file, whose bytes are not those of the FITS file
/// (errc::not_editable). It owns its file, which stays open as long as the editor exists.
///
/// Thread safety: an editor is thread-compatible, like a standard container: its const member functions can run on
/// several threads at once, and a change needs exclusive access; file() is a reader, whose const member functions can
/// run on any number of threads at once, until the next commit. Another reader of the file while an editor changes it
/// reads each byte as it is then: no unit is guaranteed to be complete or consistent before the commit returns. An
/// editor can be moved, and a moved-from editor can only be destroyed or assigned to.
class OPENFITS_API editor
{
public:
    /// Opens the file at path, which is UTF-8 on every platform and the name of the file, brackets included, for
    /// reading and writing (file_device), and walks every unit.
    /// @throws usage_error when path is empty or not valid UTF-8; io_error when the file cannot be opened for reading
    ///         and writing, as file_device throws it.
    /// @throws unsupported_error (errc::not_editable) for a gzip-compressed file.
    /// @throws invalid_data_error (errc::not_a_fits_file) for a file that is not a FITS file; invalid_data_error or
    ///         limit_error, with the code of the problem, when the walk stops at a unit whose header gives no size or
    ///         beyond the limits.
    explicit editor(std::string_view path, const editor_options& options = {});

    /// Opens the file of a file specification, as the other constructor opens a path, and selects its unit, which
    /// selected_hdu() gives.
    /// @throws usage_error (errc::invalid_file_specification) for a specification with an image section, which an
    ///         editor does not take, and (errc::hdu_not_found) when the file has no such unit; and the exceptions of
    ///         the other constructor.
    explicit editor(const file_specification& specification, const editor_options& options = {});

    /// Opens the file that device holds, which the editor then owns, as the other constructors open a path. Exceptions
    /// of the device pass through unchanged.
    /// @throws usage_error when device is null.
    explicit editor(std::unique_ptr<random_access_device> device, const editor_options& options = {});

    editor(editor&& other) noexcept;
    editor& operator=(editor&& other) noexcept;
    editor(const editor&) = delete;
    editor& operator=(const editor&) = delete;
    ~editor();

    /// The file as it is, the pending changes left out: a reader of the file of the editor, every unit walked. The
    /// reader is the same object for as long as the editor exists, and reads the file as each commit leaves it; the
    /// references to its units and their headers are valid until the next commit.
    [[nodiscard]] const reader& file() const noexcept;

    /// The unit that a file specification selected, as the file was opened; 0 without one.
    [[nodiscard]] std::size_t selected_hdu() const noexcept;

    /// The header of the unit at index to change, which a commit writes: the cards of the unit as it was read, with the
    /// changes made since, its structural keywords included (is_structural()), which must stay as they are, as must the
    /// order of the mandatory keywords that start it (standard §4.4.1): a commit refuses a header that changes them,
    /// CHECKSUM and DATASUM excepted. A card read from the file is written back as it was read. The header of a
    /// compressed image or table is that of the image or the table that it holds, as reader::image() and
    /// reader::table() give it (standard §10.1.2, §10.3.1), which a commit writes back into the header of the table
    /// through its Z keywords: its CHECKSUM and DATASUM as ZHECKSUM and ZDATASUM.
    /// @throws usage_error (errc::hdu_not_found) when the file has no unit at index, (errc::invalid_argument) for a
    ///         unit erased or replaced by a pending change; and the exceptions of reader::image() and reader::table()
    ///         for a compressed unit.
    [[nodiscard]] openfits::header& header(std::size_t index);

    /// Writes values of T, physical values by default, into the image at index, or the region of options, in place: the
    /// values of the region in its order, exactly as many, or with options.pitch at least those from the first element
    /// of its first row to the last of its last row, as reader::read_pixels() fills a destination. The values are
    /// converted into the stored values of the image now, and the commit writes them: rounded half away from zero into
    /// integer data, NaN stored as BLANK. A compressed image (standard §10.1) has each tile that its regions meet
    /// encoded again by the commit, a quantized tile at the spacing of its levels (standard §10.2).
    /// @throws usage_error (errc::not_an_image) for a unit that holds no image, and (errc::invalid_argument) when
    ///         the region, the pitch or the values do not fit the image, or options.override_scaling has a scale of 0
    ///         or a value that is not finite; and those of header() for the index.
    /// @throws invalid_data_error (errc::value_out_of_range) for a value beyond the stored type that options do not
    ///         clamp, and (errc::undefined_value) for a NaN in integer data without BLANK.
    /// @throws limit_error (errc::allocation_too_large) when the stored values take more than limits::max_allocation.
    template <numeric_value T>
    void write_pixels(std::size_t index, std::span<const T> values, const pixel_write_options& options = {})
    {
        write_values(index, numeric_type_of<T>(), std::as_bytes(values), options);
    }

    /// Writes values that are not const, such as std::span(values) of a std::vector that is not const, as the overload
    /// for const values does.
    template <numeric_value T>
    void write_pixels(std::size_t index, std::span<T> values, const pixel_write_options& options = {})
    {
        write_pixels(index, std::span<const T>(values), options);
    }

    /// Writes values of type, given as their bytes in the byte order of the host at an address that is a multiple of
    /// their size, as write_pixels<T>() writes values of T.
    /// @throws usage_error (errc::invalid_argument) when the size or the address of values is not a multiple of the
    ///         size of the values of type; and the exceptions of write_pixels<T>().
    void write_pixels(std::size_t index, numeric_type type, std::span<const std::byte> values,
                      const pixel_write_options& options = {});

    /// Writes the undefined value into the image at index, or into its region area: BLANK in integer data, which the
    /// image must have, and a NaN in floating-point data (standard §4.4.2.5, §5.3).
    /// @throws usage_error (errc::undefined_value) for integer data without BLANK; and the exceptions of
    ///         write_pixels() for the index and the region.
    void write_blank(std::size_t index, const std::optional<region>& area = {});

    /// Writes bytes into the data of the unit at index, any kind of unit, from offset on, counted from the start of its
    /// data: the bytes as they are stored, big-endian, as reader::read_raw() reads them.
    /// @throws usage_error (errc::invalid_argument) when the bytes are not within reader::hdu(index).data_size(); and
    ///         those of header() for the index.
    void write_raw(std::size_t index, std::uint64_t offset, std::span<const std::byte> bytes);

    /// Writes values into the fields of the column at index column of the table at index, an ASCII or a binary table,
    /// from the row options.first_row on: as many rows as the values fill fields, each field as writer::add_table()
    /// stores it, converted now and written by the commit.
    /// @throws usage_error (errc::not_a_table) for a unit that holds no table, (errc::not_editable) for a compressed
    ///         table, whose fields change size, or a column of variable-length arrays, which replace_hdu() changes,
    ///         (errc::column_not_found) for a column that the table does not have, and (errc::invalid_argument) for
    ///         rows beyond the table; and the exceptions of writer::add_table() for values that the column cannot hold.
    void write_cells(std::size_t index, std::size_t column, const column_values& values,
                     const cell_write_options& options = {});

    /// write_cells() of the first column named name, compared as table_info::find_column() compares names.
    void write_cells(std::size_t index, std::string_view name, const column_values& values,
                     const cell_write_options& options = {});

    /// Appends rows to the table at index, an ASCII or a binary table, after its rows: a column_values for each of its
    /// columns, in their order, with the values of the same number of rows, stored as writer::add_table() stores them,
    /// the arrays of variable-length columns after its heap. NAXIS2 counts them, and the heap, PCOUNT and the emax of
    /// TFORMn grow with their arrays. The commit extends the table in place when it ends the file and has no heap,
    /// and rewrites it otherwise, its heap after its rows.
    /// @throws usage_error (errc::not_a_table) for a unit that holds no table, (errc::not_editable) for a compressed
    ///         table, and the exceptions of writer::add_table() for values that the columns cannot hold.
    void append_rows(std::size_t index, std::span<const column_values> columns, const row_append_options& options = {});

    /// Appends the units of a writer after the last unit of the file, as extensions: a primary HDU of an image becomes
    /// an IMAGE extension, as writer::add_hdu() converts a unit placed after the first, and the primary HDU without
    /// data that a writer adds itself before a table or a compressed image is left out. The writer is kept until the
    /// commit, which writes its units as writer::save() writes them, with editor_options::write, and the values that it
    /// borrows must stay valid until then. Returns the index of its first unit in the file as the pending changes so
    /// far make it.
    /// @throws usage_error (errc::invalid_argument) for a writer without units, or with random groups, which only the
    ///         primary HDU holds (standard §6).
    std::size_t append(writer units);

    /// Appends a copy of the unit at index of source, as writer::add_hdu() copies it, which source must outlive the
    /// commit.
    /// @throws the exceptions of writer::add_hdu() and of append().
    std::size_t append(const reader& source, std::size_t index);

    /// Inserts the units of a writer before the unit at index, or after the last one for the number of units, as
    /// append() places them; at index 0 they are placed as writer::save() places them, the first of them the primary
    /// HDU, and the primary HDU of the file becomes an IMAGE extension.
    /// @throws usage_error (errc::hdu_not_found) beyond the number of units, (errc::invalid_argument) for random groups
    ///         after the first unit; and the exceptions of append().
    void insert_hdu(std::size_t index, writer units);

    /// Replaces the unit at index with the units of a writer, placed as insert_hdu() places them.
    /// @throws the exceptions of insert_hdu() and of erase_hdu().
    void replace_hdu(std::size_t index, writer units);

    /// Erases the unit at index. When it is the primary HDU, the unit after it becomes the primary HDU, an IMAGE
    /// extension converted to it, and before a table a primary HDU without data comes first.
    /// @throws usage_error (errc::hdu_not_found) when the file has no unit at index, and (errc::invalid_argument) when
    ///         the commit would leave the file without units, or with random groups after the first unit.
    void erase_hdu(std::size_t index);

    /// True when a change is pending.
    [[nodiscard]] bool has_pending_changes() const noexcept;

    /// Applies every pending change, in one pass: the writes in place, then the units appended, or the whole layout
    /// through editor_options::relayout when it changes, flushed to the storage device when the write options of the
    /// editor ask for it (write_options::flush_to_disk). Headers are checked, records formatted,
    /// values of compressed images encoded again and every size computed before the file is written. The pending
    /// changes are dropped once it returns, and file() reads the file as it then is.
    /// @throws validation_error for a header that changes its structural keywords (errc::structural_keyword), or a card
    ///         that no record can hold, before anything is written.
    /// @throws usage_error (errc::invalid_argument) for a device that cannot change its size when the file must; and
    ///         (errc::not_editable) after a failed commit in place that moved units, which left the file damaged.
    /// @throws io_error, or what a device throws, when the file cannot be written, read or replaced; io_error
    ///         (errc::end_of_data) when the commit would write data that a truncated file lacks, for a unit appended
    ///         after them or a write among them, which are not made up.
    /// @throws cancelled_error when options.progress returns false.
    void commit(const commit_options& options = {});

    /// Drops every pending change.
    void discard() noexcept;

private:
    void write_values(std::size_t index, numeric_type type, std::span<const std::byte> values,
                      const pixel_write_options& options);

    struct state;
    std::unique_ptr<state> state_;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/// Sets cards in the header of the unit at index hdu of the file at path, and commits: each card replaces the first
/// card of its keyword, and the CONTINUE records of its string, or is added after the last card that is not fill;
/// COMMENT, HISTORY and blank cards are added. The common case of an editor, in one call.
/// @throws the exceptions of editor() and of editor::commit().
OPENFITS_API void update_keywords(std::string_view path, std::size_t hdu, std::span<const card> cards,
                                  const editor_options& options = {});

} // namespace openfits
