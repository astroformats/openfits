// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Files compressed whole with gzip (RFC 1952), a convention outside the standard, on untrusted bytes. The first byte
// chooses what the rest is: a gzip file as it is, or content that the target compresses into one member, or into two,
// which gzip_source must inflate back. The content of every input that inflates within the limits is compressed again
// by gzip_sink, at a level that the input chooses, and must inflate to itself; and it is read as a FITS file, through
// the reader that recognizes the gzip member, its units walked and verified. A failure must be an error of the
// library's kinds; any other exception escapes and fails the run.

#include <openfits/error.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/verify.h>

#include "compress/gzip.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace {

void require(bool condition)
{
    if (!condition) {
        std::abort();
    }
}

std::vector<std::byte> gzip_member(std::span<const std::byte> content, int level)
{
    std::vector<std::byte> stream;
    openfits::detail::gzip_encoder(level).compress(content, stream);
    return stream;
}

// The content of a gzip file, or nothing when it is refused, with one of the errors that a gzip file can give.
std::optional<std::vector<std::byte>> inflated(std::span<const std::byte> file, const openfits::limits& limits)
{
    try {
        const openfits::gzip_source source(openfits::memory_source(file), limits);
        require(source.bytes().size() == source.size());
        return std::vector<std::byte>(source.bytes().begin(), source.bytes().end());
    } catch (const openfits::integrity_error& failure) {
        require(failure.code() == openfits::errc::corrupt_compressed_data);
    } catch (const openfits::limit_error& failure) {
        require(failure.code() == openfits::errc::inflated_too_large);
    }
    return std::nullopt;
}

} // namespace

// The entry point that libFuzzer, and the replay driver, call for every input. Its name is fixed by libFuzzer.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 1) {
        return 0;
    }
    const std::span<const std::byte> input(reinterpret_cast<const std::byte*>(data), size);
    const std::span<const std::byte> rest = input.subspan(1);
    const auto mode = std::to_integer<unsigned>(input[0]);
    const int level = static_cast<int>(mode % 10);
    openfits::limits limits;
    limits.max_inflated_bytes = std::uint64_t{1} << 20;
    limits.max_hdus = 16;
    limits.max_diagnostics = 64;

    std::vector<std::byte> file;
    std::optional<std::vector<std::byte>> expected;
    switch (mode % 3) {
    case 0:
        file.assign(rest.begin(), rest.end());
        break;
    case 1:
        file = gzip_member(rest, level);
        expected.emplace(rest.begin(), rest.end());
        break;
    default: {
        // Two members, split where the content says, and zeros after them as on a tape.
        const std::size_t split = rest.empty() ? 0 : std::to_integer<std::size_t>(rest[0]) % rest.size();
        file = gzip_member(rest.first(split), level);
        const std::vector<std::byte> second = gzip_member(rest.subspan(split), 9 - level);
        file.insert(file.end(), second.begin(), second.end());
        file.resize(file.size() + (mode / 3 % 4), std::byte{0});
        expected.emplace(rest.begin(), rest.end());
        break;
    }
    }

    const std::optional<std::vector<std::byte>> content = inflated(file, limits);
    if (expected && expected->size() <= limits.max_inflated_bytes) {
        require(content && *content == *expected);
    }
    if (!content) {
        return 0;
    }

    // Compressed again, and inflated to the same content.
    openfits::memory_sink compressed;
    openfits::gzip_sink sink(compressed, level);
    sink.write(*content);
    sink.finish();
    require(sink.position() == content->size());
    const std::optional<std::vector<std::byte>> again = inflated(compressed.data(), {});
    require(again && *again == *content);

    // The content as a FITS file, which the reader inflates by itself.
    try {
        const openfits::reader fits(std::make_unique<openfits::memory_source>(std::move(file)), {.limits = limits});
        require(fits.size() == content->size());
        const std::vector<openfits::diagnostic> report = openfits::verify(fits);
        std::size_t walked = fits.diagnostics().size();
        for (const openfits::hdu_info& unit : fits.hdus()) {
            walked += unit.diagnostics().size();
        }
        require(report.size() >= walked);
    } catch (const openfits::invalid_data_error&) {
        // A content that is not a FITS file, or whose header gives no unit.
        return 0;
    } catch (const openfits::limit_error&) {
        return 0;
    }
    return 0;
}
