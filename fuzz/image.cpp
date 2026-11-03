// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The values of an image (standard §3.3.2, §4.4.2.5, §5) read under hostile keywords and options: the input gives the
// BITPIX, the axes, the text of the values of BSCALE, BZERO and BLANK, the options of a read and its region, and the
// bytes that repeat through the data. The read must fail with an error of the library's kinds, or give the values that
// a read of the whole image gives at the elements of the region; and a source in memory, which the reader reads in
// place, and one that it reads through read(), in runs, must give the same values or the same error.

#include <openfits/error.h>
#include <openfits/image.h>
#include <openfits/io.h>
#include <openfits/reader.h>
#include <openfits/types.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using openfits::errc;
using openfits::reader;

void require(bool condition)
{
    if (!condition) {
        std::abort();
    }
}

// The bytes of the input, read in order; zeros once they run out.
class input_bytes
{
public:
    explicit input_bytes(std::span<const std::byte> data) : data_(data) {}

    std::uint8_t next()
    {
        return at_ < data_.size() ? std::to_integer<std::uint8_t>(data_[at_++]) : 0;
    }

    [[nodiscard]] std::span<const std::byte> rest() const
    {
        return data_.subspan(std::min(at_, data_.size()));
    }

private:
    std::span<const std::byte> data_;
    std::size_t at_ = 0;
};

std::string record(std::string text)
{
    text.resize(80, ' ');
    return text;
}

// A keyword with a value field of the 20 bytes of the input, made printable.
std::string hostile_record(const char* name, input_bytes& in)
{
    std::string text(name);
    text.resize(8, ' ');
    text += "= ";
    for (int i = 0; i < 20; ++i) {
        text += static_cast<char>(' ' + (in.next() % 95));
    }
    return record(text);
}

// What a read gave: its values, or the code of its error.
struct outcome
{
    std::vector<std::byte> values{};
    std::optional<errc> error{};

    friend bool operator==(const outcome&, const outcome&) = default;
};

// The errors that a read may give for a hostile file or hostile options.
bool expected_error(errc code)
{
    switch (code) {
    case errc::invalid_argument:
    case errc::arithmetic_overflow:
    case errc::allocation_too_large:
    case errc::value_out_of_range:
    case errc::undefined_value:
    case errc::reserved_keyword:
    case errc::keyword_type:
    case errc::keyword_not_found:
    case errc::keyword_undefined:
    case errc::invalid_structure:
    case errc::missing_keyword:
    case errc::not_an_image:
    case errc::end_of_data:
        return true;
    default:
        return false;
    }
}

template <typename T> outcome read(const reader& input, const openfits::pixel_read_options& options)
{
    outcome result;
    try {
        const std::vector<T> values = input.read_pixels<T>(0, options);
        result.values.resize(values.size() * sizeof(T));
        if (!values.empty()) {
            std::memcpy(result.values.data(), values.data(), result.values.size());
        }
    } catch (const openfits::error& failure) {
        require(expected_error(failure.code()));
        result.error = failure.code();
    }
    return result;
}

