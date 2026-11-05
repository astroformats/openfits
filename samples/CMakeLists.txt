# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

# The sample programs. They use the public API only, like an application, and each one runs as a test with the label
# example. The arguments after the name are the arguments of its test.

function(openfits_add_sample name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE openfits::openfits)
    set_target_properties(${name} PROPERTIES CXX_EXTENSIONS OFF)
    # windows.h comes without the min and max macros, and the arguments come from CommandLineToArgvW() of shell32.
    if(WIN32)
        target_compile_definitions(${name} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(${name} PRIVATE shell32)
    endif()
    openfits_enable_warnings(${name})
    openfits_enable_sanitizers(${name})
    if(OPENFITS_HARDENING)
        openfits_enable_hardening(${name})
    endif()

    add_test(NAME example.${name} COMMAND ${name} ${ARGN})
    set_tests_properties(example.${name} PROPERTIES LABELS example)
endfunction()

openfits_add_sample(custom_io)
openfits_add_sample(read_info "${PROJECT_SOURCE_DIR}/tests/data/astropy/a13-mef.fits")
openfits_add_sample(read_pixels "${PROJECT_SOURCE_DIR}/tests/data/astropy/a11-scaled.fits")
openfits_add_sample(read_table "${PROJECT_SOURCE_DIR}/tests/data/astropy/d2-bintable.fits")
openfits_add_sample(write_image "${CMAKE_CURRENT_BINARY_DIR}/write_image.fits")
openfits_add_sample(stream_io "${CMAKE_CURRENT_BINARY_DIR}/stream_io.fits")
openfits_add_sample(convert "${PROJECT_SOURCE_DIR}/tests/data/fpack/b1-u16-rice.fits"
                    "${CMAKE_CURRENT_BINARY_DIR}/convert.fits" -32 gzip2 4)
openfits_add_sample(compress "${PROJECT_SOURCE_DIR}/tests/data/astropy/a13-mef.fits"
                    "${CMAKE_CURRENT_BINARY_DIR}/compress.fits" rice)
openfits_add_sample(copy_hdu "${CMAKE_CURRENT_BINARY_DIR}/copy_hdu.fits"
                    "${PROJECT_SOURCE_DIR}/tests/data/astropy/a13-mef.fits"
                    "${PROJECT_SOURCE_DIR}/tests/data/astropy/d2-bintable.fits" "ORIGIN='the sample copy_hdu'")
openfits_add_sample(verify "${PROJECT_SOURCE_DIR}/tests/data/astropy/d3-ascii-table.fits"
                    "${PROJECT_SOURCE_DIR}/tests/data/gzip/d6-gzip.fits.gz")

# update_keyword changes a copy of a file with checksums, made again before each run.
add_test(NAME example.update_keyword_copy
    COMMAND "${CMAKE_COMMAND}" -E copy "${PROJECT_SOURCE_DIR}/tests/data/astropy/c2-mef-checksum.fits"
            "${CMAKE_CURRENT_BINARY_DIR}/update_keyword.fits")
set_tests_properties(example.update_keyword_copy PROPERTIES LABELS example FIXTURES_SETUP update_keyword_file)
openfits_add_sample(update_keyword "${CMAKE_CURRENT_BINARY_DIR}/update_keyword.fits[SCI,2]" "FILTER='R'"
                    "EXPTIME=300.0" -GAIN)
set_tests_properties(example.update_keyword PROPERTIES
    FIXTURES_REQUIRED update_keyword_file
    PASS_REGULAR_EXPRESSION "written in place over the 2880 bytes of the header.*CHECKSUM valid, DATASUM valid")

# The files that the samples write conform to the standard: their report holds no finding, whose line would fail it.
set(writing_samples write_image stream_io convert compress copy_hdu update_keyword)
set(written_by_samples)
foreach(name IN LISTS writing_samples)
    set_tests_properties(example.${name} PROPERTIES FIXTURES_SETUP sample_files)
    list(APPEND written_by_samples "${CMAKE_CURRENT_BINARY_DIR}/${name}.fits")
endforeach()
add_test(NAME example.verify_written_files COMMAND verify ${written_by_samples})
set_tests_properties(example.verify_written_files PROPERTIES
    LABELS example
    FIXTURES_REQUIRED sample_files
    FAIL_REGULAR_EXPRESSION ": (error|warning|info): ")
