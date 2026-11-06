// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/version.h>

namespace openfits {

std::string_view version() noexcept
{
    return OPENFITS_VERSION_STRING;
}

version_info version_numbers() noexcept
{
    return {.major = OPENFITS_VERSION_MAJOR, .minor = OPENFITS_VERSION_MINOR, .patch = OPENFITS_VERSION_PATCH};
}

} // namespace openfits