// The index in the data of each element of the region, in the order of the destination, its rows pitch apart (0 for
// none) with nothing between them.
std::vector<std::optional<std::uint64_t>> indices_of(const openfits::region& box,
                                                     const std::vector<std::uint64_t>& axes, std::uint64_t pitch)
{
    std::vector<std::vector<std::uint64_t>> picks(axes.size());
    for (std::size_t a = 0; a < axes.size(); ++a) {
        const std::int64_t step = box.step[a];
        const std::uint64_t stride = step < 0 ? 0 - static_cast<std::uint64_t>(step) : static_cast<std::uint64_t>(step);
        for (std::uint64_t k = 0; k * stride < box.size[a]; ++k) {
            picks[a].push_back(step > 0 ? box.start[a] + (k * stride) : box.start[a] + box.size[a] - 1 - (k * stride));
        }
    }
    const std::uint64_t row = picks[0].size();
    std::uint64_t rows = 1;
    for (std::size_t a = 1; a < axes.size(); ++a) {
        rows *= picks[a].size();
    }
    const std::uint64_t step = pitch == 0 ? row : pitch;
    std::vector<std::optional<std::uint64_t>> indices(((rows - 1) * step) + row);
    std::vector<std::uint64_t> position(axes.size(), 0);
    for (std::uint64_t r = 0; r < rows; ++r) {
        for (std::uint64_t k = 0; k < row; ++k) {
            std::uint64_t index = picks[0][k];
            std::uint64_t stride = axes[0];
            for (std::size_t a = 1; a < axes.size(); ++a) {
                index += picks[a][position[a]] * stride;
                stride *= axes[a];
            }
            indices[(r * step) + k] = index;
        }
        for (std::size_t a = 1; a < axes.size(); ++a) {
            if (++position[a] < picks[a].size()) {
                break;
            }
            position[a] = 0;
        }
    }
    return indices;
}

