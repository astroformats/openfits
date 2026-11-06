// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Commits that run out of memory: the pending changes of an editor stay when a commit throws std::bad_alloc, and the
// next commit applies them, in place and through a copy, which leaves the file as it was.

#include <openfits/editor.h>
#include <openfits/header.h>
#include <openfits/image.h>
#include <openfits/io.h>
#include <openfits/table.h>
#include <openfits/types.h>
#include <openfits/writer.h>

#include "allocation/allocations.h"
#include "support/files.h"
#include "support/temp_directory.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

// A file of an image and a table, with checksums, which the edits change.
std::vector<std::byte> original_file()
{
    std::vector<std::uint16_t> pixels(std::size_t{37} * 23);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<std::uint16_t>(i * 7U);
    }
    const std::vector<double> fluxes{3.44, 5.72, 4.0};
    openfits::writer output;
    openfits::image_info image;
    image.geometry.axes = {37, 23};
    image.header.set("OBJECT", "pattern");
    (void)output.add_image(image, std::span<const std::uint16_t>(pixels));
    openfits::table_info table{.kind = openfits::hdu_kind::bintable, .rows = 3};
    table.columns = {{.name = "FLUX"}};
    (void)output.add_table(table, std::vector<openfits::column_values>{std::span<const double>(fluxes)});
    openfits::memory_sink sink;
    output.save(sink, {.checksums = true});
    return sink.release();
}

// The bytes as the text that read_file() gives.
std::string as_text(const std::vector<std::byte>& bytes)
{
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// The values that the edits write, which the editor borrows until its commit.
struct edit_values
{
    std::vector<float> region = std::vector<float>(16, 1234.0F);
    std::vector<double> flux{9.5};
    std::vector<std::uint8_t> appended = std::vector<std::uint8_t>(64, 7);
};

// Changes in place, which keep the layout of the file: a keyword in the room of the header, a region of the image, a
// field of the table, and a unit appended. With grow, a header that grows by a block, which moves the units.
void edit(openfits::editor& file, const edit_values& values, bool grow)
{
    openfits::header& primary = file.header(0);
    primary.set("OBSERVER", "an observer");
    if (grow) {
        for (int i = 0; i < 40; ++i) {
            primary.set("KEY" + std::to_string(i), i);
        }
    }
    file.write_pixels(0, std::span<const float>(values.region),
                      {.region = openfits::region{.start = {2, 3}, .size = {4, 4}}});
    file.write_cells(1, "FLUX", std::span<const double>(values.flux), {.first_row = 1});
    openfits::writer appended;
    openfits::image_info image;
    image.geometry.axes = {8, 8};
    (void)appended.add_image(image, std::span<const std::uint8_t>(values.appended));
    (void)file.append(std::move(appended));
}

// The file that the edits make, committed without a failure.
std::vector<std::byte> edited_file(bool grow)
{
    const edit_values values;
    auto device = std::make_unique<openfits::memory_device>(original_file());
    const openfits::memory_device* bytes = device.get();
    openfits::editor file(std::move(device));
    edit(file, values, grow);
    file.commit();
    const std::span<const std::byte> data = bytes->data();
    return {data.begin(), data.end()};
}

TEST(editor, a_commit_in_place_out_of_memory_keeps_its_changes_pending)
{
    if (!openfits::test::can_fail_allocations()) {
        GTEST_SKIP() << "the allocations of the library cannot fail in this build";
    }
    const std::vector<std::byte> expected = edited_file(false);
    const edit_values values;

    for (std::uint64_t index = 0;; ++index) {
        auto device = std::make_unique<openfits::memory_device>(original_file());
        const openfits::memory_device* bytes = device.get();
        openfits::editor file(std::move(device));
        edit(file, values, false);
        openfits::test::fail_allocation(index);
        bool ran_out = false;
        try {
            file.commit();
        } catch (const std::bad_alloc&) {
            ran_out = true;
        }
        if (!openfits::test::stop_failing_allocations()) {
            const std::span<const std::byte> data = bytes->data();
            EXPECT_EQ(std::vector<std::byte>(data.begin(), data.end()), expected);
            break;
        }
        ASSERT_TRUE(ran_out) << "allocation " << index;
        ASSERT_TRUE(file.has_pending_changes()) << "allocation " << index;
        file.commit();
        const std::span<const std::byte> data = bytes->data();
        ASSERT_EQ(std::vector<std::byte>(data.begin(), data.end()), expected) << "allocation " << index;
    }
}

// A failure before the rename leaves the old file, and one after it, as the editor reads the new file, the new one:
// never a mix, nor a temporary file left behind.
TEST(editor, a_commit_through_a_copy_out_of_memory_leaves_the_old_file_or_the_new_one)
{
    if (!openfits::test::can_fail_allocations()) {
        GTEST_SKIP() << "the allocations of the library cannot fail in this build";
    }
    const std::vector<std::byte> original = original_file();
    const std::vector<std::byte> expected = edited_file(true);
    const edit_values values;
    const openfits::test::temp_directory directory;
    const std::string path = directory.file("edited.fits");

    for (std::uint64_t index = 0;; ++index) {
        openfits::test::write_file(openfits::test::path_of(path), original);
        std::optional<openfits::editor> file;
        file.emplace(path);
        edit(*file, values, true);
        openfits::test::fail_allocation(index);
        bool ran_out = false;
        try {
            file->commit();
        } catch (const std::bad_alloc&) {
            ran_out = true;
        }
        if (!openfits::test::stop_failing_allocations()) {
            file.reset();
            EXPECT_EQ(openfits::test::read_file(openfits::test::path_of(path)), as_text(expected));
            break;
        }
        const std::string left = openfits::test::read_file(openfits::test::path_of(path));
        ASSERT_TRUE(left == as_text(original) || left == as_text(expected)) << "allocation " << index;
        ASSERT_EQ(directory.entries(), 1U) << "allocation " << index;
        ASSERT_TRUE(ran_out) << "allocation " << index;
        ASSERT_TRUE(file->has_pending_changes()) << "allocation " << index;
        file->commit();
        file.reset();
        ASSERT_EQ(openfits::test::read_file(openfits::test::path_of(path)), as_text(expected))
            << "allocation " << index;
    }
}

} // namespace
