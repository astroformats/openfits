# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

find_package(GTest REQUIRED)
include(GoogleTest)

# A test executable of the given sources, built against the internal library with the warnings, sanitizers and hardening
# of the library.
function(openfits_add_test_executable name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

    target_link_libraries(${name}
        PRIVATE
            openfits_internal
            GTest::gtest
            GTest::gtest_main)

    set_target_properties(${name} PROPERTIES CXX_EXTENSIONS OFF)
    # The samples of other software, under tests/data.
    target_compile_definitions(${name} PRIVATE OPENFITS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

    # windows.h without the min and max macros, which break the standard headers included after it.
    if(WIN32)
        target_compile_definitions(${name} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()

    openfits_enable_warnings(${name})
    openfits_enable_sanitizers(${name})
    if(OPENFITS_HARDENING)
        openfits_enable_hardening(${name})
    endif()
endfunction()

openfits_add_test_executable(openfits_tests
    concurrency/io.cpp
    concurrency/pixels.cpp
    concurrency/walk.cpp
    conformance/datetime.cpp
    conformance/display_format.cpp
    conformance/file_structure.cpp
    conformance/headers.cpp
    conformance/images.cpp
    conformance/keyword_record.cpp
    conformance/tolerance.cpp
    conformance/units.cpp
    conformance/value.cpp
    samples/headers.cpp
    samples/images.cpp
    samples/sample_catalog.cpp
    support/links.cpp
    support/sparse_file.cpp
    support/temp_directory.cpp
    support/written_file.cpp
    unit/calendar.cpp
    unit/callback_io.cpp
    unit/card.cpp
    unit/card_value.cpp
    unit/checked_math.cpp
    unit/conversion.cpp
    unit/decimal.cpp
    unit/device_io.cpp
    unit/diagnostic_log.cpp
    unit/endian.cpp
    unit/error.cpp
    unit/file_io.cpp
    unit/file_specification.cpp
    unit/header.cpp
    unit/header_emission.cpp
    unit/image_info.cpp
    unit/image_read.cpp
    unit/io_interfaces.cpp
    unit/keyword_name.cpp
    unit/keywords.cpp
    unit/limits.cpp
    unit/long_string.cpp
    unit/memory_io.cpp
    unit/paths.cpp
    unit/quote.cpp
    unit/reader.cpp
    unit/record.cpp
    unit/stream_io.cpp
    unit/thread_safe_source.cpp
    unit/utf16.cpp
    unit/utf8.cpp
    unit/version.cpp
    unit/writer.cpp)

# The suites of concurrency/, conformance/ and samples/ are named after their directory, which gives them their label.
# The name of a parameterized suite follows the name of its instantiation and a slash.
gtest_discover_tests(openfits_tests
    DISCOVERY_MODE PRE_TEST
    TEST_FILTER "-concurrency_*:*/concurrency_*:conformance_*:*/conformance_*:samples_*:*/samples_*"
    PROPERTIES LABELS unit)
foreach(label concurrency conformance samples)
    gtest_discover_tests(openfits_tests
        DISCOVERY_MODE PRE_TEST
        TEST_FILTER "${label}_*:*/${label}_*"
        PROPERTIES LABELS ${label})
endforeach()

# The tests that take a gigabyte of memory also have the label slow. CTest knows them only once it has discovered them,
# and gtest_discover_tests() splits a list of labels into arguments, so a file that CTest includes after the discovered
# tests gives them both labels.
set(slow_tests_file "${CMAKE_CURRENT_BINARY_DIR}/slow_tests.cmake")
file(WRITE "${slow_tests_file}"
    "set_tests_properties(file_source.reads_more_than_1_gib_in_one_call PROPERTIES LABELS \"unit;slow\")\n")
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${slow_tests_file}")

# The installed package: install into an empty staging prefix, then configure, build and run tests/package against it.
# The five tests are a chain, which a static library follows with a test of its own; fixtures make a failed step skip
# the following ones. With a multi-configuration generator, each step takes the configuration of the CTest run (ctest
# -C). The build of the consumer is configured again over the previous one: on Windows its directory can stay in use
# for minutes after a build.
if(OPENFITS_INSTALL)
    set(package_root "${CMAKE_CURRENT_BINARY_DIR}/package")
    set(package_stage "${package_root}/stage")
    set(package_build "${package_root}/build")

    # The consumer must be built like the library: same generator, compiler, flags and dependencies, the staged package
    # first. A list value is passed whole, with $<SEMICOLON>, which add_test() would otherwise split into arguments.
    set(prefix_path "${package_stage}" ${CMAKE_PREFIX_PATH})
    list(JOIN prefix_path "$<SEMICOLON>" prefix_path)
    set(package_args "-DCMAKE_PREFIX_PATH=${prefix_path}" "-DOPENFITS_PACKAGE_VERSION=${PROJECT_VERSION}")
    foreach(variable
            CMAKE_BUILD_TYPE CMAKE_CXX_COMPILER CMAKE_CXX_FLAGS CMAKE_EXE_LINKER_FLAGS
            CMAKE_OSX_ARCHITECTURES CMAKE_OSX_DEPLOYMENT_TARGET CMAKE_MSVC_RUNTIME_LIBRARY
            CMAKE_TOOLCHAIN_FILE VCPKG_TARGET_TRIPLET VCPKG_CHAINLOAD_TOOLCHAIN_FILE VCPKG_OVERLAY_TRIPLETS)
        if(DEFINED ${variable} AND NOT "${${variable}}" STREQUAL "")
            string(REPLACE ";" "$<SEMICOLON>" value "${${variable}}")
            list(APPEND package_args "-D${variable}=${value}")
        endif()
    endforeach()
    if(CMAKE_GENERATOR_PLATFORM)
        list(APPEND package_args -A "${CMAKE_GENERATOR_PLATFORM}")
    endif()
    if(CMAKE_GENERATOR_TOOLSET)
        list(APPEND package_args -T "${CMAKE_GENERATOR_TOOLSET}")
    endif()

    # A multi-configuration consumer is generated for the configuration of the test alone, the only one installed: the
    # toolchain of vcpkg maps RelWithDebInfo and MinSizeRel to Release, which a stage of Debug lacks.
    get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(multi_config)
        list(APPEND package_args "-DCMAKE_CONFIGURATION_TYPES=$<CONFIG>")
        set(config_args --config "$<CONFIG>")
        set(consumer "${package_build}/$<CONFIG>/consumer${CMAKE_EXECUTABLE_SUFFIX}")
    else()
        set(config_args)
        set(consumer "${package_build}/consumer${CMAKE_EXECUTABLE_SUFFIX}")
    endif()

    set(package_path "${package_stage}/${CMAKE_INSTALL_BINDIR}")
    if(DEFINED VCPKG_TARGET_TRIPLET)
        # The consumer uses the packages that the library was built with, not a global vcpkg tree.
        list(APPEND package_args "-DVCPKG_MANIFEST_MODE=OFF" "-DVCPKG_INSTALLED_DIR=${VCPKG_INSTALLED_DIR}")
        # Windows finds the DLLs of a shared build through PATH; vcpkg keeps those of Debug apart.
        list(APPEND package_path "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}$<$<CONFIG:Debug>:/debug>/bin")
    endif()

    add_test(NAME package.clean
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${package_stage}")
    add_test(NAME package.install
        COMMAND "${CMAKE_COMMAND}" --install "${PROJECT_BINARY_DIR}" ${config_args} --prefix "${package_stage}")
    add_test(NAME package.configure
        COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_SOURCE_DIR}/package" -B "${package_build}"
                -G "${CMAKE_GENERATOR}" ${package_args})
    add_test(NAME package.build
        COMMAND "${CMAKE_COMMAND}" --build "${package_build}" ${config_args})
    add_test(NAME package.run
        COMMAND "${consumer}")

    set_tests_properties(package.clean PROPERTIES FIXTURES_SETUP package_cleaned)
    set_tests_properties(package.install PROPERTIES
        FIXTURES_REQUIRED package_cleaned FIXTURES_SETUP package_installed)
    set_tests_properties(package.configure PROPERTIES
        FIXTURES_REQUIRED package_installed FIXTURES_SETUP package_configured)
    set_tests_properties(package.build PROPERTIES
        FIXTURES_REQUIRED package_configured FIXTURES_SETUP package_built)
    set_tests_properties(package.run PROPERTIES FIXTURES_REQUIRED package_built)

    set_tests_properties(package.clean package.install package.configure package.build package.run
        PROPERTIES LABELS package)
    foreach(directory ${package_path})
        set_property(TEST package.run APPEND PROPERTY ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${directory}")
    endforeach()

    # A static package finds ZLIB through CMake's module, also for a consumer that prefers CMake packages
    # (CMAKE_FIND_PACKAGE_PREFER_CONFIG, which Conan sets), where the one of package/prefer_config comes first, whose
    # target has another name. Only the configuration is checked, in a build directory of its own.
    if(NOT OPENFITS_SHARED)
        set(stand_ins "${CMAKE_CURRENT_SOURCE_DIR}/package/prefer_config")
        add_test(NAME package.prefer_config
            COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_SOURCE_DIR}/package" -B "${package_root}/prefer_config"
                    -G "${CMAKE_GENERATOR}" ${package_args} -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON
                    "-DZLIB_DIR=${stand_ins}")
        set_tests_properties(package.prefer_config PROPERTIES FIXTURES_REQUIRED package_installed LABELS package)
    endif()
endif()