template <typename T>
void check(const reader& memory, const reader& streamed, const openfits::pixel_read_options& options,
           const std::vector<std::uint64_t>& axes)
{
    const outcome in_memory = read<T>(memory, options);
    const outcome through_reads = read<T>(streamed, options);
    require(in_memory == through_reads);
    if (in_memory.error || !options.region || options.blank == openfits::blank_policy::mask) {
        return;
    }
    // The values of the region are those of the whole image at its elements.
    openfits::pixel_read_options whole = options;
    whole.region.reset();
    whole.pitch = 0;
    const outcome all = read<T>(memory, whole);
    if (all.error) {
        // A value of the image beyond the region, which the policies refuse.
        require(*all.error == errc::value_out_of_range || *all.error == errc::undefined_value);
        return;
    }
    const std::vector<std::optional<std::uint64_t>> indices = indices_of(*options.region, axes, options.pitch);
    require(in_memory.values.size() == indices.size() * sizeof(T));
    for (std::size_t k = 0; k < indices.size(); ++k) {
        if (const std::optional<std::uint64_t>& index = indices[k]) {
            require(std::memcmp(in_memory.values.data() + (k * sizeof(T)), all.values.data() + (*index * sizeof(T)),
                                sizeof(T)) == 0);
        }
    }
}

} // namespace

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    input_bytes in({reinterpret_cast<const std::byte*>(data), size});
    constexpr std::array<int, 8> bitpix_values{8, 16, 32, 64, -32, -64, 12, 0};
    const int bitpix = bitpix_values[in.next() % bitpix_values.size()];
    const std::size_t naxis = in.next() % 5;
    std::vector<std::uint64_t> axes;
    axes.reserve(naxis);
    for (std::size_t a = 0; a < naxis; ++a) {
        axes.push_back(in.next() % 12);
    }
    std::string header = record("SIMPLE  =                    T") + record("BITPIX  = " + std::to_string(bitpix)) +
                         record("NAXIS   = " + std::to_string(naxis));
    for (std::size_t a = 0; a < naxis; ++a) {
        header += record("NAXIS" + std::to_string(a + 1) + " = " + std::to_string(axes[a]));
    }
    const std::uint8_t present = in.next();
    if ((present & 1U) != 0) {
        header += hostile_record("BSCALE", in);
    }
    if ((present & 2U) != 0) {
        header += hostile_record("BZERO", in);
    }
    if ((present & 4U) != 0) {
        header += hostile_record("BLANK", in);
    }
    header += record("END");
    header.resize((header.size() + 2879) / 2880 * 2880, ' ');

    // The options of the read.
    openfits::pixel_read_options options;
    const std::uint8_t type = in.next() % 10;
    options.blank = static_cast<openfits::blank_policy>(in.next() % 4);
    options.blank_value = static_cast<double>(static_cast<std::int8_t>(in.next()));
    options.overflow = static_cast<openfits::overflow_policy>(in.next() % 2);
    options.rounding = static_cast<openfits::rounding>(in.next() % 2);
    options.scaling = static_cast<openfits::scaling_policy>(in.next() % 3);
    options.override_scaling = {.scale = static_cast<double>(static_cast<std::int8_t>(in.next())) / 4.0,
                                .zero = static_cast<double>(static_cast<std::int8_t>(in.next())) * 1000.0};
    const bool with_region = (in.next() % 2) != 0 && !axes.empty() && std::ranges::find(axes, 0U) == axes.end();
    if (with_region) {
        openfits::region box;
        for (const std::uint64_t length : axes) {
            const std::uint64_t start = in.next() % length;
            box.start.push_back(start);
            box.size.push_back(1 + (in.next() % (length - start)));
            const int step = (in.next() % 7) - 3;
            box.step.push_back(step == 0 ? 1 : step);
        }
        options.region = box;
        const std::uint8_t pitch = in.next() % 4;
        if (pitch != 0) {
            const std::int64_t first = box.step[0] < 0 ? -box.step[0] : box.step[0];
            options.pitch = ((box.size[0] - 1) / static_cast<std::uint64_t>(first)) + pitch;
        }
    }

    // The data: the rest of the input, repeated, as many bytes as the axes give for the BITPIX.
    std::uint64_t count = axes.empty() ? 0 : 1;
    for (const std::uint64_t length : axes) {
        count *= length;
    }
    const std::uint64_t bytes = count * static_cast<std::uint64_t>(std::abs(bitpix) / 8);
    std::vector<std::byte> file(header.size() + ((bytes + 2879) / 2880 * 2880));
    std::memcpy(file.data(), header.data(), header.size());
    const std::span<const std::byte> rest = in.rest();
    for (std::uint64_t i = 0; i < bytes && !rest.empty(); ++i) {
        file[header.size() + i] = rest[i % rest.size()];
    }

    std::optional<reader> memory;
    std::optional<reader> streamed;
    try {
        memory.emplace(std::make_unique<openfits::memory_source>(file));
        streamed.emplace(std::make_unique<openfits::callback_source>(
            file.size(), [&file](std::uint64_t offset, std::span<std::byte> destination) {
                std::ranges::copy(std::span(file).subspan(offset, destination.size()), destination.begin());
            }));
    } catch (const openfits::invalid_data_error&) {
        return 0;
    }
    std::vector<std::uint8_t> mask;
    if (options.blank == openfits::blank_policy::mask) {
        // As many bytes as the region spans, which the read checks.
        std::uint64_t span = 0;
        try {
            span = memory
                       ->read_pixels<std::uint8_t>(0, {.region = options.region,
                                                       .pitch = options.pitch,
                                                       .scaling = openfits::scaling_policy::raw,
                                                       .overflow = openfits::overflow_policy::clamp})
                       .size();
        } catch (const openfits::error& failure) {
            require(expected_error(failure.code()));
        }
        mask.resize(span);
        options.mask = mask;
    }
    switch (type) {
    case 0:
        check<std::int8_t>(*memory, *streamed, options, axes);
        break;
    case 1:
        check<std::uint8_t>(*memory, *streamed, options, axes);
        break;
    case 2:
        check<std::int16_t>(*memory, *streamed, options, axes);
        break;
    case 3:
        check<std::uint16_t>(*memory, *streamed, options, axes);
        break;
    case 4:
        check<std::int32_t>(*memory, *streamed, options, axes);
        break;
    case 5:
        check<std::uint32_t>(*memory, *streamed, options, axes);
        break;
    case 6:
        check<std::int64_t>(*memory, *streamed, options, axes);
        break;
    case 7:
        check<std::uint64_t>(*memory, *streamed, options, axes);
        break;
    case 8:
        check<float>(*memory, *streamed, options, axes);
        break;
    default:
        check<double>(*memory, *streamed, options, axes);
        break;
    }
    return 0;
}
