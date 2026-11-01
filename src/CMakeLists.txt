# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/version.h.in"
    "${PROJECT_BINARY_DIR}/include/openfits/version.h"
    @ONLY)

# Whether the library is static, in an installed header that export.h includes: an application that takes only the
# include and library directories of the package, as MSBuild and hand-written makefiles do, needs no definition.
if(OPENFITS_SHARED)
    set(OPENFITS_STATIC_DEFINE OFF)
else()
    set(OPENFITS_STATIC_DEFINE ON)
endif()
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/linkage.h.in"
    "${PROJECT_BINARY_DIR}/include/openfits/linkage.h"
    @ONLY)

# The internal code, in namespace openfits::detail.
set(internal_sources
    core/calendar.cpp
    core/checked_math.cpp
    core/decimal.cpp
    core/diagnostic_log.cpp
    core/endian.cpp
    core/keyword_name.cpp
    core/quote.cpp
    core/utf16.cpp
    core/utf8.cpp
    core/value_text.cpp
    data/conversion.cpp
    data/image_description.cpp
    data/image_read.cpp
    data/image_write.cpp
    hdu/header_reader.cpp
    hdu/section.cpp
    hdu/structure.cpp
    hdu/walk.cpp
    header/emission.cpp
    header/keywords.cpp
    header/long_string.cpp
    header/record.cpp
    io/paths.cpp
    io/range_check.cpp
    io/thread_safe_device.cpp
    io/thread_safe_source.cpp)
if(WIN32)
    list(APPEND internal_sources io/native_file_win32.cpp)
else()
    list(APPEND internal_sources io/native_file_posix.cpp)
endif()

# The public API.
set(public_sources
    card.cpp
    date_time.cpp
    display_format.cpp
    error.cpp
    hdu.cpp
    header.cpp
    image.cpp
    reader.cpp
    types.cpp
    version.cpp
    writer.cpp
    io/callback.cpp
    io/file.cpp
    io/interfaces.cpp
    io/memory.cpp)

add_library(openfits ${public_sources} ${internal_sources})
add_library(openfits::openfits ALIAS openfits)

target_include_directories(openfits
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_features(openfits PUBLIC cxx_std_20)

set_target_properties(openfits PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    DEFINE_SYMBOL OPENFITS_EXPORTS
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${OPENFITS_SOVERSION}"
    # The Debug library has a name of its own, so that Debug and Release builds install side by side in one prefix.
    DEBUG_POSTFIX d)

# windows.h without the min and max macros, which break the standard headers included after it.
set(windows_definitions NOMINMAX WIN32_LEAN_AND_MEAN)
if(WIN32)
    target_compile_definitions(openfits PRIVATE ${windows_definitions})
endif()

# The dependencies are private: no third-party type appears in an installed header.
set(private_dependencies ZLIB::ZLIB)
if(OPENFITS_WITH_TBB)
    list(APPEND private_dependencies TBB::tbb)
endif()
target_link_libraries(openfits PRIVATE ${private_dependencies})

# The optional parts of the internal code, which the tests and fuzz targets see too.
set(internal_definitions "")
foreach(option OPENFITS_WITH_TBB)
    if(${option})
        list(APPEND internal_definitions ${option})
    endif()
endforeach()
target_compile_definitions(openfits PRIVATE ${internal_definitions})

openfits_enable_warnings(openfits)
openfits_enable_sanitizers(openfits)
if(OPENFITS_HARDENING)
    openfits_enable_hardening(openfits)
endif()

# The library and its internal code, for the tests and the fuzz targets. A shared library exports the public API only,
# so they compile the internal code themselves; it has no state, so the second copy is harmless. The internal headers
# include those of the dependencies.
add_library(openfits_internal INTERFACE)
target_include_directories(openfits_internal INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(openfits_internal INTERFACE openfits::openfits ${private_dependencies})
target_compile_definitions(openfits_internal INTERFACE ${internal_definitions})
if(OPENFITS_SHARED)
    list(TRANSFORM internal_sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
    target_sources(openfits_internal INTERFACE ${internal_sources})
    if(WIN32)
        target_compile_definitions(openfits_internal INTERFACE ${windows_definitions})
    endif()
endif()
