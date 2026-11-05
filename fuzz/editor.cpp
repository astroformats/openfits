// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Random sequences of edits (standard §3, §4.4.2.7, §10.1). With the first byte even, the input builds a file of a
// 16-bit image, a binary table and a compressed image, with checksums, then gives a sequence of changes: keywords set
// and erased in the three headers, values written into regions of the images and into the fields of the table, rows
// appended, units appended and inserted, and commits between them, with the strategy that the input picks. Once
// committed, the file must open strictly, every checksum matching its unit, conform to the standard, and hold the
// values of a model of the changes. With the first byte odd, the rest of the input is a file, hostile or not, which
// the editor opens and changes: every failure must be an error of the library's kinds, and a file that a commit
// writes must open again.

#include <openfits/compression.h>
#include <openfits/editor.h>
#include <openfits/error.h>
#include <openfits/header.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/table.h>
#include <openfits/verify.h>
#include <openfits/writer.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

void require(bool condition)
{
    if (!condition) {
        std::abort();
    }
}

class input_bytes
{
public:
    explicit input_bytes(std::span<const std::byte> data) : data_(data) {}

    std::uint8_t next()
    {
        return at_ < data_.size() ? std::to_integer<std::uint8_t>(data_[at_++]) : 0;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return at_ >= data_.size();
    }

    [[nodiscard]] std::span<const std::byte> rest() const noexcept
    {
        return data_.subspan(std::min(at_, data_.size()));
    }

private:
    std::span<const std::byte> data_;
    std::size_t at_ = 0;
};

constexpr std::uint64_t width = 32;
constexpr std::uint64_t height = 16;
constexpr int most_steps = 48;

// What the file holds as the changes make it: the values of the image and of the compressed image, the column of the
// table, and where each unit is.
struct model
{
    std::vector<std::uint16_t> image{};
    std::vector<std::uint16_t> packed{};
    std::vector<std::int32_t> column{};
    // The rows of the table in the file as the last commit left it, which the writes into its fields reach.
    std::size_t committed_rows = 0;
    std::size_t table_index = 1;
    std::size_t packed_index = 2;
    std::size_t units = 3;
    // The units that the changes since the last commit insert before the table and append.
    std::size_t inserted = 0;
    std::size_t appended = 0;
};

std::int32_t small_number(input_bytes& in)
{
    return static_cast<std::int32_t>(in.next()) - 128;
}

std::vector<std::byte> initial_file(input_bytes& in, model& state)
{
    // Values from a generator that the input seeds, so that the bytes of the input go to the changes.
    std::uint32_t generator = 1U + in.next();
    state.image.resize(width * height);
    state.packed.resize(width * height);
    for (std::size_t i = 0; i < state.image.size(); ++i) {
        generator = (generator * 1664525U) + 1013904223U;
        state.image[i] = static_cast<std::uint16_t>(generator >> 16U);
        state.packed[i] = static_cast<std::uint16_t>((i * 3) + (generator >> 28U));
    }
    state.column.resize(1U + (in.next() % 6U));
    for (std::int32_t& value : state.column) {
        value = small_number(in);
    }
    state.committed_rows = state.column.size();
    const std::size_t reserved = in.next() % 4U;
    // The table ends the file, where rows are appended in place, or comes before the compressed image.
    const bool table_last = (in.next() & 1U) != 0;
    if (table_last) {
        state.table_index = 2;
        state.packed_index = 1;
    }

    openfits::image_info info;
    info.geometry.axes = {width, height};
    info.header.set("OBJECT", "fuzzed");
    openfits::table_info table{.kind = openfits::hdu_kind::bintable, .rows = state.column.size()};
    table.columns.push_back({.name = "N"});
    const std::vector<openfits::column_values> columns{std::span<const std::int32_t>(state.column)};
    openfits::writer output;
    (void)output.add_image(info, std::span<const std::uint16_t>(state.image), {.reserved_cards = reserved});
    const openfits::image_write_options compressed{
        .compression = openfits::compression_options{.type = openfits::compression::rice, .tile = {8, 8}}};
    if (table_last) {
        (void)output.add_image(info, std::span<const std::uint16_t>(state.packed), compressed);
        (void)output.add_table(table, columns);
    } else {
        (void)output.add_table(table, columns);
        (void)output.add_image(info, std::span<const std::uint16_t>(state.packed), compressed);
    }
    openfits::memory_sink sink;
    output.save(sink, {.checksums = true});
    return sink.release();
}

