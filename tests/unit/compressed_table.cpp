// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/compression.h>
#include <openfits/error.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/table.h>
#include <openfits/writer.h>

#include "support/throws.h"
#include "support/written_file.h"

#include <gtest/gtest.h>

#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace {

using openfits::column_format;
using openfits::column_info;
using openfits::column_storage;
using openfits::column_type;
using openfits::column_values;
using openfits::compression;
using openfits::compression_options;
using openfits::errc;
using openfits::hdu_kind;
using openfits::reader;
using openfits::table_info;
using openfits::validation_error;
using openfits::writer;
using openfits::test::throws;
using openfits::test::written;

reader open(const std::vector<std::byte>& file)
{
    return reader(std::make_unique<openfits::memory_source>(std::vector<std::byte>(file)), {.strict = true});
}

// The values of a table of every type of column, fixed and variable-length, over rows rows.
class sample_table
{
public:
    explicit sample_table(std::size_t rows) : rows_(rows)
    {
        for (std::size_t i = 0; i < rows; ++i) {
            bytes_.push_back(static_cast<std::uint8_t>((i * 7) % 256));
            shorts_.push_back(static_cast<std::int16_t>(static_cast<int>((i * 37) % 2000) - 1000));
            ushorts_.push_back(static_cast<std::uint16_t>((i * 600) % 65536));
            ints_.push_back(static_cast<std::int32_t>(i * 100003) - 5'000'000);
            longs_.push_back(static_cast<std::int64_t>(i) * 1'000'000'000'000);
            floats_.push_back(static_cast<float>(i) / 7.0F);
            doubles_.push_back(static_cast<double>(i) / 3.0);
            complexes_.emplace_back(static_cast<float>(i), static_cast<float>(i) / 2.0F);
            double_complexes_.emplace_back(static_cast<double>(i) / 4.0, -static_cast<double>(i));
            flags_.push_back(static_cast<std::uint8_t>(i % 3 == 0 ? 1 : 0));
            names_.push_back("row " + std::to_string(i));
            for (std::size_t k = 0; k < 13; ++k) {
                bits_.push_back(static_cast<std::uint8_t>((i + k) % 3 == 0 ? 1 : 0));
            }
            for (std::size_t k = 0; k < 6; ++k) {
                matrix_.push_back(static_cast<std::int16_t>((i * 6) + k));
            }
            array_offsets_.push_back(arrays_.size());
            for (std::size_t k = 0; k < i % 5; ++k) {
                arrays_.push_back(static_cast<std::int32_t>(k * i));
            }
            real_offsets_.push_back(reals_.size());
            for (std::size_t k = 0; k < i % 3; ++k) {
                reals_.push_back(static_cast<double>(k) / static_cast<double>(i + 1));
            }
            notes_.push_back(i % 4 == 0 ? std::string() : std::string(i % 9, 'x'));
        }
        array_offsets_.push_back(arrays_.size());
        real_offsets_.push_back(reals_.size());
    }

    // The table, compressed with options when it has them.
    std::size_t add_to(writer& output, std::optional<compression_options> options = std::nullopt)
    {
        table_info info;
        info.kind = hdu_kind::bintable;
        info.rows = rows_;
        const auto named = [](std::string name, std::vector<std::uint64_t> dimensions = {}) {
            column_info column;
            column.name = std::move(name);
            column.dimensions = std::move(dimensions);
            return column;
        };
        info.columns = {named("UBYTE"),          named("SHORT"),   named("USHORT"),     named("INT"),
                        named("LONG"),           named("FLOAT"),   named("DOUBLE"),     named("CPLX"),
                        named("DCPLX"),          named("FLAG"),    named("BITS", {13}), named("NAME"),
                        named("MATRIX", {3, 2}), named("VLA_INT"), named("VLA_REAL"),   named("NOTES")};
        const std::vector<column_values> values{std::span(bytes_),
                                                std::span(shorts_),
                                                std::span(ushorts_),
                                                std::span(ints_),
                                                std::span(longs_),
                                                std::span(floats_),
                                                std::span(doubles_),
                                                std::span(complexes_),
                                                std::span(double_complexes_),
                                                column_values::logicals(flags_),
                                                column_values::bits(bits_),
                                                std::span(names_),
                                                std::span(matrix_),
                                                column_values::arrays(std::span(arrays_), array_offsets_),
                                                column_values::arrays(std::span(reals_), real_offsets_),
                                                column_values::arrays(std::span(notes_))};
        return output.add_table(info, values, {.compression = std::move(options)});
    }

private:
    std::size_t rows_;
    std::vector<std::uint8_t> bytes_{};
    std::vector<std::int16_t> shorts_{};
    std::vector<std::uint16_t> ushorts_{};
    std::vector<std::int32_t> ints_{};
    std::vector<std::int64_t> longs_{};
    std::vector<float> floats_{};
    std::vector<double> doubles_{};
    std::vector<std::complex<float>> complexes_{};
    std::vector<std::complex<double>> double_complexes_{};
    std::vector<std::uint8_t> flags_{};
    std::vector<std::string> names_{};
    std::vector<std::uint8_t> bits_{};
    std::vector<std::int16_t> matrix_{};
    std::vector<std::int32_t> arrays_{};
    std::vector<std::uint64_t> array_offsets_{};
    std::vector<double> reals_{};
    std::vector<std::uint64_t> real_offsets_{};
    std::vector<std::string> notes_{};
};

