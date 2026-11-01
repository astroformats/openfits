# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

# The benchmarks of the library through its public API, with Google Benchmark.

find_package(benchmark CONFIG REQUIRED)

add_executable(openfits_benchmarks main.cpp images.cpp)
target_link_libraries(openfits_benchmarks PRIVATE openfits::openfits benchmark::benchmark)
# windows.h without the min and max macros, which break the standard headers included after it.
if(WIN32)
    target_compile_definitions(openfits_benchmarks PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
set_target_properties(openfits_benchmarks PROPERTIES CXX_EXTENSIONS OFF)
openfits_enable_warnings(openfits_benchmarks)
openfits_enable_sanitizers(openfits_benchmarks)
if(OPENFITS_HARDENING)
    openfits_enable_hardening(openfits_benchmarks)
endif()

# One short run of each benchmark, so that they keep working. Its numbers mean nothing; run the program itself, in a
# release build, for real ones. The benchmarks of the frame of a camera, 6000 x 4000 pixels, work on about 100 MB, so
# the smoke run leaves them out.
add_test(NAME benchmarks.smoke
    COMMAND openfits_benchmarks --benchmark_min_time=0.001s "--benchmark_filter=-/frame$")
set_tests_properties(benchmarks.smoke PROPERTIES LABELS benchmark)
