// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Saves that run out of memory: a writer is unchanged by a save that throws std::bad_alloc, and a file sink leaves no
// file.

#include <openfits/compression.h>
#include <openfits/image.h>
#include <openfits/io.h>
#include <openfits/table.h>
#include <openfits/writer.h>

#include "allocation/allocations.h"
#include "support/temp_directory.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <new>
#include <span>
#include <string>
#include <vector>

namespace {

// Values for an image and a table, which the writer borrows.
struct model_values
{
    std::vector<std::uint16_t> pixels{};
    std::vector<double> fluxes{};
    std::vector<std::string> names{};
};

model_values values_of_the_model()
{
    model_values values;
    for (std::uint16_t i = 0; i < 37 * 23; ++i) {
        values.pixels.push_back(static_cast<std::uint16_t>((i * 37U) % 65'000U));
    }
    values.fluxes = {3.44, 5.72, 4.0};
    values.names = {"M 31", "M 33", "NGC 7000"};
    return values;
}

// A writer of an image, the same image compressed in tiles, and a table.
openfits::writer writer_of(const model_values& values)
{
    openfits::writer output;
    openfits::image_info image;
    image.geometry.axes = {37, 23};
    image.header.set("OBJECT", "pattern", "the target");
    (void)output.add_image(image, std::span<const std::uint16_t>(values.pixels));
    (void)output.add_image(
        image, std::span<const std::uint16_t>(values.pixels),
        {.compression = openfits::compression_options{.type = openfits::compression::rice, .tile = {37, 4}}});
    openfits::table_info table{.kind = openfits::hdu_kind::bintable, .rows = 3};
    table.columns = {{.name = "NAME"}, {.name = "FLUX", .unit = "Jy"}};
    (void)output.add_table(table, std::vector<openfits::column_values>{std::span<const std::string>(values.names),
                                                                       std::span<const double>(values.fluxes)});
    return output;
}

// Checksums, so that the save completes its headers once their data are written.
openfits::write_options save_options()
{
    return {.checksums = true};
}

std::vector<std::byte> saved(const openfits::writer& output)
{
    openfits::memory_sink sink;
    output.save(sink, save_options());
    return sink.release();
}

TEST(writer, a_save_out_of_memory_leaves_the_writer_as_it_was)
{
    if (!openfits::test::can_fail_allocations()) {
        GTEST_SKIP() << "the allocations of the library cannot fail in this build";
    }
    const model_values values = values_of_the_model();
    const openfits::writer output = writer_of(values);
    const std::vector<std::byte> expected = saved(output);

    const std::uint64_t failed = openfits::test::fail_each_allocation(
        [&output] { (void)saved(output); }, [&output, &expected] { ASSERT_EQ(saved(output), expected); });
    EXPECT_GT(failed, 0U);
}

TEST(writer, a_save_to_a_path_out_of_memory_leaves_no_file)
{
    if (!openfits::test::can_fail_allocations()) {
        GTEST_SKIP() << "the allocations of the library cannot fail in this build";
    }
    const model_values values = values_of_the_model();
    const openfits::writer output = writer_of(values);
    const openfits::test::temp_directory directory;
    const std::string path = directory.file("out.fits");

    (void)openfits::test::fail_each_allocation([&output, &path] { output.save(path, save_options()); },
                                               [&directory, &path] {
                                                   ASSERT_FALSE(std::filesystem::exists(openfits::test::path_of(path)));
                                                   ASSERT_EQ(directory.entries(), 0U);
                                               });
    EXPECT_TRUE(std::filesystem::exists(openfits::test::path_of(path)));
}

} // namespace
