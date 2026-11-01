// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>
#include <openfits/types.h>

#include <limits>

namespace openfits {

std::uint64_t geometry::element_count() const
{
    if (axes.empty()) {
        return 0;
    }
    std::uint64_t count = 1;
    for (const std::uint64_t length : axes) {
        if (length != 0 && count > std::numeric_limits<std::uint64_t>::max() / length) {
            throw usage_error(errc::arithmetic_overflow, "the number of elements of the axes does not fit in 64 bits");
        }
        count *= length;
    }
    return count;
}

} // namespace openfits
