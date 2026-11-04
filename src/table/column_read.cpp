// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "table/column_read.h"

#include <openfits/error.h>

#include "core/ascii.h"
#include "core/endian.h"
#include "core/scratch_buffer.h"
#include "data/conversion.h"
#include "data/conversion_kernels.h"
#include "data/image_description.h"
#include "io/range_check.h"
#include "table/ascii_field.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace openfits::detail {

namespace {

// Rows separated by less than this are read together, through the staging chunk; see image_read.cpp.
constexpr std::uint64_t coalescing_gap = std::uint64_t{64} << 10;

// Fields that hold at least a quarter of the bytes of their rows are read in chunks of whole rows, whatever the gaps.
constexpr std::uint64_t dense_share = 4;

// The logicals and bits converted at once, through a block of their own.
constexpr std::size_t staging_elements = std::size_t{64} << 10;

// Logical values as the conversion takes them: T, F, and the stored value of the undefined ones.
constexpr std::uint8_t logical_true = 1;
constexpr std::uint8_t logical_false = 0;
constexpr std::uint8_t logical_undefined = 2;

constexpr std::uint64_t max_uint64 = std::numeric_limits<std::uint64_t>::max();

[[noreturn]] void refuse(const std::string& message, std::uint64_t hdu, std::optional<std::uint64_t> column = {})
{
    throw usage_error(errc::invalid_argument, message, {.hdu = hdu, .column = column});
}

std::uint64_t checked_product(std::uint64_t a, std::uint64_t b, std::uint64_t hdu)
{
    if (a != 0 && b > max_uint64 / a) {
        throw usage_error(errc::arithmetic_overflow, "the size of a read of columns does not fit in 64 bits",
                          {.hdu = hdu});
    }
    return a * b;
}

std::string column_name(std::uint64_t number)
{
    return "column " + std::to_string(number);
}

// The format of a column of a table that describe_table() gives, which every column of it has.
const column_format& format_of(const column_info& column)
{
    if (!column.format) {
        throw usage_error(errc::invalid_argument, "a column of the table has no format");
    }
    return *column.format;
}

// The progress of a read, reported to the progress function of its options, which cancels it by returning false.
class read_progress
{
public:
    read_progress(const progress_function& function, std::uint64_t total, std::uint64_t hdu)
        : function_(function), total_(total), hdu_(hdu)
    {}

    void advance(std::uint64_t bytes)
    {
        done_ += bytes;
        report();
    }

    void report() const
    {
        if (function_ && !function_(std::min(done_, total_), total_)) {
            throw cancelled_error(errc::cancelled,
                                  "the read of the columns of unit " + std::to_string(hdu_) + " was cancelled",
                                  {.hdu = hdu_});
        }
    }

private:
    const progress_function& function_;
    std::uint64_t total_;
    std::uint64_t done_ = 0;
    std::uint64_t hdu_;
};

bool is_complex(element_type type) noexcept
{
    return type == element_type::complex_float32 || type == element_type::complex_float64;
}

// The numeric type of the values of an element type, or of the parts of a complex one.
numeric_type part_type(element_type type) noexcept
{
    switch (type) {
    case element_type::complex_float32:
        return numeric_type::float32;
    case element_type::complex_float64:
        return numeric_type::float64;
    default:
        return static_cast<numeric_type>(type);
    }
}

std::size_t element_size(element_type type) noexcept
{
    return (is_complex(type) ? 2 : 1) * size_of(part_type(type));
}

bool is_complex(column_type type) noexcept
{
    return type == column_type::complex_float32 || type == column_type::complex_float64;
}

// The type that a column of a binary table stores its numbers in, or the parts of its complex numbers.
bitpix stored_bitpix(column_type type) noexcept
{
    switch (type) {
    case column_type::int16:
        return bitpix::int16;
    case column_type::int32:
        return bitpix::int32;
    case column_type::int64:
        return bitpix::int64;
    case column_type::float32:
    case column_type::complex_float32:
        return bitpix::float32;
    case column_type::float64:
    case column_type::complex_float64:
        return bitpix::float64;
    case column_type::logical:
    case column_type::bit:
    case column_type::uint8:
    case column_type::character:
        break;
    }
    return bitpix::uint8;
}

// How the fields of a column convert.
enum class field_kind : std::uint8_t
{
    numbers,         // B, I, J, K, E and D, through the kernels of the conversions.
    complex_numbers, // C and M, whose parts convert as numbers.
    logicals,        // L, as the bytes 1, 0 and the undefined 2.
    bits,            // X, one byte 0 or 1 for each bit.
    ascii,           // The fields of an ASCII table, parsed.
    text,            // Characters, as strings.
};

field_kind kind_of(const column_format& format) noexcept
{
    if (format.field) {
        return field_kind::ascii;
    }
    switch (format.type) {
    case column_type::logical:
        return field_kind::logicals;
    case column_type::bit:
        return field_kind::bits;
    case column_type::complex_float32:
    case column_type::complex_float64:
        return field_kind::complex_numbers;
    case column_type::character:
        return field_kind::text;
    case column_type::uint8:
    case column_type::int16:
    case column_type::int32:
    case column_type::int64:
    case column_type::float32:
    case column_type::float64:
        break;
    }
    return field_kind::numbers;
}

// The scaling that a read applies to the values of a column.
scaling read_scaling(const column_info& column, const column_read_options& options, std::uint64_t hdu,
                     std::uint64_t number)
{
    switch (options.scaling) {
    case scaling_policy::apply:
        if (column.scaling.scale == 0.0) {
            throw invalid_data_error(errc::reserved_keyword,
                                     "TSCAL" + std::to_string(number) + " is 0, which leaves the physical values of " +
                                         column_name(number) + " of unit " + std::to_string(hdu) + " unavailable",
                                     {.hdu = hdu, .keyword = "TSCAL" + std::to_string(number), .column = number});
        }
        return column.scaling;
    case scaling_policy::raw:
        break;
    case scaling_policy::override:
        if (!std::isfinite(options.override_scaling.scale) || options.override_scaling.scale == 0.0 ||
            !std::isfinite(options.override_scaling.zero)) {
            refuse("the scaling of the read has a scale of 0, or a value that is not finite", hdu, number);
        }
        return options.override_scaling;
    }
    return {};
}

// Calls function with the C++ type of a numeric type, as a std::type_identity.
template <typename Function> decltype(auto) with_numeric_type(numeric_type type, const Function& function)
{
    switch (type) {
    case numeric_type::int8:
        return function(std::type_identity<std::int8_t>{});
    case numeric_type::uint8:
        return function(std::type_identity<std::uint8_t>{});
    case numeric_type::int16:
        return function(std::type_identity<std::int16_t>{});
    case numeric_type::uint16:
        return function(std::type_identity<std::uint16_t>{});
    case numeric_type::int32:
        return function(std::type_identity<std::int32_t>{});
    case numeric_type::uint32:
        return function(std::type_identity<std::uint32_t>{});
    case numeric_type::int64:
        return function(std::type_identity<std::int64_t>{});
    case numeric_type::uint64:
        return function(std::type_identity<std::uint64_t>{});
    case numeric_type::float32:
        return function(std::type_identity<float>{});
    case numeric_type::float64:
        break;
    }
    return function(std::type_identity<double>{});
}

// What a read needs to convert the fields of one column into one destination.
struct column_job
{
    const column_info* column = nullptr;
    // The n of TFORMn.
    std::uint64_t number = 0;
    field_kind kind = field_kind::numbers;
    element_type type = element_type::float64;
    std::size_t value_size = 0;
    std::byte* values = nullptr;
    // A byte for each element of the destination with blank_policy::mask; null otherwise.
    std::uint8_t* mask = nullptr;
    // The elements of a field, and its bytes in a row.
    std::uint64_t elements = 0;
    std::uint64_t field_size = 0;
    read_conversion conversion{};
    // For complex values with a TZEROn: the conversion of the real parts; conversion takes the imaginary ones.
    std::optional<read_conversion> real_conversion{};
    // The rules of the conversions of the fields of ASCII tables.
    scaling field_scaling{};
    blank_policy policy = blank_policy::nan;
    double blank_value = 0.0;
    overflow_policy overflow = overflow_policy::refuse;
    openfits::rounding rounding = openfits::rounding::truncate;
    // The digits after the implied decimal point of a field of an ASCII table.
    std::uint32_t decimals = 0;
    // The strings of a column of characters: substring_width characters each, substrings of them in each field.
    std::vector<std::string>* strings = nullptr;
    std::uint64_t substring_width = 0;
    std::uint64_t substrings = 0;
    bool trim = false;
    std::uint64_t clamped = 0;
    std::uint64_t hdu = 0;

