# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

cmake_minimum_required(VERSION 3.25)

project(openfits
    VERSION 0.4.0
    DESCRIPTION "C++20 library for reading and writing FITS files"
    HOMEPAGE_URL "https://github.com/astroformats/openfits"
    LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "OpenFITS supports 64-bit targets only (x86-64 and arm64), but this toolchain targets "
        "${CMAKE_SIZEOF_VOID_P}-byte pointers. Configure with a 64-bit compiler.")
endif()

# Floating-point std::to_chars needs macOS 13.3 in libc++.
if(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 13.3)
    message(FATAL_ERROR
        "OpenFITS needs a macOS deployment target of 13.3 or later, but CMAKE_OSX_DEPLOYMENT_TARGET is "
        "${CMAKE_OSX_DEPLOYMENT_TARGET}.")
endif()

# Everything except the library is off by default when OpenFITS is consumed through add_subdirectory
# or FetchContent.
option(OPENFITS_BUILD_TESTS "Build the test suites" ${PROJECT_IS_TOP_LEVEL})
option(OPENFITS_BUILD_SAMPLES "Build the sample programs" ${PROJECT_IS_TOP_LEVEL})
option(OPENFITS_BUILD_FUZZERS
    "Build the fuzz targets: libFuzzer programs when OPENFITS_SANITIZE holds 'fuzzer', programs of the engine of OPENFITS_FUZZING_ENGINE when it is set, seed replays otherwise" OFF)
option(OPENFITS_BUILD_BENCHMARKS "Build the benchmarks (Google Benchmark)" OFF)
option(OPENFITS_BUILD_DOCS "Add the docs target that builds the API documentation with Doxygen" OFF)
option(OPENFITS_INSTALL "Generate the install rules" ${PROJECT_IS_TOP_LEVEL})
option(OPENFITS_WITH_TBB "Use oneTBB for parallel work" OFF)
option(OPENFITS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(OPENFITS_HARDENING "Compile with the hardening flags of the CI and release builds" OFF)
set(OPENFITS_SANITIZE "" CACHE STRING
    "Sanitizers for the library, the tests and the fuzz targets: address;undefined, thread or fuzzer;address;undefined")

if(BUILD_SHARED_LIBS)
    set(OPENFITS_SHARED ON)
else()
    set(OPENFITS_SHARED OFF)
endif()

# During 0.x the minor version breaks the API, from 1.0 on the major version does. The ABI holds within a minor version
# only, since a minor version may add members to the option structures and functions to the sources and sinks: the
# soname is that of the minor version.
set(OPENFITS_SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
if(PROJECT_VERSION_MAJOR EQUAL 0)
    set(OPENFITS_VERSION_COMPATIBILITY SameMinorVersion)
else()
    set(OPENFITS_VERSION_COMPATIBILITY SameMajorVersion)
endif()

list(PREPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
include(openfits_warnings)
include(openfits_sanitizers)

# The programs and the DLL of a shared build go in one directory, where Windows finds the DLL: bin, unless the project
# that includes this one, or the command line, chose another.
if(NOT DEFINED CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
endif()

# Dependencies come from find_package only. Optional ones are never detected on their own. zlib is found by CMake's
# module, whatever CMAKE_FIND_PACKAGE_PREFER_CONFIG says: the package of zlib 1.3.2 names the target of a static library
# ZLIB::ZLIBSTATIC.
find_package(ZLIB MODULE REQUIRED)
if(OPENFITS_WITH_TBB)
    find_package(TBB CONFIG REQUIRED)
endif()

add_subdirectory(src)

if(OPENFITS_BUILD_TESTS OR OPENFITS_BUILD_FUZZERS OR OPENFITS_BUILD_SAMPLES OR OPENFITS_BUILD_BENCHMARKS)
    enable_testing()
endif()
if(OPENFITS_BUILD_TESTS)
    add_subdirectory(tests)
endif()
if(OPENFITS_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()
if(OPENFITS_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()
if(OPENFITS_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(OPENFITS_BUILD_DOCS)
    find_package(Doxygen REQUIRED)
    set(DOXYGEN_PROJECT_NAME OpenFITS)
    set(DOXYGEN_PROJECT_NUMBER "${PROJECT_VERSION}")
    set(DOXYGEN_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/docs")
    set(DOXYGEN_STRIP_FROM_PATH "${PROJECT_SOURCE_DIR}/include" "${PROJECT_BINARY_DIR}/include")
    set(DOXYGEN_FILE_PATTERNS "*.h")
    set(DOXYGEN_QT_AUTOBRIEF YES)
    set(DOXYGEN_PREDEFINED "OPENFITS_API=")
    # What the inline functions of public headers share is not part of the API.
    set(DOXYGEN_EXCLUDE_SYMBOLS "openfits::detail")
    set(DOXYGEN_MACRO_EXPANSION YES)
    set(DOXYGEN_EXPAND_ONLY_PREDEF YES)
    set(DOXYGEN_WARN_AS_ERROR YES)
    # Classes document their contract, and members only where a comment adds information.
    set(DOXYGEN_WARN_IF_UNDOCUMENTED NO)
    set(DOXYGEN_QUIET YES)
    doxygen_add_docs(docs
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_BINARY_DIR}/include"
        COMMENT "Generating the API documentation")
endif()

if(OPENFITS_INSTALL)
    include(openfits_install)
endif()
