# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

# Install rules and the CMake and pkg-config packages. Included by the top-level project.

include(CMakePackageConfigHelpers)

set(OPENFITS_CONFIG_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/openfits")

install(TARGETS openfits
    EXPORT openfits_targets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/openfits" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES "${PROJECT_BINARY_DIR}/include/openfits/version.h" "${PROJECT_BINARY_DIR}/include/openfits/linkage.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/openfits")

if(MSVC AND OPENFITS_SHARED)
    install(FILES "$<TARGET_PDB_FILE:openfits>" DESTINATION "${CMAKE_INSTALL_BINDIR}" OPTIONAL)
endif()

install(EXPORT openfits_targets
    NAMESPACE openfits::
    FILE openfits-targets.cmake
    DESTINATION "${OPENFITS_CONFIG_DIR}")

configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/openfits-config.cmake.in"
    "${PROJECT_BINARY_DIR}/openfits-config.cmake"
    INSTALL_DESTINATION "${OPENFITS_CONFIG_DIR}")
write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/openfits-config-version.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY ${OPENFITS_VERSION_COMPATIBILITY})
install(FILES
    "${PROJECT_BINARY_DIR}/openfits-config.cmake"
    "${PROJECT_BINARY_DIR}/openfits-config-version.cmake"
    DESTINATION "${OPENFITS_CONFIG_DIR}")

# pkg-config. The prefix is relative to the file, so the installed tree can be moved. Any absolute
# directory will do as the root of the computation. GNUInstallDirs also allows absolute directories, which
# are written as they are; when the library directory, where the file goes, is one, the prefix is that of
# the installation, which cmake --install --prefix can change after the configuration: the file gets it
# when it is installed.
if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    set(OPENFITS_PC_PREFIX "@OPENFITS_PC_INSTALL_PREFIX@")
else()
    file(RELATIVE_PATH relative_prefix
        "${PROJECT_BINARY_DIR}/prefix/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "${PROJECT_BINARY_DIR}/prefix")
    string(REGEX REPLACE "/$" "" relative_prefix "${relative_prefix}")
    set(OPENFITS_PC_PREFIX "\${pcfiledir}/${relative_prefix}")
endif()
foreach(directory LIBDIR INCLUDEDIR)
    if(IS_ABSOLUTE "${CMAKE_INSTALL_${directory}}")
        set(OPENFITS_PC_${directory} "${CMAKE_INSTALL_${directory}}")
    else()
        set(OPENFITS_PC_${directory} "\${prefix}/${CMAKE_INSTALL_${directory}}")
    endif()
endforeach()
set(OPENFITS_PC_REQUIRES_PRIVATE "")
if(NOT OPENFITS_SHARED)
    set(requires zlib)
    if(OPENFITS_WITH_TBB)
        list(APPEND requires tbb)
    endif()
    list(JOIN requires ", " requires_list)
    set(OPENFITS_PC_REQUIRES_PRIVATE "Requires.private: ${requires_list}")
endif()
# The file names the library of the configuration that is installed, whose name ends with a d in a Debug build, so it is
# generated for each configuration, which a multi-configuration generator knows only then. pkg-config has no
# configurations: installed into one prefix, they share the file, which describes the last one installed.
set(OPENFITS_PC_LIBRARY "$<TARGET_LINKER_FILE_BASE_NAME:openfits>")
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/openfits.pc.in"
    "${PROJECT_BINARY_DIR}/openfits.pc.genex"
    @ONLY)
set(pc_file "openfits$<$<CONFIG:Debug>:-debug>.pc")
file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/${pc_file}" INPUT "${PROJECT_BINARY_DIR}/openfits.pc.genex")
if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    install(CODE "
        set(OPENFITS_PC_INSTALL_PREFIX \"\${CMAKE_INSTALL_PREFIX}\")
        configure_file(\"${PROJECT_BINARY_DIR}/${pc_file}\" \"${PROJECT_BINARY_DIR}/installed/${pc_file}\" @ONLY)")
    install(FILES "${PROJECT_BINARY_DIR}/installed/${pc_file}" DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
        RENAME openfits.pc)
else()
    install(FILES "${PROJECT_BINARY_DIR}/${pc_file}" DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
        RENAME openfits.pc)
endif()

install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/NOTICE" DESTINATION "${CMAKE_INSTALL_DOCDIR}")
