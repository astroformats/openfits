# OpenFITS

OpenFITS is a C++20 library for reading, writing and editing FITS, the Flexible Image Transport System, the standard
file format of astronomy. It is written from the standard of the IAU FITS Working Group and aims at complete
conformance with [FITS 4.0](https://fits.gsfc.nasa.gov/fits_standard.html): every structure the standard defines is
read, everything but the deprecated random groups is written, existing files are edited in place, and the conventions
in common use beyond the standard, such as gzip-compressed files and `HIERARCH` keywords, are supported; with an API
that does not depend on any application, so that it can replace CFITSIO in C++ programs. It is the second library of
[AstroFormats](https://github.com/astroformats), after [OpenXISF](https://github.com/astroformats/openxisf), and
shares its design.

**Status: 0.5.0.** An early release, which reads, writes and edits headers, images, tables, checksums and tiled
compression: the primary array and `IMAGE` extensions with every `BITPIX`, their values converted into any numeric type
with the scaling, undefined values and unsigned conventions of the standard, read whole or as regions with steps, into
memory of the library or of the application; binary and ASCII tables, every column type and variable-length arrays
among them; random groups, read; `DATASUM` and `CHECKSUM`, verified as data are read and written on request; and images
and tables compressed in tiles with every algorithm of the standard, quantization and dithering among them. The writer
checks every model before it writes, streams the values of images and the rows of tables to the file as they are
produced, reserves the data of images for later filling, and copies the units of other files byte for byte. Files
compressed whole with gzip are read and written, and the conformance of a file to the standard is reported in a call,
each departure with the section of the standard that it departs from. Existing files are edited: keywords, values and
rows changed in place where the layout of the file allows it, and units appended, inserted, replaced and erased, their
checksums kept up to date from the bytes that change. The API may change before 1.0. The ABI holds within a minor
version: the shared library of each minor version has a soname of its own (`libopenfits.so.0.5` for 0.5.x), and from
0.2 on a DLL name of its own on Windows (`openfits-0.5.dll`, `libopenfits-0.5.dll` with MinGW, where 0.1 has
`openfits.dll` and `libopenfits.dll`), so a program built against 0.4 is rebuilt to use 0.5. The import library keeps
its name (`openfits.lib`, `libopenfits.dll.a`), so a program links as before. Files are read from, written to and
edited through sources, sinks and devices of the library or of the application; their header and data units are walked
as they are asked for, and their headers read as typed values and changed.

## Features and conformance

| Area | Reading | Writing | Editing |
|---|---|---|---|
| Primary array and `IMAGE` extensions, any number of header and data units, every `BITPIX` | Yes | Yes | Yes: units appended, inserted, replaced and erased |
| Header cards: fixed and free format, strings, logicals, integers, reals, complex and undefined values, commentary, units, display formats, reserved header space | Yes | Yes | Yes: in place within the blocks of the header, its blank cards taken first |
| Long strings (`CONTINUE`), `HIERARCH` keywords, `INHERIT` and `LONGSTRN` as keywords | Yes | Yes | Yes |
| Pixel data: big-endian to native order, `BSCALE`, `BZERO` and `BLANK`, the unsigned conventions, conversion to any numeric type, regions with steps | Yes | Yes: whole images, from memory or streamed, and data reserved for regions | Yes: regions with steps and a pitch, in place, undefined values and raw bytes |
| Binary tables (`BINTABLE`): every column type, repeat counts of 0, dimensions, substrings, scaling, the unsigned conventions, nulls, display formats, variable-length arrays with `P` and `Q` descriptors | Yes | Yes | Yes: fields in place, but those of variable-length arrays, and rows appended |
| ASCII tables (`TABLE`): `A`, `I`, `F`, `E` and `D` fields, null strings, blank fields, implied decimal points | Yes | Yes | Yes: fields in place and rows appended |
| Random groups: parameters, by index and by name, and the array of each group | Yes | Not planned (deprecated by the standard) | Keywords and raw bytes |
| Checksums (`CHECKSUM` and `DATASUM`): verified in reads of whole images, on request and at a strict open, the header part as units are walked; written on request | Yes | Yes | Yes: updated from the bytes that change (standard Appendix J.4) |
| Tiled image compression: `RICE_1`, `GZIP_1`, `GZIP_2`, `PLIO_1`, `HCOMPRESS_1` and `NOCOMPRESS`, with quantization and dithering, null pixels and a cache of decoded tiles | Yes | Yes | Yes: keywords through the `Z` keywords, regions re-encode the tiles they meet |
| Tiled table compression: `RICE_1`, `GZIP_1`, `GZIP_2` and `NOCOMPRESS` columns, variable-length arrays among them | Yes | Yes | Keywords; cells through a replaced unit |
| gzip-compressed files (`.fits.gz`): recognized by their first bytes, every member inflated, into memory or a temporary file | Yes | Yes, on request | No: written again whole |
| Extensions of unknown type, special records | Yes (raw) | Yes: copied unchanged | Yes: kept unchanged, raw bytes |
| File specifications `file.fits[2]`, `file.fits[SCI,1]`, `file.fits[1][*,1:100]`, parsed when asked for | Yes | Not applicable | Yes: unit selection |
| WCS and time keywords, read and written as keywords, not evaluated | Yes | Yes | Yes |
| Verification of a file against the standard, headers and data, each departure reported with its section | Yes | Not applicable | Not applicable |

Editing means changing an existing file: keywords, data values and rows in place where the layout of the file allows
it, units added and removed, and a safe rewrite for every change of layout. Not planned: evaluating world coordinates
(the work of a coordinate library), the filtering and calculator syntax of CFITSIO's extended file names, the
hierarchical grouping convention, and writing random groups. The table is updated at each release.

### Tiled compression

Compressed images and tables are read as the files of `fpack` and of the other writers have them, their older markings
included (`ZCMPTYPE = 'RICE_ONE'`, `ZQUANTIZ = 'NONE'`, `ZSCALE` and `ZZERO` as keywords). The writer compresses with
the parameters of `fpack`, and its `RICE_1`, `PLIO_1`, `HCOMPRESS_1` and quantized tiles are the bytes that CFITSIO
writes for the same parameters, but where CFITSIO's would not restore the image: a `PLIO_1` run of zeros of a multiple
of 4095 pixels before a single pixel, a lossy `HCOMPRESS_1` tile that holds undefined pixels, which the writer
compresses losslessly, and a floating-point tile whose quantization would restore infinities, which it keeps as its
values. A lossy `HCOMPRESS_1` tile restores values a little beyond those it compressed, which a read takes to the
nearest value of the type of the image. A floating-point image compressed without a quantization level is lossless: its tiles
hold its values with `GZIP_2` and no `ZQUANTIZ`, the form of standard §10.2.1, which CFITSIO, `funpack`, `fitsverify`
and astropy read. The writer writes `NOCOMPRESS` for any `BITPIX`, and CFITSIO for 16-bit and 32-bit integers and
32-bit floats only.

Other readers read what the writer writes, with these exceptions, each a limit of the reader, of CFITSIO 4.7.0 (and
its `funpack`) and astropy 8.0.1:
- CFITSIO decompresses no image of 64-bit integers, which `fpack` does not compress either; the writer compresses them
  with `GZIP_2` by default.
- `RICE_1` of 64-bit integers (`BYTEPIX` 8, standard Table 37), which the writer writes only on request, has no decoder
  in CFITSIO and astropy; astropy also fails on `NOCOMPRESS` images of 64-bit integers.
- CFITSIO refuses a table column whose `ZCTYPn` is `NOCOMPRESS`, which standard §10.3.5 allows and the writer writes
  only on request, and it crashes on a compressed table with a column of variable-length arrays of characters or bits
  (`PA`, `PX`), the files of `fpack` included.

## Requirements and platforms

- C++20 and CMake 3.25 or newer. 64-bit targets only (x86-64 and arm64); configuring for a 32-bit toolchain stops with
  an error.
- Compilers, each tested by CI at the oldest version given: MSVC from Visual Studio 2022, and clang-cl from Visual
  Studio 2026; on Linux, GCC 11, and Clang 15 with libstdc++ or Clang 18 with libc++; the current GCC and Clang of
  MSYS2; AppleClang from Xcode 16, with a macOS deployment target of 13.3 or later.
- Platforms: Windows (MSVC, clang-cl, MSYS2 UCRT64 and CLANG64), Linux (GCC and Clang, with libstdc++ or libc++) and
  macOS.
- Dependencies: zlib. oneTBB is optional. GoogleTest is needed for the tests, and Google Benchmark for the benchmarks.

## Building

The build is described by CMake presets: `msvc`, `clang-cl`, `msys2-ucrt64-gcc`, `msys2-clang64`, `linux-gcc`,
`linux-clang`, `linux-clang-libcxx` and `macos`, each with a `-debug` and a `-release` variant. The presets that do not
fit your host are hidden by `cmake --list-presets`.

```
cmake --preset linux-gcc-release
cmake --build --preset linux-gcc-release
ctest --preset linux-gcc-release
```

Binaries go to `out/build/<preset>`. Personal settings belong in a `CMakeUserPresets.json`, which git ignores.

The `package` tests install the library into an empty staging prefix, then configure, build and run a consumer against
it, in the configuration of the run with a multi-configuration generator (`ctest -C Debug`), and check the versions that
the package accepts.

Other presets run the tests under analysis tools: `linux-gcc-asan`, `linux-clang-asan` and `msvc-asan` (AddressSanitizer;
the Linux ones add UndefinedBehaviorSanitizer), `linux-clang-tsan` and `linux-clang-tsan-tbb` (ThreadSanitizer, the
second with oneTBB) and `linux-gcc-coverage`.

### Dependencies

- **vcpkg, manifest mode.** Set `VCPKG_ROOT` to a vcpkg checkout. The `msvc`, `clang-cl`, `linux-clang-libcxx` and
  `linux-clang-tsan-tbb` presets use it automatically, and `vcpkg.json` pins the versions with a baseline.
  `linux-clang-libcxx` builds the dependencies against libc++ too, and `linux-clang-tsan-tbb` with ThreadSanitizer, with
  the triplets of `cmake/vcpkg`: they need `CC` and `CXX` set to Clang, and the first libc++. With the other presets add `-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake`.
  Optional dependencies are features: `-DVCPKG_MANIFEST_FEATURES="tests;tbb;benchmarks"`.
- **Linux.** `sudo apt install pkg-config zlib1g-dev libgtest-dev`, `libtbb-dev` for oneTBB and `libbenchmark-dev` for
  the benchmarks.
- **MSYS2.** In the UCRT64 or CLANG64 shell, install `cmake`, `ninja`, `pkgconf`, `zlib` and `gtest` with the matching
  `mingw-w64-<environment>-` prefix, plus the compiler, and `tbb` and `benchmark` for the options that use them.
- **macOS.** `brew install pkgconf googletest`, and `tbb` and `google-benchmark` for the options that use them, then
  configure with `-DCMAKE_PREFIX_PATH="$(brew --prefix)"`. zlib comes with the SDK.

The Visual Studio presets use Ninja, which needs the compiler environment: run them from a Developer PowerShell
(or any shell where `cl` works). The MSYS2 presets run from the matching MSYS2 shell.

### Options

| Option | Default | Effect |
|---|---|---|
| `BUILD_SHARED_LIBS` | `OFF` | Build a shared library instead of a static one |
| `OPENFITS_BUILD_TESTS` | `ON` at the top level | Build the test suites |
| `OPENFITS_BUILD_SAMPLES` | `ON` at the top level | Build the sample programs of `samples/`, which run as tests with the label `example` |
| `OPENFITS_BUILD_FUZZERS` | `OFF` | Build the fuzz targets: libFuzzer programs when `OPENFITS_SANITIZE` contains `fuzzer`, seed replays otherwise (the presets turn it on) |
| `OPENFITS_FUZZ_SECONDS` | `60` | How long the test of each fuzz target runs with libFuzzer |
| `OPENFITS_FUZZING_ENGINE` | empty | Link the fuzz targets with this fuzzing engine (a flag such as `-fsanitize=fuzzer`, or a library) instead, with the instrumentation of `CMAKE_CXX_FLAGS`, as OSS-Fuzz builds them; no test runs them then |
| `OPENFITS_BUILD_BENCHMARKS` | `OFF` | Build the benchmarks of `benchmarks/` with Google Benchmark (the `benchmarks` feature of the vcpkg manifest), and a test that runs each of them briefly |
| `OPENFITS_BUILD_DOCS` | `OFF` | Add the `docs` target, which builds the API documentation with Doxygen |
| `OPENFITS_INSTALL` | `ON` at the top level | Generate the install rules |
| `OPENFITS_WITH_TBB` | `OFF` | Use oneTBB for parallel work |
| `OPENFITS_WARNINGS_AS_ERRORS` | `OFF` | Treat compiler warnings as errors (CI turns it on) |
| `OPENFITS_HARDENING` | `OFF` | Compile with hardening flags (the presets turn it on) |
| `OPENFITS_SANITIZE` | empty | Sanitizers for the library, the tests and the fuzz targets: `address;undefined`, `thread` or `fuzzer;address;undefined` (Clang), or `address` (MSVC) |

Optional dependencies are never detected on their own. Everything except the library is off when OpenFITS is added
with `add_subdirectory` or `FetchContent`. The programs, and the DLL of a shared build, go to `bin` of the build
directory, unless `CMAKE_RUNTIME_OUTPUT_DIRECTORY` says otherwise, as the project that adds OpenFITS may.

## Using it from CMake

```cmake
find_package(openfits CONFIG REQUIRED)
target_link_libraries(app PRIVATE openfits::openfits)
```

The installed package can be moved, unless it is installed with an absolute library directory (`CMAKE_INSTALL_LIBDIR`):
it then names the prefix of the configuration, which `cmake --install --prefix` cannot change. The library of a Debug
build has a `d` at the end of its name, so that Debug and Release builds can be installed into one prefix. A pkg-config
file, `openfits.pc`, is installed as well, for the configuration installed last. OpenFITS is also set up to become a
vcpkg port.

The installed headers record whether the library is static or shared (`openfits/linkage.h`), so a build system that
is given only the include and library directories, as a Visual Studio project or a makefile is, needs no definition to
link it; a static library also needs the libraries of its dependencies, which the CMake package and
`pkg-config --static` list. A prefix holds one of the two.

## Examples

Files are read from an `openfits::input_source`, written to an `openfits::output_sink`, and edited through an
`openfits::random_access_device`. The library provides them for files (paths are UTF-8 on every platform, and a file is
replaced only when it has been written completely), for memory, for functions of the application, and, in
`<openfits/stream_io.h>`, for C and C++ streams, with `read_into_memory()` for a stream that cannot seek, such as a pipe.
An application can also implement them over its own storage; this is an excerpt of `samples/custom_io.cpp`, a source
over the function table of a storage library written in C:

```cpp
#include <openfits/openfits.h>

class blob_source final : public openfits::input_source
{
public:
    blob_source(const blob_functions& functions, const void* blob) : functions_(functions), blob_(blob) {}

    std::uint64_t size() const override
    {
        return functions_.size(blob_);
    }

    void read(std::uint64_t offset, std::span<std::byte> destination) const override
    {
        if (functions_.read_at(blob_, offset, destination.data(), destination.size()) != 0) {
            throw openfits::io_error(openfits::errc::read_failed, "cannot read the blob", {}, {.offset = offset});
        }
    }

private:
    blob_functions functions_;
    const void* blob_;
};
```

A source reads exactly the bytes it is asked for, at the offset it is given, or throws. The library never asks for bytes
beyond its size, and calls it from one thread at a time unless it declares concurrent reads.

A `reader` opens a file from a path, a source or a file specification, and walks its header and data units as they are
asked for: the open reads the primary header, and no data. Each unit has its header, the offsets of its header and its
data, and the departures from the standard found in its header, which the reader reads as intended and reports as
diagnostics. This excerpt of `samples/read_info.cpp` lists the units of a file and reads typed values from their
headers:

```cpp
const openfits::reader file(path);  // A path is always the name of a file, brackets included.
for (const openfits::hdu_info& unit : file.hdus()) {
    std::cout << "HDU " << unit.index() << ": " << unit.header().size() << " cards\n";

    // find() gives nothing for a keyword that the header lacks, value() throws; both convert the value to the type
    // asked for, a string that holds a number included, and refuse what would lose information.
    const openfits::header& cards = unit.header();
    if (const auto naxis = cards.find<std::uint64_t>("NAXIS"); naxis && *naxis > 0 && *naxis <= 999) {
        for (const std::uint64_t length : cards.values<std::uint64_t>("NAXIS", 1, *naxis)) {
            std::cout << ' ' << length;
        }
    }
    if (const std::optional<std::string> object = cards.find<std::string>("OBJECT")) {
        std::cout << "  object: " << *object << '\n';
    }
    for (const openfits::diagnostic& found : unit.diagnostics()) {
        std::cout << "  " << found.message << '\n';
    }
}
```

`openfits::file_specification::parse("image.fits[SCI,2][1:100,1:100]")` reads the extended file names of CFITSIO,
whose unit and image section a reader then selects; `adjust_for_section()` gives the header of the section.

`file.image(index)` describes the image of a unit: its `BITPIX`, its axes in the order of the file, the first varying
fastest, its scaling (`BSCALE` and `BZERO`), its `BLANK`, and `value_type()`, the type that holds its values without a
loss: `std::uint16_t` for 16-bit data with `BZERO = 32768`, the unsigned convention of the standard, and `float` for
16-bit data scaled by a fraction. The values are read into memory of the library or of the application, as any of the
ten numeric types, and are the physical values, scaled, unless the caller asks otherwise. These excerpts of
`samples/read_pixels.cpp` read an image as `float` into a buffer of the application while a progress function, which
receives the bytes read and their total, reports how far the read is and can cancel it by returning false; then a
region of the image whose second axis has a step of -1, so that its rows come upside down, into the middle of a larger
buffer, its rows a pitch apart:

```cpp
const openfits::image_info image = file.image(index);
std::vector<float> values(image.geometry.element_count());
(void)file.read_pixels(index, std::span<float>(values), {.progress = progress});
```

```cpp
(void)file.read_pixels(index, middle.first(((height - 1) * pitch) + width), {.region = box, .pitch = pitch});
```

A read into an integer type truncates toward zero, or rounds half away from zero with `rounding::nearest`, and refuses
a value beyond the range of the type, or clamps and counts it with `overflow_policy::clamp`. Undefined values, `BLANK`
and NaN, become NaN in floating-point types, a value of the caller's, or a mask beside the values (`blank_policy`);
`scaling_policy::raw` reads the stored values themselves. Floating-point data read as their own type are copied bit for
bit, NaN payloads and signed zeros included. A region costs reads in proportion to its bytes rather than to its rows:
runs close together are read in one request, and a source in memory is read in place.

A `writer` holds the units of a file until it saves them: for an image, its description, with the keywords of the
application, and its values, which the writer borrows and never modifies. The library writes the structural keywords
itself, from the description and from the type of the values: `std::uint16_t` values are stored as 16-bit data with
`BZERO = 32768`. These excerpts of `samples/write_image.cpp` write a frame with keywords, then the same frame as
physical values in floating point, scaled into 16-bit integers with NaN stored as `BLANK`:

```cpp
openfits::image_info image;
image.geometry.axes = {width, height};
image.header.set("OBJECT", "NGC 7000", "the target");
image.header.set("EXPTIME", 300.0, "[s] exposure time");
openfits::writer output;
(void)output.add_image(image, std::span<const std::uint16_t>(values));
```

```cpp
openfits::image_info scaled;
scaled.geometry.axes = {width, height};
scaled.bitpix = openfits::bitpix::int16;
scaled.scaling = {.scale = 0.01, .zero = 327.68};
scaled.blank = -32768;
(void)output.add_image(scaled, std::span<const float>(physical));
output.save(path, {.checksums = true, .checksum_time = openfits::to_date_time(observed + std::chrono::hours{2})});
```

A write into integer data rounds half away from zero, and refuses a value beyond the range of the stored type, or clamps
it on request; with `scaling_policy::raw` in its options, an image of stored values, such as a frame read raw, is
written as it is, under the `BSCALE` and `BZERO` of its description, and with `scaling_policy::override` its values
convert into stored ones by a scaling of the caller's instead: codes of `std::uint16_t` with the offset of their type,
under a scaling of the application's, for instance. `save()` replaces a file only once the new one is
complete, or refuses to replace one (`if_exists::fail`), and writes into any sink as well: `samples/stream_io.cpp`
writes through a `std::ofstream`, and reads the file back through a `std::ifstream` and a `FILE*`. A file that exists
is changed through an editor, described below.

`save()` checks the whole model before it writes a byte, and throws `validation_error`, naming the unit and the keyword,
for what the standard forbids: a keyword name, a value or a comment that no record holds, a structural keyword in a
header, a `BLANK` of floating-point data, a `TNULLn` of a column of reals, a compression that the data cannot take. A
card can carry the text of its value, `card::with_value_text("EXPTIME", "3.0E+02")`, which the save writes as it is,
under the same options as the cards it formats. Values too large for memory are streamed: the save asks a function of
the application for them, chunk after chunk of whole rows, as it writes them; a table's rows are streamed alike, and its
`NAXIS2`, its heap and its checksums completed once they are written. This excerpt of `samples/write_image.cpp` writes
a frame whose values are computed as the save asks for them:

```cpp
(void)output.add_image<std::uint16_t>(sky, [](std::uint64_t first, std::span<std::uint16_t> rows) {
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const std::uint64_t x = (first + i) % width;
        const std::uint64_t y = (first + i) / width;
        rows[i] = static_cast<std::uint16_t>(1000 + (2 * x) + y);
    }
});
```

`add_reserved_image()` writes an image whose data are zeros, or undefined values, for an editor to fill region by
region later, the way a stacked frame or a mosaic is written without holding it whole; a file sink makes the file
longer for its zeros rather than writing them. `add_hdu()` copies a unit of another file as it is, byte for byte, its
checksums kept, compressed units and extensions of unknown types included, or with its header edited on the way; a
`without_structural()` of a header, an image or a table read from a file gives the writer its model back in one line.
This excerpt of `samples/copy_hdu.cpp` appends the units of one file to those of another, with a `HISTORY` card in each
copied unit and a keyword given as the text of its value:

```cpp
openfits::header cards = openfits::without_structural(unit.header());
cards.add_history("Copied from unit " + std::to_string(i) + " of " + arguments[2]);
if (!keyword.empty()) {
    // The card takes the place of the first card of the keyword, or comes after the others.
    openfits::card edited = openfits::card::with_value_text(keyword, value_text);
    if (const std::optional<std::size_t> position = cards.index_of(keyword)) {
        (void)cards.erase(keyword);
        cards.insert(*position, std::move(edited));
    } else {
        cards.append(std::move(edited));
    }
}
(void)output.add_hdu(second, i, {.header = cards});
```

With `checksums` in its options, `save()` gives every unit `DATASUM`, the sum of its data, and `CHECKSUM`, which makes the
sum of the whole unit negative zero (standard §4.4.2.7), their comment the time of the application, since the library
reads no clock. A read of a whole image verifies the `DATASUM` of its unit in the same pass as the read, and throws
`integrity_error` once the values are in the destination when the data do not match it (`verify_checksum = false` reads
a damaged unit for what it holds); the header part of `CHECKSUM` is checked as each unit is walked, a mismatch being a
warning among its diagnostics, and a strict open verifies every unit. `verify()` reads a whole unit of any kind and
tells, for each sum, whether it is valid, invalid or absent, a blank or undefined value asserting nothing; like a read,
it takes a progress function that can cancel it:

```cpp
const openfits::checksum_report sums = file.verify(unit.index());
std::cout << "  DATASUM " << state_name(sums.datasum) << ", CHECKSUM " << state_name(sums.checksum) << '\n';
```

An image or a table is compressed in tiles (standard §10) when its options, or those of `save()` for the units that
give none, carry `compression_options`: the algorithm, the shape of the tiles, and for floating-point images the level
of the quantization and its dithering. The tiles are compressed in parallel in a build with oneTBB. A compressed image
is read as any other, the tiles that a read meets decompressed, in parallel too; a `tile_cache` of the caller's, which
threads can share, keeps decoded tiles for reads row by row. A compressed primary array is unit 1 after an empty
primary, as the standard writes it. This excerpt of `samples/write_image.cpp` writes the frame again, compressed with
`RICE_1` in tiles of 16 rows:

```cpp
openfits::image_info packed = image;
packed.header.set("EXTNAME", "RICE");
(void)output.add_image(packed, std::span<const std::uint16_t>(values),
                       {.compression = openfits::compression_options{.type = openfits::compression::rice,
                                                                     .tile = {width, 16}}});
```

```cpp
openfits::tile_cache cache(std::uint64_t{16} << 20);  // 16 MiB of decoded tiles.
for (std::uint64_t y = 0; y < height; ++y) {
    (void)file.read_pixels(unit, std::span<std::uint16_t>(row),
                           {.region = openfits::region{.start = {0, y}, .size = {width, 1}}, .cache = &cache});
}
```

`samples/convert.cpp` reads an image of any `BITPIX`, compressed or not, and writes it as another, compressed or not;
`samples/compress.cpp` compresses the images of a file as `fpack` does, on every core. On one core, its work on a
camera frame of 6000 x 4000 pixels takes three quarters of the time of `fpack` 1.7.0 for 16-bit values with `RICE_1`,
and three fifths for 32-bit floating-point values quantized at `q = 4` (an AMD Ryzen 9 9950X under Windows 11, both
built by MSVC); with oneTBB, the tiles of each image are compressed on every core.

A file compressed whole with gzip, a `.fits.gz` file, a convention outside the standard, is read as the file it holds:
a reader recognizes it by its first bytes, whatever its name, and inflates it once, every member of it as gzip(1)
does, each checked against its CRC-32, into memory within `limits::max_inflated_bytes`, or, larger than
`read_options::inflate.memory_threshold`, into a temporary file in a directory that the application gives there, which
is removed with the reader; the library takes no directory from the environment. `gzip_source` inflates any source
alike. A save compresses the file it writes at the level of zlib that `write_options::gzip_level` gives, and
`gzip_sink` compresses what is written into any sink; a gzip sink cannot go back, so the checksums of values in memory
are summed before the file is written, and those of streamed values are refused.

```cpp
output.save("NGC 7000.fits.gz", {.gzip_level = 1});
const openfits::reader file("NGC 7000.fits.gz", {.inflate = {.temporary_directory = scratch}});
```

`file.table(index)` describes a table, ASCII or binary: its rows and columns, the format, dimensions, scaling, null and
unit of each, and the heap of variable-length arrays. A column is read over a range of rows as any numeric type, scaled,
its undefined values given as the policy of the read says, or as strings through its display format; several columns
are read in one pass over the rows, and the arrays of a column of variable-length arrays are read with their
descriptors sorted, so that the heap is read in ranges. This excerpt of `samples/read_table.cpp` reads a column of
numbers as `double`, its undefined values marked in a mask, then the arrays of a column of variable-length arrays:

```cpp
std::vector<double> values(rows);
std::vector<std::uint8_t> undefined(rows);
std::vector<openfits::column_destination> destinations{
    openfits::column_destination::of(index, std::span<double>(values), std::span<std::uint8_t>(undefined))};
(void)file.read_columns(unit, destinations, {.blank = openfits::blank_policy::mask});
```

```cpp
const openfits::column_arrays<double> arrays = file.read_arrays<double>(unit, index);
std::cout << "column " << index << ": " << arrays.size() << " arrays, " << arrays.values.size()
          << " elements in all";
```

A table is written from a description and the values of each column, which the writer borrows: a column without a
format takes it from the type of its values, `std::uint16_t` as 16-bit integers with `TZEROn = 32768`, strings as
characters of the longest of them, and arrays of different lengths as variable-length arrays in a heap that the writer
builds; an ASCII table takes fields wide enough for the values of each type.

```cpp
const std::vector<std::string> names{"M 31", "M 33", "NGC 7000"};
const std::vector<double> fluxes{3.44, 5.72, 4.0};
openfits::table_info table{.kind = openfits::hdu_kind::bintable, .rows = 3};
table.columns = {{.name = "NAME"}, {.name = "FLUX", .unit = "Jy"}};
openfits::writer output;
(void)output.add_table(table, std::vector<openfits::column_values>{std::span(names), std::span(fluxes)});
output.save(path);
```

`file.groups(0)` describes random groups (standard §6), whose parameters `read_parameter<T>()` reads by index, or by
name, the parameters of one name summed as the standard says, and whose arrays `read_group<T>()` reads as an image.

`openfits::verify()` gives the conformance report of a file, which no other FITS library gives in a call: each
departure from the standard that the library finds, with its severity, its unit, its keyword or column, and the
section of the standard that it departs from. It holds what a strict reading finds in the headers, every departure
that the reader reads through as intended among them, and then checks what reads do not: the fill of every data unit,
every field of every ASCII table, the logicals and characters of binary tables, every descriptor of variable-length
arrays against the heap, every checksum, and units of one name and version. A problem that repeats in a column is
listed ten times and counted after. This excerpt of `samples/verify.cpp` prints the report of a file, one line for
each finding:

```cpp
const std::vector<openfits::diagnostic> findings = openfits::verify(path);
for (const openfits::diagnostic& finding : findings) {
    std::cout << path << ": " << severity_name(finding.severity) << ": " << place(finding.context) << ": "
              << finding.message;
    if (!finding.section.empty()) {
        std::cout << " (standard " << finding.section << ')';
    }
    std::cout << '\n';
}
```

The diagnostics of a reader carry the same sections. Their severities say what reading a file takes: an `error` leaves
part of the file unavailable, a `warning` is a departure read as intended, and an `info` a departure that changes
nothing, a form that the standard deprecates, or a convention outside it.

An `editor` opens a file to change it, from a path, a file specification that selects a unit, or a device, and walks
every unit, since it needs the whole layout. Its changes are pending until `commit()`, which applies them together: the
header of a unit, changed as any header, its structural keywords refused; values written into images, region by region,
and into the fields of tables, converted as a save converts them; rows appended to tables; and units appended,
inserted, replaced and erased. A commit writes in place what keeps the layout of the file, each write touching only the
bytes that change: a header that still fits its blocks, the blank cards before `END` taking the cards added, values,
rows appended to a table without a heap that ends the file, units appended. Any other change rewrites the file through
a copy that replaces it once complete, so that a failure leaves the old file or the new one, or, for files too large to
copy, moves the units within the file (`relayout::in_place`), where a failure leaves it damaged;
`editor_options::atomic` applies every commit through a copy. The `CHECKSUM` and `DATASUM` of a unit that changes are
updated from the bytes that change alone, as standard Appendix J.4 describes, with no pass over the unit, so that a unit
that matched them still does. This excerpt of `samples/update_keyword.cpp` sets a keyword, given as the text of its
value, in the header of the unit that a file specification such as `image.fits[SCI,2]` selects:

```cpp
openfits::editor file(openfits::file_specification::parse(arguments[0]));
const std::size_t index = file.selected_hdu();
openfits::header& cards = file.header(index);  // Valid until the commit.
openfits::card edited = openfits::card::with_value_text(keyword, value_text, cards.comment(keyword).value_or(""));
if (const std::optional<std::size_t> position = cards.index_of(keyword)) {
    (void)cards.erase(keyword);
    cards.insert(*position, std::move(edited));
} else {
    cards.append(std::move(edited));
}
cards.add_history("Keywords changed by the sample update_keyword");
file.commit();
```

`update_keywords(path, hdu, cards)` sets cards in a header and commits, in one call. An output frame, a stack or a
mosaic, is written region by region into the zeros of a reserved image, and the rows of a log or a catalogue appended as
they come:

```cpp
openfits::editor frame(path);
frame.write_pixels(0, std::span<const float>(tile), {.region = openfits::region{.start = {x, y}, .size = {512, 512}}});
frame.append_rows(1, std::vector<openfits::column_values>{std::span(names), std::span(fluxes)});
frame.commit();
```

A compressed image is edited as the image it holds: its header through the `Z` keywords of its table, and its values in
the tiles that the regions written meet, which the commit encodes again, a quantized tile at the spacing of its levels.
When a commit throws, its changes stay pending, and the next commit applies them again; `discard()` drops them.

## Thread safety, errors and text

These are the design rules. They take effect as the API arrives.

- **Thread safety.** There is no mutable global state. A `reader` never changes in the eyes of its callers once
  constructed (the walk of its units is internal and synchronized), so any number of threads can use it. Writers,
  editors and values are thread-compatible, like standard containers; the reader of an editor can be used by any
  number of threads between its commits, and another reader of a file that an editor changes is not supported. The
  file and memory sources can be read from any number of threads at once, and the file and memory devices read and
  written; the library calls a source that does not declare concurrent reads, or a device that does not declare
  concurrent writes, from one thread at a time.
- **Errors.** Failures are exceptions derived from `openfits::error`, one class per kind of failure, each with an error
  code. A problem confined to one header and data unit, such as an unsupported compression, makes that unit
  unavailable and is reported as a diagnostic; the rest of the file stays readable. Harmless departures from the
  standard, which files in the wild are full of, are read as intended, with a diagnostic. A strict mode turns such
  problems into failures.
- **Text.** The API carries text as UTF-8 in `std::string` and `std::string_view`, paths included, on every platform;
  invalid UTF-8 in a path is refused. A path is always the name of a file, so that `M31 [Ha].fits` opens as itself.
  Header cards hold printable ASCII, as the standard requires: other bytes are reported when read and refused when
  written, unless the save replaces them with question marks or keeps them (`write_options::non_ascii`), for
  applications that copy cards as they read them.

## Security

Report a vulnerability through GitHub's private vulnerability reporting, on the Security tab of the repository. Please do
not open a public issue for it.

Files are untrusted input. The design goals are: every size and offset is overflow-checked, from the `BITPIX`, `NAXISn`,
`PCOUNT` and `GCOUNT` keywords that give the size of a data unit to the descriptors of variable-length arrays and the
tiles of compressed images; allocations are limited and configurable, and so are the number of header and data units,
of cards in a header and the size of a table heap; checksums are verified when a whole unit is read, and on request;
compressed tiles decode to exactly their declared size or fail; there is no network access. They are checked
continuously by fuzzing, sanitizers and static analysis.

## Contributing

- Tests come first, with GoogleTest. Each behaviour is tested once, at the lowest level that proves it.
- A change to a parser, a codec or the I/O layer adds or extends a fuzz target, or adds a seed to `fuzz/seeds`. An input
  that once broke a target becomes a seed.
- Write plain code: classes and free functions, templates only where they remove real duplication. The naming follows
  the standard library: `snake_case` everywhere, lowercase enumerators, a trailing `_` on private members.
- Public headers carry short Doxygen comments. Every source file starts with an SPDX header, as the existing ones do;
  a file you create names you in its `SPDX-FileCopyrightText` line, and a file you change substantially can get a line
  of its own for you.
- Format with clang-format 22 and check with clang-tidy 22 (`pip install clang-format==22.1.3 clang-tidy==22.1.8`).
  Both run in CI.
- Sign off your commits to certify the [Developer Certificate of Origin](https://developercertificate.org/):
  `git commit -s`.
- Do not copy code from other FITS implementations, such as CFITSIO or wcslib, whatever their licence: this library is
  written from the standard.
- A pull request needs a green CI: every platform builds with warnings as errors, and the tests pass, also under the
  sanitizers.

## Author

OpenFITS is written and maintained by Ezequiel Ruiz ([@emruiz81](https://github.com/emruiz81)). Contributors are
credited in the history of the repository and in the SPDX headers of the files they wrote.

## License

OpenFITS is licensed under the [Apache License 2.0](LICENSE); see also [NOTICE](NOTICE).

OpenFITS is an independent implementation. It is not affiliated with or endorsed by the International Astronomical
Union or by NASA.
