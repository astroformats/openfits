// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <cstdint>

namespace openfits::test {

/// The values of the SplitMix64 sequence from a seed, for tests that sample a space of values: the same values on every
/// platform, which the distributions of <random> do not give, so that a failure on one platform reproduces on another.
class splitmix64
{
public:
    explicit splitmix64(std::uint64_t seed) noexcept : state_(seed) {}

    [[nodiscard]] std::uint64_t next() noexcept
    {
        state_ += 0x9E37'79B9'7F4A'7C15U;
        std::uint64_t value = state_;
        value = (value ^ (value >> 30U)) * 0xBF58'476D'1CE4'E5B9U;
        value = (value ^ (value >> 27U)) * 0x94D0'49BB'1331'11EBU;
        return value ^ (value >> 31U);
    }

private:
    std::uint64_t state_;
};

} // namespace openfits::test