// A region of an image from the input, and values for it.
struct region_write
{
    openfits::region area{};
    std::vector<std::uint16_t> values{};
};

region_write region_of(input_bytes& in)
{
    const std::uint64_t x = in.next() % width;
    const std::uint64_t y = in.next() % height;
    const std::uint64_t w = 1 + (in.next() % (width - x));
    const std::uint64_t h = 1 + (in.next() % (height - y));
    region_write result{.area = {.start = {x, y}, .size = {w, h}}};
    result.values.resize(w * h);
    for (std::uint16_t& value : result.values) {
        const unsigned high = in.next();
        const unsigned low = in.next();
        value = static_cast<std::uint16_t>((high << 8U) | low);
    }
    return result;
}

void lay(std::vector<std::uint16_t>& image, const region_write& write)
{
    const std::uint64_t x = write.area.start[0];
    const std::uint64_t y = write.area.start[1];
    const std::uint64_t w = write.area.size[0];
    for (std::size_t i = 0; i < write.values.size(); ++i) {
        image[((y + (i / w)) * width) + x + (i % w)] = write.values[i];
    }
}

void commit(openfits::editor& file, model& state)
{
    file.commit();
    // The units inserted go before the table.
    if (state.packed_index > state.table_index) {
        state.packed_index += state.inserted;
    }
    state.table_index += state.inserted;
    state.units += state.inserted + state.appended;
    state.committed_rows = state.column.size();
    state.inserted = 0;
    state.appended = 0;
}

void edit_sequence(input_bytes& in)
{
    model state;
    std::vector<std::byte> bytes = initial_file(in, state);
    const std::uint8_t strategy = in.next();
    const openfits::editor_options options{.relayout = (strategy & 1U) != 0 ? openfits::relayout::in_place
                                                                            : openfits::relayout::safe_copy,
                                           .atomic = (strategy & 2U) != 0};
    auto device = std::make_unique<openfits::memory_device>(std::move(bytes));
    const openfits::memory_device& memory = *device;
    openfits::editor file(std::move(device), options);
    // The values of the units added, which their writers borrow until the commit.
    std::vector<std::vector<std::uint8_t>> borrowed;
    borrowed.reserve(most_steps);
    for (int step = 0; step < most_steps && !in.empty(); ++step) {
        const unsigned operation = in.next() % 9U;
        switch (operation) {
        case 0: {
            // A keyword of the image, the table or the compressed image, among more than a block of cards.
            const unsigned unit = in.next() % 3U;
            const std::string name = "FUZZ" + std::to_string(in.next() % 48U);
            const std::int64_t value = in.next();
            const std::size_t index = unit == 0 ? 0 : (unit == 1 ? state.table_index : state.packed_index);
            file.header(index).set(name, value);
            break;
        }
        case 1: {
            const unsigned unit = in.next() % 3U;
            const std::string name = "FUZZ" + std::to_string(in.next() % 48U);
            const std::size_t index = unit == 0 ? 0 : (unit == 1 ? state.table_index : state.packed_index);
            (void)file.header(index).erase(name);
            break;
        }
        case 2:
        case 3: {
            const region_write write = region_of(in);
            const std::size_t index = operation == 2 ? 0 : state.packed_index;
            file.write_pixels(index, std::span<const std::uint16_t>(write.values), {.region = write.area});
            lay(operation == 2 ? state.image : state.packed, write);
            break;
        }
        case 4: {
            const std::size_t row = in.next() % state.committed_rows;
            const std::array<std::int32_t, 1> value{small_number(in)};
            file.write_cells(state.table_index, std::size_t{0}, std::span<const std::int32_t>(value),
                             {.first_row = row});
            state.column[row] = value[0];
            break;
        }
        case 5: {
            std::vector<std::int32_t> rows(1U + (in.next() % 3U));
            for (std::int32_t& value : rows) {
                value = small_number(in);
            }
            file.append_rows(state.table_index,
                             std::vector<openfits::column_values>{std::span<const std::int32_t>(rows)});
            state.column.insert(state.column.end(), rows.begin(), rows.end());
            break;
        }
        case 6:
        case 7: {
            // An image appended after the last unit, or a unit inserted before the table: a header alone, or an
            // image.
            const std::uint8_t kind = in.next();
            const std::size_t count = 1U + (kind % 5U);
            const std::uint8_t fill = in.next();
            const std::vector<std::uint8_t>& values = borrowed.emplace_back(count, fill);
            openfits::writer units;
            if (operation == 7 && (kind & 8U) != 0) {
                (void)units.add_header({});
            } else {
                openfits::image_info added;
                added.geometry.axes = {values.size()};
                (void)units.add_image(added, std::span<const std::uint8_t>(values));
            }
            if (operation == 6) {
                (void)file.append(std::move(units));
                ++state.appended;
            } else {
                file.insert_hdu(state.table_index, std::move(units));
                ++state.inserted;
            }
            break;
        }
        default:
            commit(file, state);
            break;
        }
    }
    commit(file, state);

    // The file opens strictly, every checksum matching its unit, conforms, and holds the values of the model.
    const std::span<const std::byte> result = memory.data();
    const openfits::reader edited(
        std::make_unique<openfits::memory_source>(std::vector<std::byte>(result.begin(), result.end())),
        openfits::read_options{.strict = true});
    require(edited.hdu_count() == state.units);
    require(openfits::verify(edited).empty());
    require(edited.read_pixels<std::uint16_t>(0) == state.image);
    require(edited.read_pixels<std::uint16_t>(state.packed_index) == state.packed);
    require(edited.read_column<std::int32_t>(state.table_index, 0) == state.column);
}