    [[nodiscard]] std::byte* value_at(std::uint64_t element) const noexcept
    {
        return values + (element * value_size);
    }

    [[nodiscard]] std::uint8_t* mask_at(std::uint64_t element) const noexcept
    {
        return mask == nullptr ? nullptr : mask + element;
    }

    [[nodiscard]] error_context where() const
    {
        return {.hdu = hdu, .column = number};
    }
};

// The blocks that the conversions of logicals, bits and complex numbers go through, allocated when one needs them.
class staging
{
public:
    // Each allocates its block at its first call, and may throw std::bad_alloc then.
    [[nodiscard]] std::byte* first()
    {
        return grown(first_);
    }

    [[nodiscard]] std::byte* second()
    {
        return grown(second_);
    }

private:
    static std::byte* grown(std::vector<std::byte>& block)
    {
        if (block.empty()) {
            block.resize(staging_elements * 8);
        }
        return block.data();
    }

    std::vector<std::byte> first_;
    std::vector<std::byte> second_;
};

// Converts count stored elements of the column of job, contiguous at stored, into the destination from its element
// destination on.
void convert_elements(column_job& job, const std::byte* stored, std::uint64_t destination, std::uint64_t count,
                      staging& blocks);

void convert_logicals(column_job& job, const std::byte* stored, std::uint64_t destination, std::uint64_t count,
                      staging& blocks)
{
    std::byte* block_bytes = blocks.first();
    auto* mapped = reinterpret_cast<std::uint8_t*>(block_bytes);
    for (std::uint64_t first = 0; first < count; first += staging_elements) {
        const std::size_t block = std::min<std::uint64_t>(staging_elements, count - first);
        for (std::size_t i = 0; i < block; ++i) {
            const auto byte = static_cast<unsigned char>(stored[first + i]);
            if (byte == 'T') {
                mapped[i] = logical_true;
            } else if (byte == 'F') {
                mapped[i] = logical_false;
            } else if (byte == 0) {
                mapped[i] = logical_undefined;
            } else {
                throw invalid_data_error(errc::invalid_field,
                                         "a logical of " + column_name(job.number) + " of unit " +
                                             std::to_string(job.hdu) + " holds the byte " + std::to_string(byte) +
                                             ", which is none of T, F and 0 (standard §7.3.3.1)",
                                         job.where());
            }
        }
        job.clamped += convert_read(job.conversion, block_bytes, 1, job.value_at(destination + first),
                                    job.mask_at(destination + first), block);
    }
}

// The bits from first_bit to first_bit + count of bytes, the most significant bit of each byte first, as elements of 0
// and 1 from the element destination on.
void convert_bits(column_job& job, const std::byte* bytes, std::uint64_t first_bit, std::uint64_t count,
                  std::uint64_t destination, staging& blocks)
{
    std::byte* block_bytes = blocks.first();
    auto* unpacked = reinterpret_cast<std::uint8_t*>(block_bytes);
    for (std::uint64_t first = 0; first < count; first += staging_elements) {
        const std::size_t block = std::min<std::uint64_t>(staging_elements, count - first);
        for (std::size_t i = 0; i < block; ++i) {
            const std::uint64_t bit = first_bit + first + i;
            const auto byte = static_cast<unsigned>(bytes[bit / 8]);
            unpacked[i] = static_cast<std::uint8_t>((byte >> (7 - (bit % 8))) & 1U);
        }
        job.clamped += convert_read(job.conversion, block_bytes, 1, job.value_at(destination + first),
                                    job.mask_at(destination + first), block);
    }
}

// The undefined complex values, a NaN in either part (standard §7.3.3.1): marked in the mask, or given the value of
// blank_policy::value.
template <typename T> void complex_undefined(column_job& job, std::uint64_t destination, std::uint64_t count)
{
    if (job.policy == blank_policy::nan || job.policy == blank_policy::ignore) {
        return;
    }
    for (std::uint64_t i = 0; i < count; ++i) {
        std::byte* place = job.value_at(destination + i);
        std::array<T, 2> parts{};
        std::memcpy(parts.data(), place, sizeof(parts));
        const bool missing = std::isnan(parts[0]) || std::isnan(parts[1]);
        if (job.policy == blank_policy::mask) {
            job.mask[destination + i] = missing ? 1 : 0;
        } else if (missing) {
            parts = {static_cast<T>(job.blank_value), T{0}};
            std::memcpy(place, parts.data(), sizeof(parts));
        }
    }
}

void convert_complex(column_job& job, const std::byte* stored, std::uint64_t destination, std::uint64_t count,
                     staging& blocks)
{
    if (!job.real_conversion) {
        job.clamped += convert_read(job.conversion, stored, 1, job.value_at(destination), nullptr, 2 * count);
    } else {
        // The real parts take TZEROn and the imaginary ones not (standard §7.3.2): converted apart, then interleaved.
        const std::size_t stored_part = format_of(*job.column).type == column_type::complex_float32 ? 4 : 8;
        const std::size_t part = job.value_size / 2;
        for (std::uint64_t first = 0; first < count; first += staging_elements) {
            const std::size_t block = std::min<std::uint64_t>(staging_elements, count - first);
            const std::byte* parts = stored + (first * 2 * stored_part);
            std::byte* reals = blocks.first();
            std::byte* imaginaries = blocks.second();
            job.clamped += convert_read(*job.real_conversion, parts, 2, reals, nullptr, block);
            job.clamped += convert_read(job.conversion, parts + stored_part, 2, imaginaries, nullptr, block);
            std::byte* out = job.value_at(destination + first);
            for (std::size_t i = 0; i < block; ++i) {
                std::memcpy(out + (i * 2 * part), reals + (i * part), part);
                std::memcpy(out + (i * 2 * part) + part, imaginaries + (i * part), part);
            }
        }
    }
    if (job.type == element_type::complex_float32) {
        complex_undefined<float>(job, destination, count);
    } else {
        complex_undefined<double>(job, destination, count);
    }
}

void convert_elements(column_job& job, const std::byte* stored, std::uint64_t destination, std::uint64_t count,
                      staging& blocks)
{
    switch (job.kind) {
    case field_kind::numbers:
        job.clamped +=
            convert_read(job.conversion, stored, 1, job.value_at(destination), job.mask_at(destination), count);
        return;
    case field_kind::complex_numbers:
        convert_complex(job, stored, destination, count, blocks);
        return;
    case field_kind::logicals:
        convert_logicals(job, stored, destination, count, blocks);
        return;
    case field_kind::bits:
        convert_bits(job, stored, 0, count, destination, blocks);
        return;
    case field_kind::ascii:
    case field_kind::text:
        break;
    }
}

[[noreturn]] void refuse_field_value(const column_job& job, double real, std::string_view type)
{
    throw invalid_data_error(errc::value_out_of_range,
                             "the value " + std::to_string(real) + " of " + column_name(job.number) + " of unit " +
                                 std::to_string(job.hdu) + " is beyond the range of " + std::string(type),
                             job.where());
}

[[noreturn]] void refuse_field(const column_job& job, std::string_view field, errc problem)
{
    throw invalid_data_error(problem,
                             "the field '" + std::string(field) + "' of " + column_name(job.number) + " of unit " +
                                 std::to_string(job.hdu) +
                                 (problem == errc::invalid_field ? " is not a number of its format (standard §7.2.5)"
                                                                 : " is beyond the range of 64-bit integers or double"),
                             job.where());
}

[[noreturn]] void refuse_undefined_field(const column_job& job, std::string_view type)
{
    throw invalid_data_error(errc::undefined_value,
                             "an undefined field of " + column_name(job.number) + " of unit " +
                                 std::to_string(job.hdu) + " cannot be held in " + std::string(type) +
                                 " without a policy for undefined values",
                             job.where());
}

// A value of an ASCII table beyond the range of T, clamped or refused as the read says.
template <typename T> T field_overflow(column_job& job, double real)
{
    if (job.overflow == overflow_policy::refuse) {
        refuse_field_value(job, real, kernels::type_name<T>());
    }
    ++job.clamped;
    return kernels::clamp_real<T>(real);
}

template <typename T> void store_value(std::byte* out, T value) noexcept
{
    std::memcpy(out, &value, sizeof(T));
}

// The values of fields of an ASCII table into the destination, as the conversions of images give them: exact for
// integers under an integral offset, and otherwise zero + scale x value in double.
template <typename T> void store_real(column_job& job, std::byte* out, double real)
{
    const scaling& s = job.field_scaling;
    if (s.scale != 1.0 || s.zero != 0.0) {
        const double product = real * s.scale;
        real = product + s.zero;
    }
    if constexpr (std::same_as<T, double>) {
        store_value(out, real);
    } else if constexpr (std::same_as<T, float>) {
        if (std::isfinite(real) && std::abs(real) > static_cast<double>(std::numeric_limits<float>::max())) {
            store_value(out, field_overflow<T>(job, real));
            return;
        }
        store_value(out, static_cast<float>(real));
    } else {
        if (!kernels::real_fits<T>(real, job.rounding)) {
            store_value(out, field_overflow<T>(job, real));
            return;
        }
        store_value(out, kernels::real_to_integer<T>(real, job.rounding));
    }
}

// The offset of a scaling of 1 whose zero is an integer that keeps integers exact, or nothing.
std::optional<std::int64_t> integral_offset(const scaling& s) noexcept
{
    if (s.scale == 1.0 && s.zero == std::trunc(s.zero) && std::abs(s.zero) < 0x1p62) {
        return static_cast<std::int64_t>(s.zero);
    }
    return std::nullopt;
}

// An integer that the offset kept exact, a std::int64_t or a std::uint64_t.
template <typename T, typename I> void store_exact(column_job& job, std::byte* out, I sum)
{
    if constexpr (std::integral<T>) {
        if (!std::in_range<T>(sum)) {
            store_value(out, field_overflow<T>(job, static_cast<double>(sum)));
            return;
        }
    }
    store_value(out, static_cast<T>(sum));
}

template <typename T> void store_integer(column_job& job, std::byte* out, std::int64_t integer)
{
    if (const std::optional<std::int64_t> offset = integral_offset(job.field_scaling)) {
        if ((*offset >= 0 && integer <= std::numeric_limits<std::int64_t>::max() - *offset) ||
            (*offset < 0 && integer >= std::numeric_limits<std::int64_t>::min() - *offset)) {
            store_exact<T>(job, out, integer + *offset);
            return;
        }
    }
    store_real<T>(job, out, static_cast<double>(integer));
}

// An integer from 2^63 to 2^64 - 1.
template <typename T> void store_unsigned(column_job& job, std::byte* out, std::uint64_t integer)
{
    if (const std::optional<std::int64_t> offset = integral_offset(job.field_scaling)) {
        if (*offset <= 0) {
            store_exact<T>(job, out, integer - static_cast<std::uint64_t>(-*offset));
            return;
        }
        if (integer <= max_uint64 - static_cast<std::uint64_t>(*offset)) {
            store_exact<T>(job, out, integer + static_cast<std::uint64_t>(*offset));
            return;
        }
    }
    store_real<T>(job, out, static_cast<double>(integer));
}

template <typename T> void store_field(column_job& job, std::byte* out, const card_value& value)
{
    if (const auto* integer = std::get_if<std::int64_t>(&value.data())) {
        store_integer<T>(job, out, *integer);
    } else if (const auto* large = std::get_if<std::uint64_t>(&value.data())) {
        store_unsigned<T>(job, out, *large);
    } else {
        store_real<T>(job, out, value.get<double>());
    }
}

// An undefined field of an ASCII table into the destination, as the policy of the read says.
template <typename T> void store_undefined(column_job& job, std::uint64_t destination)
{
    std::byte* out = job.value_at(destination);
    T value{};
    switch (job.policy) {
    case blank_policy::value:
        value = static_cast<T>(job.blank_value);
        break;
    case blank_policy::mask:
        job.mask[destination] = 1;
        if constexpr (std::floating_point<T>) {
            value = std::numeric_limits<T>::quiet_NaN();
        }
        break;
    case blank_policy::nan:
    case blank_policy::ignore:
        if constexpr (std::floating_point<T>) {
            value = std::numeric_limits<T>::quiet_NaN();
        } else {
            refuse_undefined_field(job, kernels::type_name<T>());
        }
        break;
    }
    std::memcpy(out, &value, sizeof(T));
}

// A field of an ASCII table into the destination as T: the forms that most fields take without allocating, the others
// as Fortran reads them.
template <typename T> void convert_ascii(column_job& job, std::string_view field, std::uint64_t destination)
{
    const column_format& format = format_of(*job.column);
    if (job.policy != blank_policy::ignore && is_null_field(field, job.column->null_string)) {
        store_undefined<T>(job, destination);
        return;
    }
    if (job.mask != nullptr) {
        job.mask[destination] = 0;
    }
    std::byte* out = job.value_at(destination);
    const std::uint32_t decimals = job.decimals;
    if (format.type == column_type::int64) {
        if (const std::optional<std::int64_t> integer = read_plain_integer_field(field)) {
            store_integer<T>(job, out, *integer);
            return;
        }
    } else if (const std::optional<double> real = read_plain_real_field(field, decimals)) {
        store_real<T>(job, out, *real);
        return;
    }
    const field_number number =
        format.type == column_type::int64 ? read_integer_field(field) : read_real_field(field, decimals);
    if (number.problem) {
        refuse_field(job, field, *number.problem);
    }
    store_field<T>(job, out, number.value);
}

// The strings of a field of characters, up to the first NUL of each (standard §7.3.3.1).
void convert_text(column_job& job, const std::byte* field, std::uint64_t row)
{
    const auto* text = reinterpret_cast<const char*>(field);
    if (format_of(*job.column).field) {
        // A field of an ASCII table, as it is.
        std::string_view value(text, job.field_size);
        const bool missing = is_null_field(value, job.column->null_string);
        if (job.mask != nullptr) {
            job.mask[row] = missing ? 1 : 0;
        }
        if (missing) {
            value = {};
        } else if (job.trim) {
            value = trim_trailing_spaces(value);
        }
        job.strings->emplace_back(value);
        return;
    }
    for (std::uint64_t k = 0; k < job.substrings; ++k) {
        std::string_view value(text + (k * job.substring_width), job.substring_width);
        value = value.substr(0, value.find('\0'));
        if (job.trim) {
            value = trim_trailing_spaces(value);
        }
        if (job.mask != nullptr) {
            job.mask[(row * job.substrings) + k] = 0;
        }
        job.strings->emplace_back(value);
    }
}

// Converts the fields of rows of the chunk: the field of the row at index i of the chunk at fields + i x stride,
// first the index of the first row in the read.
void convert_fields(column_job& job, const std::byte* fields, std::uint64_t stride, std::uint64_t rows,
                    std::uint64_t first, std::vector<std::byte>& gather, staging& blocks)
{
    switch (job.kind) {
    case field_kind::ascii:
        with_numeric_type(part_type(job.type), [&]<typename T>(std::type_identity<T>) {
            for (std::uint64_t i = 0; i < rows; ++i) {
                const auto* text = reinterpret_cast<const char*>(fields + (i * stride));
                convert_ascii<T>(job, std::string_view(text, job.field_size), first + i);
            }
        });
        return;
    case field_kind::text:
        for (std::uint64_t i = 0; i < rows; ++i) {
            convert_text(job, fields + (i * stride), first + i);
        }
        return;
    case field_kind::bits:
        for (std::uint64_t i = 0; i < rows; ++i) {
            convert_bits(job, fields + (i * stride), 0, job.elements, (first + i) * job.elements, blocks);
        }
        return;
    case field_kind::numbers:
    case field_kind::complex_numbers:
    case field_kind::logicals:
        break;
    }
    if (job.field_size == 0) {
        return;
    }
    const std::byte* stored = fields;
    if (stride != job.field_size) {
        // The fields of the rows gathered one after the other, which the conversion then takes in one call.
        const std::size_t size = job.field_size;
        if (gather.size() < rows * size) {
            gather.resize(rows * size);
        }
        for (std::uint64_t i = 0; i < rows; ++i) {
            std::memcpy(gather.data() + (i * size), fields + (i * stride), size);
        }
        stored = gather.data();
    }
    convert_elements(job, stored, first * job.elements, rows * job.elements, blocks);
}

// The bytes of a stored element of a column, which a read of pieces keeps whole.
std::uint64_t stored_element_bytes(const column_job& job)
{
    switch (job.kind) {
    case field_kind::numbers:
        return stored_size(stored_bitpix(format_of(*job.column).type));
    case field_kind::complex_numbers:
        return 2 * stored_size(stored_bitpix(format_of(*job.column).type));
    case field_kind::logicals:
    case field_kind::bits:
        return 1;
    case field_kind::ascii:
    case field_kind::text:
        break;
    }
    return job.field_size;
}

// Reads the field of one row in pieces of at most transfer bytes, for fields too large for a chunk.
void read_field_in_pieces(const thread_safe_source& source, column_job& job, std::uint64_t offset, std::uint64_t row,
                          std::size_t transfer, std::vector<std::byte>& buffer, std::vector<std::byte>& gather,
                          staging& blocks)
{
    if (job.kind == field_kind::ascii || job.kind == field_kind::text) {
        // A field of an ASCII table, or a string, is read whole.
        const std::size_t size = job.field_size;
        if (buffer.size() < size) {
            buffer.resize(size);
        }
        source.read(offset, std::span<std::byte>(buffer).first(size));
        convert_fields(job, buffer.data(), job.field_size, 1, row, gather, blocks);
        return;
    }
    const std::uint64_t element = stored_element_bytes(job);
    const std::uint64_t per_piece = std::max<std::uint64_t>(1, transfer / element);
    const std::uint64_t stored_elements = job.kind == field_kind::bits ? job.field_size : job.elements;
    for (std::uint64_t done = 0; done < stored_elements;) {
        const std::uint64_t count = std::min(per_piece, stored_elements - done);
        const std::size_t size = count * element;
        if (buffer.size() < size) {
            buffer.resize(size);
        }
        source.read(offset + (done * element), std::span<std::byte>(buffer).first(size));
        if (job.kind == field_kind::bits) {
            const std::uint64_t bits = std::min(count * 8, job.elements - (done * 8));
            convert_bits(job, buffer.data(), 0, bits, (row * job.elements) + (done * 8), blocks);
        } else {
            convert_elements(job, buffer.data(), (row * job.elements) + done, count, blocks);
        }
        done += count;
    }
}

// Reads the rows of a selection of the table and converts the fields of every job: from memory in place, in chunks of
// whole rows, row by row for fields far apart in large rows, or field by field in pieces for fields larger than a
// chunk.
void read_rows(const thread_safe_source& source, const hdu_info& unit, const table_info& table, row_selection rows,
               std::span<column_job> jobs, read_progress& progress, std::size_t transfer)
{
    std::uint64_t low = max_uint64;
    std::uint64_t high = 0;
    for (const column_job& job : jobs) {
        if (job.field_size > 0) {
            low = std::min(low, job.column->offset);
            high = std::max(high, job.column->offset + job.field_size);
        }
    }
    if (low >= high || rows.count == 0) {
        progress.advance(rows.count * table.row_size);
        return;
    }
    const std::uint64_t row_size = table.row_size;
    const std::uint64_t span = high - low;
    const std::uint64_t start = unit.data_offset() + (rows.first * row_size) + low;
    std::vector<std::byte> gather;
    staging blocks;
    const auto convert_chunk = [&](const std::byte* base, std::uint64_t stride, std::uint64_t count,
                                   std::uint64_t first) {
        for (column_job& job : jobs) {
            convert_fields(job, base + (job.column->offset - low), stride, count, first, gather, blocks);
        }
    };
    const std::span<const std::byte> held = source.bytes();
    if (!held.empty()) {
        check_read_range(source.size(), start, ((rows.count - 1) * row_size) + span, source.description());
        const std::uint64_t per_chunk = std::max<std::uint64_t>(1, transfer / row_size);
        for (std::uint64_t done = 0; done < rows.count;) {
            const std::uint64_t count = std::min(per_chunk, rows.count - done);
            convert_chunk(held.data() + start + (done * row_size), row_size, count, done);
            done += count;
            progress.advance(count * row_size);
        }
        return;
    }
    if (span > transfer) {
        std::vector<std::byte> buffer;
        for (std::uint64_t row = 0; row < rows.count; ++row) {
            for (column_job& job : jobs) {
                const std::uint64_t offset = unit.data_offset() + ((rows.first + row) * row_size) + job.column->offset;
                if (job.field_size > 0) {
                    read_field_in_pieces(source, job, offset, row, transfer, buffer, gather, blocks);
                }
            }
            progress.advance(row_size);
        }
        return;
    }
    const bool dense = row_size - span < coalescing_gap || span * dense_share >= row_size;
    if (dense) {
        const std::uint64_t per_chunk = 1 + ((transfer - span) / row_size);
        const std::uint64_t chunk_rows = std::min(per_chunk, rows.count);
        scratch_buffer chunk(((chunk_rows - 1) * row_size) + span);
        for (std::uint64_t done = 0; done < rows.count;) {
            const std::uint64_t count = std::min(per_chunk, rows.count - done);
            const std::span<std::byte> bytes = chunk.bytes().first(((count - 1) * row_size) + span);
            source.read(start + (done * row_size), bytes);
            convert_chunk(bytes.data(), row_size, count, done);
            done += count;
            progress.advance(count * row_size);
        }
        return;
    }
    const std::uint64_t per_chunk = std::max<std::uint64_t>(1, transfer / span);
    scratch_buffer chunk(std::min(per_chunk, rows.count) * span);
    for (std::uint64_t done = 0; done < rows.count;) {
        const std::uint64_t count = std::min(per_chunk, rows.count - done);
        for (std::uint64_t i = 0; i < count; ++i) {
            source.read(start + ((done + i) * row_size), chunk.bytes().subspan(i * span, span));
        }
        convert_chunk(chunk.bytes().data(), span, count, done);
        done += count;
        progress.advance(count * row_size);
    }
}

// The job of a destination, its conversions prepared and its sizes checked against the rows.
column_job prepare(const table_info& table, const column_destination& target, const column_read_options& options,
                   std::uint64_t rows, std::uint64_t hdu)
{
    if (target.column >= table.columns.size()) {
        throw usage_error(errc::column_not_found,
                          "the table of unit " + std::to_string(hdu) + " has " + std::to_string(table.columns.size()) +
                              " columns, and none at index " + std::to_string(target.column),
                          {.hdu = hdu});
    }
    const column_info& column = table.columns[target.column];
    const column_format& format = format_of(column);
    column_job job;
    job.column = &column;
    job.number = target.column + 1;
    job.kind = kind_of(format);
    job.type = target.type;
    job.hdu = hdu;
    job.value_size = element_size(target.type);
    job.field_size = format.field_size();
    job.elements = field_elements(column);
    if (format.field) {
        job.decimals = format.field->digits.value_or(0);
    }
    job.policy = options.blank;
    job.blank_value = options.blank_value;
    job.overflow = options.overflow;
    job.rounding = options.rounding;
    if (format.storage != column_storage::fixed) {
        refuse(column_name(job.number) + " of unit " + std::to_string(hdu) +
                   " holds variable-length arrays, which read_arrays() reads",
               hdu, job.number);
    }
    if (job.kind == field_kind::text) {
        throw usage_error(errc::column_type,
                          column_name(job.number) + " of unit " + std::to_string(hdu) +
                              " holds characters, which read_strings() reads",
                          job.where());
    }
    if (is_complex(target.type) != is_complex(format.type)) {
        throw usage_error(errc::column_type,
                          column_name(job.number) + " of unit " + std::to_string(hdu) +
                              (is_complex(format.type) ? " holds complex numbers, which read as std::complex"
                                                       : " holds no complex numbers"),
                          job.where());
    }
    const std::uint64_t needed = checked_product(rows, job.elements, hdu);
    if (target.values.size() / job.value_size != needed || target.values.size() % job.value_size != 0) {
        refuse("the destination of " + column_name(job.number) + " has " +
                   std::to_string(target.values.size() / job.value_size) + " elements, and the rows need " +
                   std::to_string(needed),
               hdu, job.number);
    }
    if (options.blank == blank_policy::mask) {
        if (target.mask.size() != needed) {
            refuse("the mask of " + column_name(job.number) + " has " + std::to_string(target.mask.size()) +
                       " bytes, and the rows need " + std::to_string(needed),
                   hdu, job.number);
        }
        job.mask = target.mask.data();
    }
    job.values = target.values.data();
    const numeric_type to = part_type(target.type);
    switch (job.kind) {
    case field_kind::numbers:
        job.conversion = make_read_conversion(
            stored_bitpix(format.type), to, read_scaling(column, options, hdu, job.number), column.null, options.blank,
            options.blank_value, options.overflow, options.rounding, hdu);
        break;
    case field_kind::complex_numbers: {
        const scaling values = read_scaling(column, options, hdu, job.number);
        if (options.blank == blank_policy::value) {
            // The value of undefined complex values must convert into a part.
            (void)make_read_conversion(bitpix::float64, to, {}, std::nullopt, blank_policy::value, options.blank_value,
                                       options.overflow, options.rounding, hdu);
        }
        job.conversion = make_read_conversion(stored_bitpix(format.type), to, {.scale = values.scale}, std::nullopt,
                                              blank_policy::nan, 0.0, options.overflow, options.rounding, hdu);
        if (values.zero != 0.0) {
            job.real_conversion = make_read_conversion(stored_bitpix(format.type), to, values, std::nullopt,
                                                       blank_policy::nan, 0.0, options.overflow, options.rounding, hdu);
        }
        break;
    }
    case field_kind::logicals:
        // An undefined logical has no stored value to give, and stays undefined under blank_policy::ignore.
        job.conversion = make_read_conversion(bitpix::uint8, to, {}, logical_undefined,
                                              options.blank == blank_policy::ignore ? blank_policy::nan : options.blank,
                                              options.blank_value, options.overflow, options.rounding, hdu);
        // An undefined logical has no stored value to give, as BLANK gives one: it is always undefined.
        job.conversion.checks_undefined = true;
        break;
    case field_kind::bits:
        job.conversion = make_read_conversion(bitpix::uint8, to, {}, std::nullopt, options.blank, options.blank_value,
                                              options.overflow, options.rounding, hdu);
        break;
    case field_kind::ascii:
        job.field_scaling = format.type == column_type::character && options.scaling != scaling_policy::override
                                ? scaling{}
                                : read_scaling(column, options, hdu, job.number);
        if (options.blank == blank_policy::value) {
            (void)make_read_conversion(bitpix::float64, to, {}, std::nullopt, blank_policy::value, options.blank_value,
                                       options.overflow, options.rounding, hdu);
        }
        break;
    case field_kind::text:
        break;
    }
    return job;
}

} // namespace

