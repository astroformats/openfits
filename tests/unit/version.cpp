// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/openfits.h>

#include <gtest/gtest.h>

#include <string>

// The header comes from the build tree and the function from the linked library, so equality shows that
// the two belong together.
TEST(version, library_reports_the_version_of_the_header)
{
    EXPECT_EQ(openfits::version(), OPENFITS_VERSION_STRING);
}

TEST(version, string_is_made_of_the_numeric_macros)
{
    const std::string expected = std::to_string(OPENFITS_VERSION_MAJOR) + '.' + std::to_string(OPENFITS_VERSION_MINOR) +
                                 '.' + std::to_string(OPENFITS_VERSION_PATCH);
    EXPECT_EQ(openfits::version(), expected);
}

TEST(version, numbers_are_those_of_the_macros)
{
    const openfits::semantic_version numbers = openfits::version_numbers();

    EXPECT_EQ(numbers.major, OPENFITS_VERSION_MAJOR);
    EXPECT_EQ(numbers.minor, OPENFITS_VERSION_MINOR);
    EXPECT_EQ(numbers.patch, OPENFITS_VERSION_PATCH);
}

// Major, then minor, then patch, as Semantic Versioning orders versions.
TEST(version, numbers_order_as_semantic_versioning_does)
{
    using openfits::semantic_version;

    EXPECT_LT((semantic_version{.major = 0, .minor = 9, .patch = 9}), (semantic_version{.major = 1}));
    EXPECT_LT((semantic_version{.major = 1, .minor = 1}), (semantic_version{.major = 1, .minor = 2}));
    EXPECT_LT((semantic_version{.major = 1, .minor = 2, .patch = 3}),
              (semantic_version{.major = 1, .minor = 2, .patch = 4}));
    EXPECT_EQ((semantic_version{.major = 1, .minor = 2, .patch = 3}),
              (semantic_version{.major = 1, .minor = 2, .patch = 3}));
}
