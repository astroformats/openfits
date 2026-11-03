// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Checksums (standard §4.4.2.7, Appendix J) on untrusted bytes: the first 16 bytes decoded as the value of a CHECKSUM,
// whose sum must encode in alphanumerics and decode back; the input summed in pieces of sizes that it gives, which must
// make the sum of the whole; the input read as a file whose units are verified, under small limits; and an image of the
// input written with checksums, which must verify, and no longer verify once a bit that the input chooses is changed.
// A failure must be an error of the library's kinds; any other exception escapes and fails the run.

#include "checksum/checksum.h"

#include <openfits/checksum.h>
#include <openfits/error.h>
#include <openfits/image.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/writer.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using openfits::checksum_report;
using openfits::checksum_state;
using openfits::reader;

constexpr std::uint32_t negative_zero = 0xFFFFFFFFU;

// Stops the run, so that the fuzzer reports the input that broke a property.
void require(bool condition)
{
    if (!condition) {
        std::abort();
    }
}

bool alphanumeric(char c) noexcept
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

openfits::read_options small_limits()
{
    openfits::read_options options;
    options.limits.max_hdus = 16;
    options.limits.max_header_bytes = std::uint64_t{64} * 2880;
    options.limits.max_total_header_bytes = std::uint64_t{128} * 2880;
    options.limits.max_diagnostics = 64;
    return options;
}

void decode(std::span<const std::byte> input)
{
    if (input.size() < 16) {
        return;
    }
    const std::string_view text(reinterpret_cast<const char*>(input.data()), 16);
    const std::optional<std::uint32_t> sum = openfits::decode_checksum(text);
    require(sum.has_value());
    const std::string encoded = openfits::encode_checksum(sum.value_or(0));
    require(encoded.size() == 16 && std::ranges::all_of(encoded, alphanumeric));
    require(openfits::decode_checksum(encoded) == sum);
}

void sum_in_pieces(std::span<const std::byte> input)
{
    const std::uint32_t whole = openfits::detail::sum_of(input);
    openfits::detail::checksum_accumulator pieces;
    std::size_t step = input.empty() ? 1 : (std::to_integer<std::size_t>(input.front()) % 13) + 1;
    for (std::size_t at = 0; at < input.size();) {
        const std::size_t count = std::min(step, input.size() - at);
        pieces.add(input.subspan(at, count));
        at += count;
        step = (((step * 7) + 3) % 29) + 1;
    }
    require(pieces.sum() == whole);
    require(openfits::detail::sum_of(input, (input.size() % 5) + 1) == whole);
    require((whole == 0) == std::ranges::all_of(input, [](std::byte value) { return value == std::byte{0}; }));
}

void verify_file(std::span<const std::byte> input)
{
    try {
        const reader file(std::make_unique<openfits::memory_source>(input), small_limits());
        const std::size_t count = file.hdu_count();
        for (std::size_t i = 0; i < count; ++i) {
            try {
                const checksum_report report = file.verify(i);
                // CHECKSUM is valid exactly when the unit sums to negative zero.
                if (report.checksum != checksum_state::absent) {
                    require((report.checksum == checksum_state::valid) == (report.hdu_sum == negative_zero));
                }
            } catch (const openfits::error& failure) {
                // A unit whose header gives no size; verify() reports a mismatch, and never throws one.
                require(failure.code() != openfits::errc::checksum_mismatch);
            }
        }
    } catch (const openfits::error& failure) {
        // Not a FITS file, or one beyond the limits: an open that is not strict verifies no checksum.
        require(failure.code() != openfits::errc::checksum_mismatch);
    }
}

// Not both sums valid: a change of the unit is found, or leaves the unit unreadable.
bool changed(std::span<const std::byte> file)
{
    try {
        const reader input(std::make_unique<openfits::memory_source>(file));
        const checksum_report report = input.verify(0);
        return report.datasum != checksum_state::valid || report.checksum != checksum_state::valid;
    } catch (const openfits::error&) {
        return true;
    }
}

void write_with_checksums(std::span<const std::byte> input)
{
    openfits::image_info info;
    info.geometry.axes = {input.size()};
    openfits::writer output;
    (void)output.add_image(
        info, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(input.data()), input.size()));
    openfits::memory_sink sink;
    output.save(sink, {.checksums = true});
    std::vector<std::byte> file = sink.release();
    {
        const reader written(std::make_unique<openfits::memory_source>(std::span<const std::byte>(file)));
        const checksum_report report = written.verify(0);
        require(report.datasum == checksum_state::valid && report.checksum == checksum_state::valid);
        if (!input.empty()) {
            require(written.read_pixels<std::uint8_t>(0).size() == input.size());
        }
    }
    if (!input.empty()) {
        const std::size_t bit = (std::to_integer<std::size_t>(input.back()) * 7919) % (file.size() * 8);
        file[bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
        require(changed(file));
    }
}

} // namespace

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::span<const std::byte> input(reinterpret_cast<const std::byte*>(data), size);
    decode(input);
    sum_in_pieces(input);
    verify_file(input);
    // Images of up to 64 KiB, which the writer writes quickly.
    write_with_checksums(input.first(std::min<std::size_t>(input.size(), std::size_t{1} << 16)));
    return 0;
}
