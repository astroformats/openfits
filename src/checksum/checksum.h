// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openfits/card.h>
#include <openfits/checksum.h>
#include <openfits/hdu.h>
#include <openfits/header.h>
#include <openfits/io.h>

#include "core/diagnostic_log.h"
#include "header/record.h"
#include "io/thread_safe_source.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The sums of standard §4.4.2.7: the 32-bit ones' complement sum of the bytes of a unit taken as big-endian integers,
// which DATASUM records for the data records and CHECKSUM makes negative zero for the whole unit (Appendix J).

namespace openfits::detail {

/// The ones' complement sum of two sums: their sum, with the carry out of the top bit added back into the bottom one.
/// It is associative and commutative, so that the sums of the pieces of a unit combine in any order.
[[nodiscard]] constexpr std::uint32_t add_sums(std::uint32_t a, std::uint32_t b) noexcept
{
    const std::uint64_t total = std::uint64_t{a} + b;
    // At most 2^33 - 2: a carry of 1 into a low part of at most 2^32 - 2, which does not carry again.
    return static_cast<std::uint32_t>((total & 0xFFFFFFFFU) + (total >> 32U));
}

/// The bytes of the data records of data of size bytes, to the end of their last block (standard §3.3.2); the largest
/// 64-bit size for a size beyond it.
[[nodiscard]] std::uint64_t data_records(std::uint64_t size) noexcept;

/// The pairs of integers that sum_of() adds in 64 bits before it folds them into the sum: each pair adds less than
/// 2^33, so that 2^30 of them never overflow.
inline constexpr std::size_t sum_round = std::size_t{1} << 30;

/// The sum of bytes taken as big-endian 32-bit integers from their first byte, a last integer that they end within
/// completed with zeros: 0 for bytes that are all zeros, and never 0 otherwise. round is the pairs of integers added
/// before each fold, a parameter for the tests.
[[nodiscard]] std::uint32_t sum_of(std::span<const std::byte> bytes, std::size_t round = sum_round) noexcept;

/// The sum of bytes given in pieces of any size, in order, each integer continuing across the pieces as sum_of() of
/// all of them would take it.
class checksum_accumulator
{
public:
    /// Adds bytes that follow those added so far.
    void add(std::span<const std::byte> bytes) noexcept;

    /// Adds count zero bytes that follow those added so far, which change no sum but the places of the bytes after
    /// them.
    void add_zeros(std::uint64_t count) noexcept;

    /// The sum of the bytes added so far.
    [[nodiscard]] std::uint32_t sum() const noexcept;

private:
    std::uint32_t sum_ = 0;
    // The bytes of an integer that the last piece ended within, in their places, and their number.
    std::uint32_t pending_ = 0;
    unsigned pending_bytes_ = 0;
};

/// A sink that sums the bytes written to it, in order, and writes them to its target when it has one.
class summing_sink final : public output_sink
{
public:
    explicit summing_sink(output_sink* target = nullptr) noexcept : target_(target) {}

    void write(std::span<const std::byte> data) override;
    /// The zeros of the target, which add nothing to the sum.
    void write_zeros(std::uint64_t count) override;
    [[nodiscard]] std::uint64_t position() const override;
    /// Whether the target can rewrite.
    [[nodiscard]] bool can_rewrite() const override;
    /// Rewrites bytes of the target, which the sum does not see: the caller adds what they change to it.
    void rewrite(std::uint64_t offset, std::span<const std::byte> data) override;

    /// The sum of the bytes written.
    [[nodiscard]] std::uint32_t sum() const noexcept
    {
        return sum_.sum();
    }

private:
    output_sink* target_;
    checksum_accumulator sum_{};
    std::uint64_t written_ = 0;
};

/// Adds to sum the bytes of source from offset, size of them, or those to its end when it ends first: read in pieces
/// of a few MiB, or seen in pieces of its content in memory, advance called with the bytes of each.
/// @throws the exceptions of the source and of advance.
void add_source_bytes(checksum_accumulator& sum, const thread_safe_source& source, std::uint64_t offset,
                      std::uint64_t size, const std::function<void(std::uint64_t)>& advance = {});

/// What the DATASUM of a header records (standard §4.4.2.7).
struct recorded_datasum
{
    /// False without DATASUM, or with a value that is blank, null or undefined, which asserts nothing of the sum.
    bool known = false;
    /// The sum that it records: its string of decimal digits, between spaces and after zeros, or the integer of a
    /// DATASUM written without quotes; nothing for any other value, which no data match.
    std::optional<std::uint32_t> value{};
};

/// The DATASUM of a header, the first card of the keyword.
[[nodiscard]] recorded_datasum datasum_of(const header& cards);

/// True when the header has a CHECKSUM whose value is not blank, null or undefined.
[[nodiscard]] bool has_checksum(const header& cards);

/// The departures of the DATASUM and CHECKSUM of the header of the unit hdu, which starts at header_offset, as
/// warnings: a value that is not a string, a DATASUM that records no sum; and the header part of CHECKSUM, given a
/// DATASUM of a sum and the sum of the blocks of the header in header_sum: the header and the data that DATASUM
/// records sum to negative zero, or the header changed after CHECKSUM was computed (errc::checksum_mismatch).
/// @throws the exceptions of log in strict mode.
void check_checksum_keywords(const header& cards, std::optional<std::uint32_t> header_sum, std::uint64_t hdu,
                             std::uint64_t header_offset, diagnostic_log& log);

/// The checksums of unit, read from source whole: the sum of the blocks of its header and that of its data records,
/// the fill of their last block included, of the bytes that a truncated file holds; DATASUM against the second, and
/// CHECKSUM against the sum of both, negative zero when it is valid. progress, when there is one, gets the bytes read
/// of those of the unit, at the start and after each piece.
/// @throws the error that leaves the data of the unit unavailable when its header gives no size, the exceptions of the
///         source, and cancelled_error (errc::cancelled) when progress returns false.
[[nodiscard]] checksum_report verify_unit(const thread_safe_source& source, const hdu_info& unit,
                                          const progress_function& progress = {});

/// The integrity_error (errc::checksum_mismatch) of the unit hdu, whose data records sum to sum where its DATASUM
/// records something else.
[[noreturn]] void throw_datasum_mismatch(std::uint64_t hdu, std::uint32_t sum, const recorded_datasum& recorded);

/// The integrity_error (errc::checksum_mismatch) of the first sum of report that is invalid, for the unit hdu.
[[noreturn]] void throw_checksum_mismatch(std::uint64_t hdu, const checksum_report& report);

/// The cards that the writer puts after those of a header for its checksums: CHECKSUM with the placeholder of 16
/// zeros of standard Appendix J.1, then DATASUM of 0, where CFITSIO and astropy put them, each with comment.
[[nodiscard]] std::vector<card> checksum_cards(std::string_view comment);

/// A range of bytes.
struct byte_range
{
    std::size_t offset = 0;
    std::size_t size = 0;
};

/// Completes the checksums of the blocks of a header that hold the records of checksum_cards(): DATASUM records
/// data_sum, with comment, and CHECKSUM then makes the sum of the blocks and of the data negative zero (standard
/// Appendix J.1). Returns the bytes changed, those of the two records.
/// @throws the errors of write_value_record() for the record of DATASUM.
byte_range seal_checksums(std::string& blocks, std::uint32_t data_sum, std::string_view comment,
                          const record_options& options);

} // namespace openfits::detail
