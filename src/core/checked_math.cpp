// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "core/checked_math.h"

namespace openfits::detail {

void throw_arithmetic_overflow()
{
    throw invalid_data_error(errc::arithmetic_overflow, "a size or offset is too large for its type");
}

} // namespace openfits::detail