// Every column of the table of unit 1 of compressed equals that of plain, the same table uncompressed.
void expect_same_tables(const reader& compressed, const reader& plain)
{
    EXPECT_EQ(compressed.hdu(1).kind(), hdu_kind::compressed_table);
    const table_info ours = compressed.table(1);
    const table_info theirs = plain.table(1);
    ASSERT_EQ(ours.columns.size(), theirs.columns.size());
    EXPECT_EQ(ours.rows, theirs.rows);
    EXPECT_EQ(ours.kind, hdu_kind::bintable);
    for (std::size_t c = 0; c < ours.columns.size(); ++c) {
        SCOPED_TRACE(theirs.columns[c].name);
        EXPECT_EQ(ours.columns[c].name, theirs.columns[c].name);
        EXPECT_EQ(ours.columns[c].format, theirs.columns[c].format);
        EXPECT_EQ(ours.columns[c].dimensions, theirs.columns[c].dimensions);
        const column_format format = theirs.columns[c].format.value_or(column_format{});
        if (format.storage != column_storage::fixed && format.type != column_type::character) {
            const openfits::column_arrays<double> mine = compressed.read_arrays<double>(1, c);
            const openfits::column_arrays<double> other = plain.read_arrays<double>(1, c);
            EXPECT_EQ(mine.values, other.values);
            EXPECT_EQ(mine.offsets, other.offsets);
        } else {
            EXPECT_EQ(compressed.read_strings(1, c), plain.read_strings(1, c));
        }
    }
}

struct table_case
{
    std::string name{};
    std::size_t rows = 0;
    compression_options options{};
};

// NOLINTNEXTLINE(readability-identifier-naming)
void PrintTo(const table_case& value, std::ostream* output)
{
    *output << value.name;
}

class compressed_table_round_trip : public testing::TestWithParam<table_case>
{};

TEST_P(compressed_table_round_trip, reads_back_the_table_it_wrote)
{
    const table_case& parameter = GetParam();
    sample_table values(parameter.rows);
    writer compressed_writer;
    EXPECT_EQ(values.add_to(compressed_writer, parameter.options), 1U);
    writer plain_writer;
    (void)values.add_to(plain_writer);
    expect_same_tables(open(written(compressed_writer)), open(written(plain_writer)));
}

std::vector<table_case> table_cases()
{
    std::vector<table_case> cases;
    const auto add = [&cases](std::string name, std::size_t rows, compression_options options) {
        table_case entry;
        entry.name = std::move(name);
        entry.rows = rows;
        entry.options = std::move(options);
        cases.push_back(std::move(entry));
    };
    add("default", 1000, {});
    add("one_row", 1, {});
    add("rows_of_seven", 1000, {.tile_rows = 7});
    add("tile_of_one_row", 50, {.tile_rows = 1});
    add("tile_beyond_the_table", 50, {.tile_rows = 5000});
    add("rice", 300, {.type = compression::rice});
    add("gzip_1", 300, {.type = compression::gzip_1});
    add("gzip_2", 300, {.type = compression::gzip_2});
    add("nocompress", 300, {.type = compression::nocompress});
    add("level_9", 300, {.gzip_level = 9});
    std::vector<compression> columns(16, compression::automatic);
    columns[1] = compression::rice;
    columns[3] = compression::gzip_2;
    columns[13] = compression::gzip_1;
    columns[14] = compression::nocompress;
    add("per_column", 300, {.columns = columns});
    return cases;
}

INSTANTIATE_TEST_SUITE_P(tables, compressed_table_round_trip, testing::ValuesIn(table_cases()),
                         [](const testing::TestParamInfo<table_case>& instance) { return instance.param.name; });

