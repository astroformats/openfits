// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The paths that an application gives: UTF-8, taken literally, and on Windows refused by their text when they name a
// device.

#include "io/paths.h"

#include <openfits/error.h>

#include "support/throws.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace {

using openfits::errc;
using openfits::usage_error;
using openfits::detail::check_path_argument;
using openfits::test::throws;

TEST(paths, a_path_argument_is_utf8_and_not_empty)
{
    check_path_argument("image.fits", "the path");
    check_path_argument("Nebulosa del Ñandú [Hα].fits", "the path");
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [] { check_path_argument("", "the path"); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_utf8, [] { check_path_argument("a\xC3.fits", "the path"); }));
    EXPECT_TRUE(
        throws<usage_error>(errc::invalid_utf8, [] { check_path_argument(std::string_view("a\0b", 3), "the path"); }));
    try {
        check_path_argument("", "the path of the copy");
        ADD_FAILURE() << "no exception";
    } catch (const usage_error& failure) {
        EXPECT_STREQ(failure.what(), "the path of the copy is empty");
    }
}

#if defined(_WIN32)
TEST(paths, a_path_of_the_device_namespace_names_a_device_unless_a_file_system_follows)
{
    using openfits::detail::names_a_device;
    for (const std::string_view path :
         {R"(\\.\NUL)", R"(\\.\COM1)", R"(\\.\CONIN$)", R"(\\.\pipe\name)", R"(\\?\pipe\name)", R"(\\.\mailslot\name)",
          R"(\\.\PhysicalDrive0)", R"(\\.\C:)", R"(\\?\C:)", R"(\\.\C:name)", R"(\\?\GLOBALROOT\Device\Null)",
          R"(\\?\GLOBALROOT\Device\HarddiskVolume1\image.fits)", R"(\\.\Volume{0})", R"(\\.\)", R"(\\?\)",
          "//./pipe/name"}) {
        EXPECT_TRUE(names_a_device(path)) << path;
    }
    // A drive and a separator, a network share or a volume go on with the path of a file, and the other forms of a
    // path are not of the namespace.
    for (const std::string_view path :
         {R"(\\.\C:\)", R"(\\.\C:\dir\image.fits)", R"(\\?\c:\image.fits)", R"(\\?\UNC\server\share\image.fits)",
          R"(\\.\unc\server\share\image.fits)", R"(\\?\Volume{0f6c3b71-0000-0000-0000-100000000000}\image.fits)",
          R"(C:\dir\image.fits)", R"(\\server\share\image.fits)", "C:/dir/image.fits", "//./C:/image.fits"}) {
        EXPECT_FALSE(names_a_device(path)) << path;
    }
}
#endif

} // namespace
