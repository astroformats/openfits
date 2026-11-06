#!/bin/bash -eu
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

# The build script of OpenFITS for OSS-Fuzz. It runs in the image of the openfits project of the OSS-Fuzz repository,
# whose Dockerfile clones this repository and the sources of zlib into $SRC, with the compiler, the flags and the
# fuzzing engine of the build in $CC, $CXX, $CFLAGS, $CXXFLAGS and $LIB_FUZZING_ENGINE. zlib is built from its sources
# with the same flags, so that every sanitizer sees it, and linked statically. Each fuzz target goes to $OUT with its
# seed corpus. No CMake project gets a build type, whose flags would come after those of $CXXFLAGS, which OSS-Fuzz
# chooses for each sanitizer.

prefix="$WORK/prefix"
mkdir -p "$prefix"
jobs="$(nproc)"

# zlib
(cd "$SRC/zlib" && ./configure --static --prefix="$prefix" && make -j"$jobs" install)

# OpenFITS and its fuzz targets. CMake takes the compilers and the flags from the environment.
cmake -S "$SRC/openfits" -B "$WORK/openfits" -DCMAKE_PREFIX_PATH="$prefix" \
    -DOPENFITS_BUILD_TESTS=OFF -DOPENFITS_BUILD_SAMPLES=OFF -DOPENFITS_INSTALL=OFF -DOPENFITS_BUILD_FUZZERS=ON \
    "-DOPENFITS_FUZZING_ENGINE=$LIB_FUZZING_ENGINE"
cmake --build "$WORK/openfits" -j"$jobs"

for target in card_grammar checksum editor gzip_file gzip_tile hcompress hdu_walk header image plio quantize rice \
    roundtrip table tiled_table tiles; do
    cp "$WORK/openfits/bin/fuzz_$target" "$OUT/"
    (cd "$SRC/openfits/fuzz/seeds/$target" && zip -q -r "$OUT/fuzz_${target}_seed_corpus.zip" .)
done
