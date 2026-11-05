// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/card.h>
#include <openfits/compression.h>
#include <openfits/image.h>
#include <openfits/io.h>
#include <openfits/types.h>
#include <openfits/writer.h>

#include "compress/image_compress.h"
#include "hdu/copy.h"
#include "table/table_write.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The state of a writer: its units, and how a save formats and writes them. The editor writes the units of a writer
// into a file too, at their place in it.

namespace openfits {

namespace detail {

// Where the units of a save go in a file: from the position first on, and as extensions only when extension, the
// primary HDUs that the writer adds itself left out, and a compressed image never the primary image.
struct unit_placement
{
    std::size_t first = 0;
    bool extension = false;
};

} // namespace detail

struct writer::state
{
    // A unit as it is added: an image, its model and its values, borrowed, given by a function or reserved, or none for
    // a header alone; a table instead when it has one, its values borrowed or its rows given by a function; or a copy
    // of a unit of a file.
    struct unit
    {
        image_info image{};
        numeric_type type = numeric_type::uint8;
        std::span<const std::byte> data{};
        image_fill fill{};
        image_write_options options{};
        std::optional<reserved_fill> reserved{};
        std::optional<detail::table_model> table{};
        table_rows rows{};
        std::optional<detail::unit_copy> copy{};
        // The primary HDU without data that a first image compressed by its own options comes after, whose image the
        // table names as the primary one (standard §10.1.2).
        bool hosts_compressed_primary = false;
        // A primary HDU without data that the writer adds itself, before a table, a copy of an extension or a
        // compressed image that come first: units placed after the first unit of a file leave it out.
        bool implicit = false;
    };

    // A unit of the file of a save: a unit of the writer, compressed or not, or the primary HDU without data that a
    // compressed first image needs (standard §10.1).
    struct file_unit
    {
        std::optional<std::size_t> source{};
        std::optional<detail::compressed_unit> compressed{};
        // The copy of a unit as it takes its place in the file, converted to it.
        std::optional<detail::unit_copy> copy{};
        // The bytes of its data records; nothing for a streamed table whose rows or heap are known once written.
        std::optional<std::uint64_t> data{};
        // Its header holds the records of checksum_cards(), which the save completes once its data are written.
        bool sealed = false;
    };

    // The units of the file of a save, their headers formatted, the bytes of the file and of the data of its units,
    // those of streamed tables whose size is known once they are written left out; and the place of the first unit in
    // the file.
    struct formatted_file
    {
        std::vector<file_unit> units{};
        std::vector<std::string> headers{};
        std::uint64_t size = 0;
        std::uint64_t data = 0;
        bool size_known = true;
        std::size_t first = 0;
    };

    std::vector<unit> units{};

    // The compression of the unit at index: its own, or else that of options; none for a unit that has no data to
    // compress, an image without values, a reserved image, a streamed table or a copy.
    [[nodiscard]] std::optional<compression_options> compression_of(std::size_t index,
                                                                    const write_options& options) const;

    // The bytes of the stored values of the units that a save with options compresses.
    [[nodiscard]] std::uint64_t compressed_bytes(const write_options& options) const;

    // The file of a save with options, its units compressed, advance receiving the bytes of the values compressed, at
    // the place in a file that where gives.
    [[nodiscard]] formatted_file format(const write_options& options,
                                        const std::function<void(std::uint64_t)>& advance = {},
                                        detail::unit_placement where = {}) const;

    // The header of a unit of the file, at position in it, as a save with options writes it, in whole blocks, closing
    // with the cards of closing.
    [[nodiscard]] std::string format_header(const file_unit& entry, std::size_t position, const write_options& options,
                                            std::span<const card> closing) const;

    // The header of a table unit at position, with the cards of the model table.
    [[nodiscard]] static std::string table_header(const detail::table_model& table, const unit& model,
                                                  std::size_t position, const write_options& options,
                                                  std::span<const card> closing);

    // Writes the data of a unit of the file into sink, the fill of the last block included; for a streamed table, what
    // the write found, which completes its header.
    std::optional<detail::streamed_table> write_data(output_sink& sink, const file_unit& entry, std::size_t position,
                                                     const std::function<void(std::uint64_t)>& advance) const;

    // Writes the unit at index of file into sink, at its place: its header, then its data, and, when sink can rewrite,
    // its header completed in place once they are written where it needs it, its checksums and the NAXIS2 or the heap
    // of a streamed table. advance receives the bytes of each write.
    void write_unit(output_sink& sink, formatted_file& file, std::size_t index, const write_options& options,
                    const std::function<void(std::uint64_t)>& advance) const;

    // The unit of the writer that a unit of the file writes, or nothing for the primary HDU that a save adds.
    [[nodiscard]] const unit* model_of(const file_unit& entry) const noexcept
    {
        return entry.source ? &units[*entry.source] : nullptr;
    }

    // Throws usage_error for a unit whose header a save with options completes once its streamed data are written,
    // which a sink that cannot rewrite refuses: the checksums of uncompressed streamed data, which a pass of their own
    // would ask the function for again, and the NAXIS2 or the heap of a streamed table.
    void check_streams(const write_options& options) const;

    // True for a streamed table whose header is completed once its rows are written: their number, or its heap.
    [[nodiscard]] bool completes_table(const file_unit& entry) const noexcept
    {
        const unit* model = model_of(entry);
        return model != nullptr && model->rows && !entry.data;
    }

    // Writes the file to sink, as writer::save() does, without gzip.
    void save(output_sink& sink, const write_options& options) const;
};

} // namespace openfits
