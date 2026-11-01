// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openfits/error.h>

#include <gtest/gtest.h>

#include <exception>
#include <stdexcept>
#include <system_error>
#include <type_traits>

namespace {

using openfits::errc;

// The properties every class of the hierarchy shares.
template <typename T> class error_class : public testing::Test
{};

using error_classes = testing::Types<openfits::io_error, openfits::invalid_data_error, openfits::integrity_error,
                                     openfits::unsupported_error, openfits::limit_error, openfits::validation_error,
                                     openfits::usage_error, openfits::cancelled_error>;
TYPED_TEST_SUITE(error_class, error_classes);

TYPED_TEST(error_class, is_an_openfits_error_and_a_runtime_error)
{
    static_assert(std::is_base_of_v<openfits::error, TypeParam>);
    static_assert(std::is_base_of_v<std::runtime_error, TypeParam>);
}

TYPED_TEST(error_class, copies_and_moves_without_throwing)
{
    static_assert(std::is_nothrow_copy_constructible_v<TypeParam>);
    static_assert(std::is_nothrow_move_constructible_v<TypeParam>);
    static_assert(std::is_nothrow_destructible_v<TypeParam>);
}

TYPED_TEST(error_class, is_caught_as_openfits_error_with_its_code_and_message)
{
    try {
        throw TypeParam(errc::invalid_argument, "the message");
    } catch (const openfits::error& caught) {
        EXPECT_NE(dynamic_cast<const TypeParam*>(&caught), nullptr);
        EXPECT_EQ(caught.code(), errc::invalid_argument);
        EXPECT_STREQ(caught.what(), "the message");
        EXPECT_EQ(caught.context(), openfits::error_context{});
    }
}

TEST(error, what_appends_the_context)
{
    const openfits::error_context context{.hdu = 2, .keyword = "NAXIS1", .column = 3, .offset = 5760};
    const openfits::invalid_data_error failure(errc::invalid_integer, "'x' is not an integer", context);

    EXPECT_STREQ(failure.what(), "'x' is not an integer (HDU 2, keyword NAXIS1, column 3, offset 5760)");
    EXPECT_EQ(failure.context(), context);
}

TEST(error, what_names_only_the_known_parts_of_the_context)
{
    EXPECT_STREQ(openfits::invalid_data_error(errc::invalid_integer, "message", {.offset = 0}).what(),
                 "message (offset 0)");
    EXPECT_STREQ(openfits::invalid_data_error(errc::invalid_integer, "message", {.hdu = 0}).what(), "message (HDU 0)");
    EXPECT_STREQ(openfits::invalid_data_error(errc::invalid_integer, "message", {.keyword = "EXPTIME"}).what(),
                 "message (keyword EXPTIME)");
}

// std::make_exception_ptr holds a copy, as when an error is handed to another thread.
TEST(error, copy_keeps_code_message_and_context)
{
    const openfits::limit_error original(errc::invalid_argument, "message", {.keyword = "BITPIX"});
    try {
        std::rethrow_exception(std::make_exception_ptr(original));
    } catch (const openfits::limit_error& copy) {
        EXPECT_NE(&copy, &original);
        EXPECT_EQ(copy.code(), original.code());
        EXPECT_STREQ(copy.what(), original.what());
        EXPECT_EQ(copy.context(), original.context());
    }
}

TEST(io_error, keeps_the_operating_system_error)
{
    const openfits::io_error failure(errc::invalid_argument, "message", std::make_error_code(std::errc::io_error));

    EXPECT_EQ(failure.system_code(), std::errc::io_error);
}

TEST(io_error, has_no_operating_system_error_by_default)
{
    EXPECT_FALSE(openfits::io_error(errc::invalid_argument, "message").system_code());
}

TEST(errc, zero_is_no_code)
{
    EXPECT_EQ(static_cast<int>(errc::invalid_argument), 1);
}

} // namespace
