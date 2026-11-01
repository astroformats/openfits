# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

# Overlay port that builds the checkout it lives in, so CI can prove that the library builds as a vcpkg
# port. A port for the vcpkg registry would differ only in fetching a tagged release with
# vcpkg_from_github instead.
#
# vcpkg's binary cache hashes the files of the port, not this checkout. Build it with binary caching
# disabled for the port itself (the dependencies may still come from the cache).

get_filename_component(SOURCE_PATH "${CURRENT_PORT_DIR}/../.." ABSOLUTE)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        tbb OPENFITS_WITH_TBB)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DOPENFITS_BUILD_TESTS=OFF
        -DOPENFITS_BUILD_SAMPLES=OFF
        -DOPENFITS_BUILD_FUZZERS=OFF
        -DOPENFITS_BUILD_DOCS=OFF
        -DOPENFITS_INSTALL=ON
        ${FEATURE_OPTIONS})
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME openfits CONFIG_PATH lib/cmake/openfits)
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
configure_file("${CMAKE_CURRENT_LIST_DIR}/usage" "${CURRENT_PACKAGES_DIR}/share/${PORT}/usage" COPYONLY)