row_selection select_rows(const std::optional<row_range>& rows, std::uint64_t table_rows, std::uint64_t hdu)
{
    if (!rows) {
        return {.first = 0, .count = table_rows};
    }
    if (rows->first > table_rows) {
        refuse("the rows of the read start at row " + std::to_string(rows->first) + ", and the table of unit " +
                   std::to_string(hdu) + " has " + std::to_string(table_rows),
               hdu);
    }
    const std::uint64_t count = rows->count.value_or(table_rows - rows->first);
    if (count > table_rows - rows->first) {
        refuse("the " + std::to_string(count) + " rows of the read from row " + std::to_string(rows->first) +
                   " go past the end of the table of unit " + std::to_string(hdu) + ", which has " +
                   std::to_string(table_rows),
               hdu);
    }
    return {.first = rows->first, .count = count};
}

std::uint64_t field_elements(const column_info& column) noexcept
{
    if (!column.format) {
        return 0;
    }
    return column.format->field ? 1 : column.format->repeat;
}

column_read_result read_columns(const thread_safe_source& source, const hdu_info& unit, const table_info& table,
                                std::span<const column_destination> columns, const column_read_options& options,
                                std::size_t transfer)
{
    const std::uint64_t hdu = unit.index();
    const row_selection rows = select_rows(options.rows, table.rows, hdu);
    std::vector<column_job> jobs;
    jobs.reserve(columns.size());
    for (const column_destination& target : columns) {
        jobs.push_back(prepare(table, target, options, rows.count, hdu));
    }
    read_progress progress(options.progress, checked_product(rows.count, table.row_size, hdu), hdu);
    progress.report();
    if (!unit.data_size()) {
        throw_unavailable(unit);
    }
    read_rows(source, unit, table, rows, jobs, progress, std::max<std::size_t>(transfer, 1));
    column_read_result result;
    for (const column_job& job : jobs) {
        result.clamped += job.clamped;
    }
    return result;
}