TEST(compressed_table, writes_the_keywords_of_standard_10_3)
{
    sample_table values(20);
    writer output;
    (void)values.add_to(output, compression_options{.tile_rows = 8});
    const reader input = open(written(output));
    const openfits::header& cards = input.hdu(1).header();
    EXPECT_TRUE(cards.value<bool>("ZTABLE"));
    EXPECT_EQ(cards.value<std::int64_t>("ZTILELEN"), 8);
    EXPECT_EQ(cards.value<std::int64_t>("ZNAXIS2"), 20);
    EXPECT_EQ(cards.value<std::int64_t>("NAXIS2"), 3);
    EXPECT_EQ(cards.value<std::string>("ZFORM1"), "1B");
    EXPECT_EQ(cards.value<std::string>("ZFORM14"), "1PJ(4)");
    EXPECT_EQ(cards.value<std::string>("TFORM1").substr(0, 3), "1QB");
    // The defaults of the types: RICE_1 for 32-bit integers, GZIP_2 for the numbers it shuffles, GZIP_1 otherwise.
    EXPECT_EQ(cards.value<std::string>("ZCTYP1"), "GZIP_1");
    EXPECT_EQ(cards.value<std::string>("ZCTYP2"), "GZIP_2");
    EXPECT_EQ(cards.value<std::string>("ZCTYP4"), "RICE_1");
    EXPECT_EQ(cards.value<std::string>("ZCTYP7"), "GZIP_2");
    EXPECT_EQ(cards.value<std::string>("ZCTYP8"), "GZIP_1");
    // The table that it holds.
    const table_info table = input.table(1);
    EXPECT_EQ(table.header.value<std::int64_t>("NAXIS2"), 20);
    EXPECT_FALSE(table.header.index_of("ZTILELEN").has_value());
    EXPECT_EQ(table.header.value<std::string>("TFORM14"), "1PJ(4)");
}

TEST(compressed_table, honours_the_directives_of_its_header)
{
    // FZALGOR and FZTILELN (standard §10.3.3), and FZALGn before FZALGOR.
    std::vector<std::int32_t> numbers(30);
    std::vector<double> reals(30);
    for (std::size_t i = 0; i < numbers.size(); ++i) {
        numbers[i] = static_cast<std::int32_t>(i * 3);
        reals[i] = static_cast<double>(i) / 5.0;
    }
    table_info model;
    model.kind = hdu_kind::bintable;
    model.rows = 30;
    model.columns = {column_info{.name = "N"}, column_info{.name = "R"}};
    model.header.set("FZALGOR", "GZIP_1");
    model.header.set("FZALG2", "GZIP_2");
    model.header.set("FZTILELN", 4);
    writer with_directives;
    (void)with_directives.add_table(model, std::vector<column_values>{std::span(numbers), std::span(reals)},
                                    {.compression = compression_options{}});
    const reader input = open(written(with_directives));
    const openfits::header& cards = input.hdu(1).header();
    EXPECT_EQ(cards.value<std::string>("ZCTYP1"), "GZIP_1");
    EXPECT_EQ(cards.value<std::string>("ZCTYP2"), "GZIP_2");
    EXPECT_EQ(cards.value<std::int64_t>("ZTILELEN"), 4);
    EXPECT_EQ(input.read_column<std::int32_t>(1, "N"), numbers);
    EXPECT_EQ(input.read_column<double>(1, "R"), reals);
}

TEST(compressed_table, refuses_an_algorithm_that_a_column_cannot_take)
{
    sample_table values(10);
    std::vector<compression> rice_on_reals(16, compression::automatic);
    rice_on_reals[6] = compression::rice;
    writer output;
    (void)values.add_to(output, compression_options{.columns = rice_on_reals});
    EXPECT_TRUE(throws<validation_error>(errc::invalid_compression, [&] { (void)written(output); }));
    std::vector<compression> plio(16, compression::automatic);
    plio[0] = compression::plio;
    writer refused;
    (void)values.add_to(refused, compression_options{.columns = plio});
    EXPECT_TRUE(throws<validation_error>(errc::invalid_compression, [&] { (void)written(refused); }));
}

TEST(compressed_table, an_ascii_table_is_never_compressed)
{
    const std::vector<std::int32_t> numbers{1, 2, 3};
    table_info info;
    info.kind = hdu_kind::table;
    info.rows = 3;
    info.columns = {column_info{.name = "N"}};
    writer output;
    (void)output.add_table(info, std::vector<column_values>{std::span(numbers)},
                           {.compression = compression_options{}});
    EXPECT_EQ(open(written(output)).hdu(1).kind(), hdu_kind::table);
}

} // namespace