void hostile_file(input_bytes& in)
{
    const std::uint8_t changes = in.next();
    const std::span<const std::byte> rest = in.rest();
    // The values of the unit appended, which its writer borrows until the commit.
    const std::vector<std::uint8_t> values(2, changes);
    std::vector<std::byte> result;
    try {
        auto device = std::make_unique<openfits::memory_device>(std::vector<std::byte>(rest.begin(), rest.end()));
        const openfits::memory_device& memory = *device;
        openfits::editor file(std::move(device), {.relayout = (changes & 1U) != 0 ? openfits::relayout::in_place
                                                                                  : openfits::relayout::safe_copy});
        const std::size_t count = file.file().hdu_count();
        const std::size_t target = changes % count;
        if ((changes & 2U) != 0) {
            file.header(target).set("EDITED", true);
        }
        const std::uint64_t size = file.file().hdu(target).data_size().value_or(0);
        if ((changes & 4U) != 0 && size > 0) {
            const std::array<std::byte, 3> bytes{std::byte{1}, std::byte{2}, std::byte{3}};
            const std::size_t length = std::min<std::uint64_t>(bytes.size(), size);
            file.write_raw(target, 0, std::span<const std::byte>(bytes).first(length));
        }
        if ((changes & 8U) != 0) {
            openfits::image_info added;
            added.geometry.axes = {values.size()};
            openfits::writer units;
            (void)units.add_image(added, std::span<const std::uint8_t>(values));
            (void)file.append(std::move(units));
        }
        if ((changes & 16U) != 0 && count > 1) {
            file.erase_hdu(count - 1);
        }
        file.commit();
        const std::span<const std::byte> written = memory.data();
        result.assign(written.begin(), written.end());
    } catch (const openfits::error&) {
        // A file that the editor cannot change, or a change that it refuses, is reported as an error of its kinds.
        return;
    }
    // The file that the commit wrote opens again, and its walk and its report complete.
    try {
        const openfits::reader edited(std::make_unique<openfits::memory_source>(std::move(result)));
        (void)edited.hdu_count();
        (void)openfits::verify(edited);
    } catch (const openfits::error&) {
        std::abort();
    }
}

} // namespace

// The name is fixed by libFuzzer.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    input_bytes in({reinterpret_cast<const std::byte*>(data), size});
    if (in.next() % 2 == 0) {
        edit_sequence(in);
    } else {
        hostile_file(in);
    }
    return 0;
}