namespace {

// The default display formats of the columns of binary tables without TDISPn: those that show every value of their
// types, as CFITSIO shows them.
display_format default_display(const column_info& column)
{
    const column_format& format = format_of(column);
    const auto integer = [](std::uint32_t width) {
        return display_format{.code = display_code::integer, .width = width};
    };
    const auto general = [](std::uint32_t width, std::uint32_t digits) {
        return display_format{.code = display_code::general, .width = width, .digits = digits};
    };
    const auto exponential = [](std::uint32_t width, std::uint32_t digits) {
        return display_format{.code = display_code::exponential, .width = width, .digits = digits};
    };
    const bool scaled = column.value_type() == numeric_type::float32 || column.value_type() == numeric_type::float64;
    switch (format.type) {
    case column_type::logical:
        return {.code = display_code::logical, .width = 1};
    case column_type::bit:
        return {.code = display_code::binary, .width = 8, .digits = 8};
    case column_type::uint8:
        return scaled ? general(14, 6) : integer(4);
    case column_type::int16:
        return scaled ? general(14, 6) : integer(6);
    case column_type::int32:
        return scaled ? general(23, 15) : integer(11);
    case column_type::int64:
        return scaled ? general(23, 15) : integer(20);
    case column_type::float32:
        return general(14, 6);
    case column_type::complex_float32:
        return exponential(14, 6);
    case column_type::complex_float64:
        return exponential(23, 15);
    case column_type::character:
    case column_type::float64:
        break;
    }
    return general(23, 15);
}

// Reads a column of characters of a binary or an ASCII table as strings.
std::vector<std::string> read_text(const thread_safe_source& source, const hdu_info& unit, const table_info& table,
                                   const column_info& column, std::uint64_t number, row_selection rows,
                                   const column_read_options& options, const limits& limits, std::size_t transfer)
{
    const std::uint64_t hdu = unit.index();
    const column_format& format = format_of(column);
    column_job job;
    job.column = &column;
    job.number = number;
    job.kind = field_kind::text;
    job.hdu = hdu;
    job.field_size = format.field_size();
    job.trim = options.trim_strings;
    if (format.field) {
        job.substrings = 1;
    } else {
        // The substrings of TDIMn, or of rAw: the first dimension their width, the others their number.
        job.substring_width = column.dimensions.empty() ? format.repeat : column.dimensions.front();
        job.substrings = 1;
        for (std::size_t i = 1; i < column.dimensions.size(); ++i) {
            job.substrings = checked_product(job.substrings, column.dimensions[i], hdu);
        }
    }
    const std::uint64_t count = checked_product(rows.count, job.substrings, hdu);
    if (limits.max_allocation != 0 && checked_product(count, sizeof(std::string), hdu) > limits.max_allocation) {
        throw limit_error(errc::allocation_too_large,
                          "the " + std::to_string(count) + " strings of " + column_name(number) + " of unit " +
                              std::to_string(hdu) + " take more than limits::max_allocation",
                          job.where());
    }
    if (options.blank == blank_policy::mask && options.mask.size() != count) {
        refuse("the mask of " + column_name(number) + " has " + std::to_string(options.mask.size()) +
                   " bytes, and the strings need " + std::to_string(count),
               hdu, number);
    }
    job.mask = options.blank == blank_policy::mask ? options.mask.data() : nullptr;
    if (job.field_size == 0) {
        // Fields of no character: empty strings, which no read gives.
        if (job.mask != nullptr) {
            std::ranges::fill(options.mask, std::uint8_t{0});
        }
        return std::vector<std::string>(count);
    }
    std::vector<std::string> strings;
    strings.reserve(count);
    job.strings = &strings;
    read_progress progress(options.progress, checked_product(rows.count, table.row_size, hdu), hdu);
    progress.report();
    if (!unit.data_size()) {
        throw_unavailable(unit);
    }
    read_rows(source, unit, table, rows, std::span<column_job>(&job, 1), progress, std::max<std::size_t>(transfer, 1));
    return strings;
}

// The strings of the values of a column of numbers, logicals, bits or complex numbers, as its display format shows
// them.
std::vector<std::string> render(const thread_safe_source& source, const hdu_info& unit, const table_info& table,
                                std::size_t index, row_selection rows, const column_read_options& options,
                                const limits& limits, std::size_t transfer)
{
    const std::uint64_t hdu = unit.index();
    const column_info& column = table.columns[index];
    const column_format& format = format_of(column);
    // Bits display as the unsigned integers of their bytes (standard §7.3.2), read as a column of bytes.
    table_info bytes_table;
    const table_info* source_table = &table;
    std::size_t source_index = index;
    if (format.type == column_type::bit) {
        bytes_table.rows = table.rows;
        bytes_table.row_size = table.row_size;
        column_info bytes = column;
        bytes.format = column_format{.type = column_type::uint8, .repeat = format.field_size()};
        bytes_table.columns.push_back(std::move(bytes));
        source_table = &bytes_table;
        source_index = 0;
    }
    const column_info& read = source_table->columns[source_index];
    const display_format display = column.display.value_or(default_display(column));
    const std::uint64_t count = checked_product(rows.count, field_elements(read), hdu);
    if (limits.max_allocation != 0 && checked_product(count, sizeof(std::string) + 16, hdu) > limits.max_allocation) {
        throw limit_error(errc::allocation_too_large,
                          "the " + std::to_string(count) + " strings of column " + std::to_string(index + 1) +
                              " of unit " + std::to_string(hdu) + " take more than limits::max_allocation",
                          {.hdu = hdu, .column = index + 1});
    }
    if (options.blank == blank_policy::mask && options.mask.size() != count) {
        refuse("the mask of column " + std::to_string(index + 1) + " has " + std::to_string(options.mask.size()) +
                   " bytes, and the strings need " + std::to_string(count),
               hdu, index + 1);
    }
    std::vector<std::uint8_t> missing(count);
    column_read_options numbers = options;
    numbers.blank = blank_policy::mask;
    numbers.overflow = overflow_policy::refuse;
    std::vector<std::string> strings;
    strings.reserve(count);
    const auto read_as = [&]<typename T>(std::type_identity<T>) {
        std::vector<T> values(count);
        const column_destination target =
            column_destination::of(source_index, std::span<T>(values), std::span<std::uint8_t>(missing));
        (void)read_columns(source, unit, *source_table, std::span<const column_destination>(&target, 1), numbers,
                           transfer);
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (missing[i] != 0) {
                strings.emplace_back();
            } else if constexpr (std::same_as<T, std::uint8_t>) {
                if (format.type == column_type::logical) {
                    strings.push_back(format_display(display, values[i] != 0));
                } else {
                    strings.push_back(format_display(display, std::uint64_t{values[i]}));
                }
            } else {
                strings.push_back(format_display(display, values[i]));
            }
            if (options.trim_strings) {
                strings.back().resize(trim_trailing_spaces(strings.back()).size());
            }
        }
    };
    const numeric_type type = format.type == column_type::bit || format.type == column_type::logical
                                  ? numeric_type::uint8
                                  : read.value_type();
    if (format.type == column_type::complex_float32 || format.type == column_type::complex_float64) {
        read_as(std::type_identity<std::complex<double>>{});
    } else if (type == numeric_type::uint8 &&
               (format.type == column_type::bit || format.type == column_type::logical)) {
        read_as(std::type_identity<std::uint8_t>{});
    } else if (type == numeric_type::uint64) {
        read_as(std::type_identity<std::uint64_t>{});
    } else if (type == numeric_type::float32 || type == numeric_type::float64) {
        read_as(std::type_identity<double>{});
    } else {
        read_as(std::type_identity<std::int64_t>{});
    }
    if (options.blank == blank_policy::mask) {
        std::ranges::copy(missing, options.mask.begin());
    }
    return strings;
}

} // namespace

