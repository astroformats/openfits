// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/card.h>
#include <openfits/checksum.h>
#include <openfits/error.h>
#include <openfits/header.h>

#include "checksum/checksum.h"
#include "compress/image_tiles.h"
#include "edit/header_edit.h"
#include "header/record.h"
#include "support/fixture_builder.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// The headers that the editor writes: checked against the header they were read as, written into blocks with the
// records of their cards kept, and completed with checksums that keep a mismatch they had (standard Appendix J.4).

namespace {

using openfits::card;
using openfits::errc;
using openfits::header;
using openfits::validation_error;
using openfits::test::card_record;
using openfits::test::throws;

constexpr std::size_t record = 80;
constexpr std::size_t block = 2880;

// A header read from records, as the walk of a file reads it.
header parsed(const std::vector<std::string>& records)
{
    std::vector<card> cards;
    cards.reserve(records.size());
    for (const std::string& text : records) {
        cards.push_back(card::parse(text));
    }
    return header(std::move(cards));
}

header image_header()
{
    return parsed({card_record("SIMPLE", "T"), card_record("BITPIX", "16"), card_record("NAXIS", "2"),
                   card_record("NAXIS1", "10"), card_record("NAXIS2", "20"), card_record("EXTEND", "T"),
                   card_record("BZERO", "32768"), card_record("OBJECT", "'M 31    '", "the source"),
                   card_record("CHECKSUM", "'0000000000000000'"), card_record("DATASUM", "'0'")});
}

TEST(check_structure, takes_a_header_whose_structure_stays)
{
    const header original = image_header();
    header edited = original;
    edited.set("OBJECT", "M 33");
    edited.append(card("EXPTIME", 30.0));
    (void)edited.erase("CHECKSUM");
    edited.set("DATASUM", "12");
    // The value of a structural keyword given again, with another comment.
    edited.set("NAXIS1", 10, "the first axis");
    EXPECT_NO_THROW(openfits::detail::check_structure(original, edited, 0));
}

TEST(check_structure, refuses_a_header_whose_structure_changes)
{
    const header original = image_header();
    const auto refused = [&original](const std::function<void(header&)>& change) {
        header edited = original;
        change(edited);
        return throws<validation_error>(errc::structural_keyword,
                                        [&] { openfits::detail::check_structure(original, edited, 3); });
    };
    EXPECT_TRUE(refused([](header& cards) { cards.set("NAXIS2", 21); }));
    EXPECT_TRUE(refused([](header& cards) { (void)cards.erase("BZERO"); }));
    EXPECT_TRUE(refused([](header& cards) { cards.append(card("BSCALE", 2.0)); }));
    EXPECT_TRUE(refused([](header& cards) { cards.append(card("TTYPE1", "X")); }));
    EXPECT_TRUE(refused([](header& cards) { (void)cards.rename("OBJECT", "BLANK"); }));
    // Another card among the mandatory keywords that start the header.
    EXPECT_TRUE(refused([](header& cards) { cards.insert(1, card("FIRST", 1)); }));
    EXPECT_TRUE(refused([](header& cards) { cards.insert(0, card("FIRST", 1)); }));
}

TEST(edited_blocks, writes_each_card_read_from_a_file_as_its_record_and_formats_the_others)
{
    header cards = image_header();
    // A record with a byte outside printable ASCII, read from a file, is written as it is.
    std::string odd = card_record("ODD", "'caf'", "a comment");
    odd[25] = static_cast<char>(0xE9);
    cards.append(card::parse(odd));
    cards.set("OBJECT", "NGC 224");
    const std::string blocks = openfits::detail::edited_blocks(cards, {}, false, 1);
    ASSERT_EQ(blocks.size(), 2880U);
    EXPECT_EQ(blocks.substr(0, 80), card_record("SIMPLE", "T"));
    EXPECT_EQ(blocks.substr(10 * record, 80), odd);
    EXPECT_EQ(blocks.substr(7 * record, 20), "OBJECT  = 'NGC 224 '");
    EXPECT_EQ(blocks.substr(11 * record, 3), "END");
}

TEST(edited_blocks, keeps_end_in_the_last_of_the_blocks_it_had_after_blank_cards)
{
    const header cards = image_header();
    const std::string blocks = openfits::detail::edited_blocks(cards, {}, false, 3);
    ASSERT_EQ(blocks.size(), 3 * block);
    // END is the first record of the third block, after the blank cards that fill the two before it.
    EXPECT_EQ(blocks.substr(2 * block, 3), "END");
    EXPECT_EQ(blocks.find_first_not_of(' ', 10 * record), 2 * block);
    // A header of more records than its blocks held takes more.
    header longer = cards;
    for (int i = 0; i < 40; ++i) {
        longer.append(card("KEY" + std::to_string(i), i));
    }
    EXPECT_EQ(openfits::detail::edited_blocks(longer, {}, false, 1).size(), 2 * block);
}

TEST(edited_blocks, announces_a_long_string_with_longstrn_when_asked)
{
    header cards = image_header();
    cards.set("NOTES", std::string(100, 'x'));
    const std::string blocks = openfits::detail::edited_blocks(cards, {}, true, 1);
    EXPECT_EQ(blocks.substr(10 * record, 8), "LONGSTRN");
    EXPECT_EQ(blocks.substr(11 * record, 8), "NOTES   ");
    EXPECT_EQ(blocks.substr(12 * record, 8), "CONTINUE");
    // The long string of a reserved keyword is refused, as the writer refuses it (standard §4.2.1.2).
    header reserved = image_header();
    reserved.set("OBJECT", std::string(100, 'x'));
    EXPECT_THROW((void)openfits::detail::edited_blocks(reserved, {}, false, 1), validation_error);
}

TEST(seal_checksum, makes_the_unit_sum_to_the_total_asked_for)
{
    const header cards = openfits::detail::with_checksum_cards(image_header(), 123456, true);
    EXPECT_EQ(cards.value<std::string>("DATASUM"), "123456");
    for (const std::uint32_t total : {0xFFFFFFFFU, 0x12345678U, 1U}) {
        std::string blocks = openfits::detail::edited_blocks(cards, {}, false, 1);
        openfits::detail::seal_checksum(blocks, 123456, total);
        const std::uint32_t sum =
            openfits::detail::add_sums(openfits::detail::sum_of(std::as_bytes(std::span(blocks))), 123456);
        EXPECT_EQ(sum, total);
    }
    // A header without the placeholder is refused.
    std::string plain = openfits::detail::edited_blocks(image_header(), {}, false, 1);
    plain.replace(8 * record, record, std::string(80, ' '));
    EXPECT_THROW(openfits::detail::seal_checksum(plain, 0, 0xFFFFFFFFU), openfits::usage_error);
}

TEST(compressed_header, writes_the_cards_of_the_image_back_into_the_table)
{
    const header table = parsed({card_record("XTENSION", "'BINTABLE'"),
                                 card_record("BITPIX", "8"),
                                 card_record("NAXIS", "2"),
                                 card_record("NAXIS1", "8"),
                                 card_record("NAXIS2", "20"),
                                 card_record("PCOUNT", "100"),
                                 card_record("GCOUNT", "1"),
                                 card_record("TFIELDS", "1"),
                                 card_record("TTYPE1", "'COMPRESSED_DATA'"),
                                 card_record("TFORM1", "'1PB(10) '"),
                                 card_record("ZIMAGE", "T"),
                                 card_record("ZBITPIX", "16"),
                                 card_record("ZNAXIS", "2"),
                                 card_record("ZNAXIS1", "10"),
                                 card_record("ZNAXIS2", "20"),
                                 card_record("ZCMPTYPE", "'GZIP_1  '"),
                                 card_record("EXTNAME", "'SCI     '"),
                                 card_record("OBJECT", "'M 31    '"),
                                 card_record("ZHECKSUM", "'aaaaaaaaaaaaaaaa'"),
                                 card_record("ZDATASUM", "'5'"),
                                 card_record("CHECKSUM", "'bbbbbbbbbbbbbbbb'"),
                                 card_record("DATASUM", "'7'"),
                                 openfits::test::record(""),
                                 openfits::test::record("")});
    const header restored = openfits::detail::restored_image_header(table);
    ASSERT_EQ(restored.value<std::string>("DATASUM"), "5");
    header edited = restored;
    edited.set("OBJECT", "M 33");
    edited.append(card("EXPTIME", 30.0));
    edited.set("DATASUM", "6");
    const header written = openfits::detail::compressed_header(table, restored, edited);
    EXPECT_EQ(written.value<std::string>("OBJECT"), "M 33");
    EXPECT_EQ(written.value<double>("EXPTIME"), 30.0);
    EXPECT_EQ(written.value<std::string>("ZDATASUM"), "6");
    EXPECT_EQ(written.value<std::string>("ZHECKSUM"), "aaaaaaaaaaaaaaaa");
    // The table keeps its own checksums, its compression and its fill space, less the blank card that EXPTIME took.
    EXPECT_EQ(written.value<std::string>("DATASUM"), "7");
    EXPECT_EQ(written.value<std::string>("ZCMPTYPE"), "GZIP_1");
    EXPECT_EQ(written.free_cards(), 1U);
    // Restored again, the image has the cards of edited.
    const header again = openfits::detail::restored_image_header(written);
    EXPECT_EQ(again.value<std::string>("OBJECT"), "M 33");
    EXPECT_EQ(again.value<std::string>("DATASUM"), "6");
}

} // namespace
