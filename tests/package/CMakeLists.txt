# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

# A consumer of OpenFITS, which finds the library as OPENFITS_CONSUMED_AS says: the installed package through
# find_package (the default), the installed pkg-config file, or the sources of OPENFITS_SOURCE_DIR through FetchContent,
# as a project that is not the top level. The test suite of the main project builds it against a staged install (see
# tests/CMakeLists.txt), and CI builds it against the vcpkg port, through pkg-config and from the sources. Given
# OPENFITS_PACKAGE_VERSION, the version of the package, it also checks the versions that the package accepts.

cmake_minimum_required(VERSION 3.25)

project(openfits_package_test LANGUAGES CXX)

set(OPENFITS_CONSUMED_AS "package" CACHE STRING "How the consumer finds OpenFITS: package, pkg-config or sources")
set(OPENFITS_SOURCE_DIR "" CACHE PATH "The sources of OpenFITS, for OPENFITS_CONSUMED_AS=sources")
set(OPENFITS_PACKAGE_VERSION "" CACHE STRING "The version of the package, for OPENFITS_CONSUMED_AS=package")

add_executable(consumer main.cpp)

if(OPENFITS_CONSUMED_AS STREQUAL "package" AND OPENFITS_PACKAGE_VERSION)
    # During 0.x, a package of version 0.m.p is found for a request of 0.m, and not for one of another minor version or
    # of 1.0 (SameMinorVersion). From 1.0 on, a package of version M.m.p is found for a request of M.0 to M.m, and not
    # for one of a later minor version or of an earlier major version (SameMajorVersion).
    string(REGEX MATCH "^([0-9]+)\\.([0-9]+)" major_minor "${OPENFITS_PACKAGE_VERSION}")
    set(major "${CMAKE_MATCH_1}")
    set(minor "${CMAKE_MATCH_2}")
    math(EXPR later_minor "${minor} + 1")
    if(major EQUAL 0)
        set(oldest_accepted "${major_minor}")
        set(refused 0.${later_minor} 1.0)
    else()
        math(EXPR earlier_major "${major} - 1")
        set(oldest_accepted ${major}.0)
        set(refused ${major}.${later_minor} ${earlier_major}.0)
    endif()
    find_package(openfits ${oldest_accepted} CONFIG REQUIRED)
    set(version "${openfits_VERSION}")
    foreach(request ${refused})
        find_package(openfits ${request} CONFIG QUIET)
        if(openfits_FOUND)
            message(FATAL_ERROR "openfits ${version} was found for version ${request}")
        endif()
    endforeach()
    find_package(openfits ${major_minor} CONFIG REQUIRED)
    target_link_libraries(consumer PRIVATE openfits::openfits)
elseif(OPENFITS_CONSUMED_AS STREQUAL "package")
    find_package(openfits CONFIG REQUIRED)
    target_link_libraries(consumer PRIVATE openfits::openfits)
    set(version "${openfits_VERSION}")
elseif(OPENFITS_CONSUMED_AS STREQUAL "pkg-config")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(openfits REQUIRED openfits)
    # The static flags add the libraries of the private requirements, which a static library needs and a shared one
    # does not list. Libraries are linked after the objects that need them.
    target_compile_options(consumer PRIVATE ${openfits_STATIC_CFLAGS})
    target_link_libraries(consumer PRIVATE ${openfits_STATIC_LDFLAGS})
    target_compile_features(consumer PRIVATE cxx_std_20)
    set(version "${openfits_VERSION}")
elseif(OPENFITS_CONSUMED_AS STREQUAL "sources")
    include(FetchContent)
    FetchContent_Declare(openfits SOURCE_DIR "${OPENFITS_SOURCE_DIR}")
    # The project that includes OpenFITS says where the programs and DLLs go: here, where the consumer is, which was
    # made before.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
    FetchContent_MakeAvailable(openfits)
    # A project that is not the top level gets the library alone, which goes where that project says.
    foreach(option OPENFITS_BUILD_TESTS OPENFITS_BUILD_SAMPLES OPENFITS_BUILD_DOCS OPENFITS_INSTALL)
        if(${option})
            message(FATAL_ERROR "${option} is on, in a project that is not the top level")
        endif()
    endforeach()
    get_target_property(directory openfits RUNTIME_OUTPUT_DIRECTORY)
    if(NOT directory STREQUAL CMAKE_RUNTIME_OUTPUT_DIRECTORY)
        message(FATAL_ERROR "The library goes to ${directory}, not to ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
    endif()
    target_link_libraries(consumer PRIVATE openfits::openfits)
    get_target_property(version openfits VERSION)
else()
    message(FATAL_ERROR "OPENFITS_CONSUMED_AS is ${OPENFITS_CONSUMED_AS}, not package, pkg-config or sources")
endif()

target_compile_definitions(consumer PRIVATE EXPECTED_VERSION="${version}")