std::vector<std::string> read_strings(const thread_safe_source& source, const hdu_info& unit, const table_info& table,
                                      std::size_t column, const column_read_options& options, const limits& limits,
                                      std::size_t transfer)
{
    const std::uint64_t hdu = unit.index();
    if (column >= table.columns.size()) {
        throw usage_error(errc::column_not_found,
                          "the table of unit " + std::to_string(hdu) + " has " + std::to_string(table.columns.size()) +
                              " columns, and none at index " + std::to_string(column),
                          {.hdu = hdu});
    }
    const row_selection rows = select_rows(options.rows, table.rows, hdu);
    const column_info& info = table.columns[column];
    const column_format& format = format_of(info);
    const std::uint64_t number = column + 1;
    if (format.storage != column_storage::fixed) {
        if (format.type != column_type::character) {
            throw usage_error(errc::column_type,
                              column_name(number) + " of unit " + std::to_string(hdu) +
                                  " holds variable-length arrays of numbers, which read_arrays() reads",
                              {.hdu = hdu, .column = number});
        }
        // Each array of characters is a string: read as bytes, then cut at its first NUL.
        table_info bytes_table = table;
        column_info& bytes_column = bytes_table.columns[column];
        column_format bytes_format = format_of(bytes_column);
        bytes_format.type = column_type::uint8;
        bytes_column.format = bytes_format;
        bytes_column.scaling = {};
        bytes_column.null.reset();
        if (options.blank == blank_policy::mask && options.mask.size() != rows.count) {
            refuse("the mask of " + column_name(number) + " has " + std::to_string(options.mask.size()) +
                       " bytes, and the strings need " + std::to_string(rows.count),
                   hdu, number);
        }
        std::vector<std::uint8_t> characters;
        std::vector<std::uint64_t> offsets;
        std::vector<std::uint8_t> mask;
        column_read_options bytes = options;
        bytes.blank = blank_policy::nan;
        read_arrays(
            source, unit, bytes_table, column, element_type::uint8, bytes, limits,
            [&characters](std::uint64_t elements) {
                characters.resize(elements);
                return std::as_writable_bytes(std::span<std::uint8_t>(characters));
            },
            offsets, mask, transfer);
        std::vector<std::string> strings;
        strings.reserve(rows.count);
        for (std::size_t row = 0; row + 1 < offsets.size(); ++row) {
            std::string_view value(reinterpret_cast<const char*>(characters.data()) + offsets[row],
                                   offsets[row + 1] - offsets[row]);
            value = value.substr(0, value.find('\0'));
            if (options.trim_strings) {
                value = trim_trailing_spaces(value);
            }
            strings.emplace_back(value);
        }
        if (options.blank == blank_policy::mask) {
            std::ranges::fill(options.mask, std::uint8_t{0});
        }
        return strings;
    }
    if (format.field) {
        // The fields of an ASCII table are text: as they are, or as TDISPn shows their values.
        if (!info.display || format.type == column_type::character) {
            return read_text(source, unit, table, info, number, rows, options, limits, transfer);
        }
        return render(source, unit, table, column, rows, options, limits, transfer);
    }
    if (format.type == column_type::character) {
        return read_text(source, unit, table, info, number, rows, options, limits, transfer);
    }
    return render(source, unit, table, column, rows, options, limits, transfer);
}

namespace {

// A descriptor of a variable-length array (standard §7.3.5): its elements, and the offset of the first in the heap.
struct descriptor
{
    std::uint64_t elements = 0;
    std::uint64_t offset = 0;
    std::uint64_t bytes = 0;
    std::uint64_t row = 0;
};

// Reads the descriptors of a column of the rows, and checks each against the heap and the limits.
std::vector<descriptor> read_descriptors(const thread_safe_source& source, const hdu_info& unit,
                                         const table_info& table, std::size_t index, row_selection rows,
                                         const limits& limits, read_progress& progress, std::size_t transfer)
{
    const std::uint64_t hdu = unit.index();
    const column_info& column = table.columns[index];
    const column_format& format = format_of(column);
    const std::uint64_t number = index + 1;
    const error_context where{.hdu = hdu, .column = number};
    const bool wide = format.storage == column_storage::variable_q;
    // The descriptors as bytes: two 32-bit integers for P, read unsigned as CFITSIO reads them, and two 64-bit
    // integers for Q, of which a negative one is refused (standard §7.3.5 leaves negative values undefined).
    table_info bytes_table;
    bytes_table.rows = table.rows;
    bytes_table.row_size = table.row_size;
    column_info bytes = column;
    bytes.format = column_format{.type = column_type::uint8, .repeat = format.field_size()};
    bytes.scaling = {};
    bytes.null.reset();
    bytes_table.columns.push_back(std::move(bytes));
    std::vector<std::uint8_t> raw(checked_product(rows.count, format.field_size(), hdu));
    const column_destination target = column_destination::of(0, std::span<std::uint8_t>(raw));
    column_read_options plain;
    plain.rows = row_range{.first = rows.first, .count = rows.count};
    (void)read_columns(source, unit, bytes_table, std::span<const column_destination>(&target, 1), plain, transfer);
    progress.advance(rows.count * table.row_size);

    std::vector<descriptor> found;
    if (format.repeat == 0) {
        found.resize(rows.count);
        for (std::size_t row = 0; row < found.size(); ++row) {
            found[row].row = row;
        }
        return found;
    }
    found.reserve(rows.count);
    const auto* data = reinterpret_cast<const std::byte*>(raw.data());
    const std::uint64_t size = format.field_size();
    for (std::uint64_t row = 0; row < rows.count; ++row) {
        const std::byte* at = data + (row * size);
        descriptor entry;
        entry.row = row;
        if (wide) {
            const auto elements =
                static_cast<std::int64_t>(load_big_endian<std::uint64_t>(std::span<const std::byte, 8>(at, 8)));
            const auto offset =
                static_cast<std::int64_t>(load_big_endian<std::uint64_t>(std::span<const std::byte, 8>(at + 8, 8)));
            if (elements < 0 || offset < 0) {
                throw invalid_data_error(errc::invalid_descriptor,
                                         "the descriptor of row " + std::to_string(rows.first + row) + " of " +
                                             column_name(number) + " of unit " + std::to_string(hdu) +
                                             " holds a negative value (standard §7.3.5)",
                                         where);
            }
            entry.elements = static_cast<std::uint64_t>(elements);
            entry.offset = static_cast<std::uint64_t>(offset);
        } else {
            entry.elements = load_big_endian<std::uint32_t>(std::span<const std::byte, 4>(at, 4));
            entry.offset = load_big_endian<std::uint32_t>(std::span<const std::byte, 4>(at + 4, 4));
        }
        if (limits.max_vla_elements != 0 && entry.elements > limits.max_vla_elements) {
            throw limit_error(errc::array_too_large,
                              "the array of row " + std::to_string(rows.first + row) + " of " + column_name(number) +
                                  " of unit " + std::to_string(hdu) + " has " + std::to_string(entry.elements) +
                                  " elements, more than limits::max_vla_elements, " +
                                  std::to_string(limits.max_vla_elements),
                              where);
        }
        switch (format.type) {
        case column_type::bit:
            entry.bytes = (entry.elements / 8) + (entry.elements % 8 == 0 ? 0 : 1);
            break;
        default:
            entry.bytes = checked_product(entry.elements, column_format{.type = format.type}.field_size(), hdu);
            break;
        }
        // An empty array has no data, and its offset is undefined (standard §7.3.5).
        if (entry.bytes > 0 && (entry.offset > table.heap_size || entry.bytes > table.heap_size - entry.offset)) {
            throw invalid_data_error(errc::invalid_descriptor,
                                     "the array of row " + std::to_string(rows.first + row) + " of " +
                                         column_name(number) + " of unit " + std::to_string(hdu) + ", " +
                                         std::to_string(entry.bytes) + " bytes at offset " +
                                         std::to_string(entry.offset) + ", lies outside the heap of " +
                                         std::to_string(table.heap_size) + " bytes (standard §7.3.5)",
                                     where);
        }
        found.push_back(entry);
    }
    return found;
}

} // namespace

void read_arrays(const thread_safe_source& source, const hdu_info& unit, const table_info& table, std::size_t column,
                 element_type type, const column_read_options& options, const limits& limits,
                 const std::function<std::span<std::byte>(std::uint64_t)>& allocate,
                 std::vector<std::uint64_t>& offsets, std::vector<std::uint8_t>& mask, std::size_t transfer)
{
    const std::uint64_t hdu = unit.index();
    if (column >= table.columns.size()) {
        throw usage_error(errc::column_not_found,
                          "the table of unit " + std::to_string(hdu) + " has " + std::to_string(table.columns.size()) +
                              " columns, and none at index " + std::to_string(column),
                          {.hdu = hdu});
    }
    transfer = std::max<std::size_t>(transfer, 1);
    const row_selection rows = select_rows(options.rows, table.rows, hdu);
    const column_info& info = table.columns[column];
    const column_format& format = format_of(info);
    const std::uint64_t number = column + 1;
    const std::size_t value_size = element_size(type);
    const auto check_allocation = [&](std::uint64_t elements) {
        const std::uint64_t most = limits.max_allocation;
        if (most != 0 && checked_product(elements, value_size, hdu) > most) {
            throw limit_error(errc::allocation_too_large,
                              "the " + std::to_string(elements) + " elements of the arrays of " + column_name(number) +
                                  " of unit " + std::to_string(hdu) + " take more than limits::max_allocation, " +
                                  std::to_string(most) + " bytes",
                              {.hdu = hdu, .column = number});
        }
    };
    if (format.storage == column_storage::fixed) {
        const std::uint64_t per_row = field_elements(info);
        const std::uint64_t total = checked_product(rows.count, per_row, hdu);
        check_allocation(total);
        const std::span<std::byte> values = allocate(total);
        if (options.blank == blank_policy::mask) {
            mask.resize(total);
        }
        offsets.resize(rows.count + 1);
        for (std::size_t row = 0; row < offsets.size(); ++row) {
            offsets[row] = row * per_row;
        }
        const column_destination target{.column = column, .type = type, .values = values, .mask = mask};
        (void)read_columns(source, unit, table, std::span<const column_destination>(&target, 1), options, transfer);
        return;
    }
    // A job of the elements of the arrays, which the checks of a fixed field of their type prepare.
    if (limits.max_heap_bytes != 0 && table.heap_size > limits.max_heap_bytes) {
        throw limit_error(errc::array_too_large,
                          "the heap of unit " + std::to_string(hdu) + " has " + std::to_string(table.heap_size) +
                              " bytes, more than limits::max_heap_bytes, " + std::to_string(limits.max_heap_bytes),
                          {.hdu = hdu, .column = number});
    }
    table_info elements_table;
    column_info elements = info;
    elements.format = column_format{.type = format.type, .repeat = 1};
    elements_table.columns.push_back(std::move(elements));
    std::array<std::byte, 16> probe{};
    std::array<std::uint8_t, 1> probe_mask{};
    const column_destination probe_target{
        .column = 0, .type = type, .values = std::span<std::byte>(probe).first(value_size), .mask = probe_mask};
    column_read_options checks = options;
    checks.rows.reset();
    elements_table.rows = 1;
    column_job job = prepare(elements_table, probe_target, checks, 1, hdu);
    job.number = number;

    read_progress progress(options.progress, checked_product(rows.count, table.row_size, hdu), hdu);
    progress.report();
    if (!unit.data_size()) {
        throw_unavailable(unit);
    }
    std::vector<descriptor> descriptors =
        read_descriptors(source, unit, table, column, rows, limits, progress, transfer);
    offsets.resize(descriptors.size() + 1);
    std::uint64_t total = 0;
    for (std::size_t row = 0; row < descriptors.size(); ++row) {
        offsets[row] = total;
        if (descriptors[row].elements > max_uint64 - total) {
            throw usage_error(errc::arithmetic_overflow, "the elements of the arrays do not fit in 64 bits",
                              {.hdu = hdu, .column = number});
        }
        total += descriptors[row].elements;
    }
    offsets.back() = total;
    check_allocation(total);
    const std::span<std::byte> values = allocate(total);
    if (options.blank == blank_policy::mask) {
        mask.resize(total);
    }
    job.values = values.data();
    job.mask = options.blank == blank_policy::mask ? mask.data() : nullptr;

    // The arrays in the order of their offsets, so that the heap is read in ranges.
    std::ranges::sort(descriptors, {}, &descriptor::offset);
    const std::uint64_t heap = unit.data_offset() + table.heap_offset;
    const std::span<const std::byte> held = source.bytes();
    staging blocks;
    const auto convert = [&](const descriptor& entry, const std::byte* bytes) {
        if (job.kind == field_kind::bits) {
            convert_bits(job, bytes, 0, entry.elements, offsets[entry.row], blocks);
        } else {
            convert_elements(job, bytes, offsets[entry.row], entry.elements, blocks);
        }
    };
    if (!held.empty()) {
        for (const descriptor& entry : descriptors) {
            if (entry.bytes > 0) {
                check_read_range(source.size(), heap + entry.offset, entry.bytes, source.description());
                convert(entry, held.data() + heap + entry.offset);
                progress.advance(entry.bytes);
            }
        }
        return;
    }
    std::vector<std::byte> buffer;
    for (std::size_t i = 0; i < descriptors.size();) {
        const descriptor& first = descriptors[i];
        if (first.bytes == 0) {
            ++i;
            continue;
        }
        if (first.bytes > transfer) {
            // An array larger than the chunk, read in pieces of whole elements.
            const std::uint64_t element = stored_element_bytes(job);
            const std::uint64_t per_piece = std::max<std::uint64_t>(1, transfer / element) * element;
            for (std::uint64_t done = 0; done < first.bytes;) {
                const std::uint64_t size = std::min(per_piece, first.bytes - done);
                buffer.resize(size);
                source.read(heap + first.offset + done, buffer);
                const std::uint64_t at = done / element;
                if (job.kind == field_kind::bits) {
                    const std::uint64_t bits = std::min(size * 8, first.elements - (at * 8));
                    convert_bits(job, buffer.data(), 0, bits, offsets[first.row] + (at * 8), blocks);
                } else {
                    convert_elements(job, buffer.data(), offsets[first.row] + at, size / element, blocks);
                }
                done += size;
                progress.advance(size);
            }
            ++i;
            continue;
        }
        // The arrays that follow it within the gap and the chunk, read together.
        std::uint64_t end = first.offset + first.bytes;
        std::size_t last = i + 1;
        while (last < descriptors.size()) {
            const descriptor& next = descriptors[last];
            if (next.bytes == 0) {
                ++last;
                continue;
            }
            const std::uint64_t next_end = std::max(end, next.offset + next.bytes);
            if ((next.offset > end && next.offset - end >= coalescing_gap) || next_end - first.offset > transfer) {
                break;
            }
            end = next_end;
            ++last;
        }
        buffer.resize(end - first.offset);
        source.read(heap + first.offset, buffer);
        std::uint64_t bytes = 0;
        for (std::size_t k = i; k < last; ++k) {
            const descriptor& entry = descriptors[k];
            if (entry.bytes > 0) {
                convert(entry, buffer.data() + (entry.offset - first.offset));
                bytes += entry.bytes;
            }
        }
        progress.advance(bytes);
        i = last;
    }
}

} // namespace openfits::detail
