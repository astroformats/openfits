# OpenFITS

OpenFITS is a C++20 library for reading, writing and editing FITS, the Flexible Image Transport System, the standard
file format of astronomy. It is written from the standard of the IAU FITS Working Group and aims at complete
conformance with [FITS 4.0](https://fits.gsfc.nasa.gov/fits_standard.html): every structure the standard defines is
read, everything but the deprecated random groups is written, existing files are edited in place, and the conventions
in common use beyond the standard, such as gzip-compressed files and `HIERARCH` keywords, are supported; with an API
that does not depend on any application, so that it can replace CFITSIO in C++ programs. It is the second library of
[AstroFormats](https://github.com/astroformats), after [OpenXISF](https://github.com/astroformats/openxisf), and
shares its design.

**Status: 0.1.0.** An early release, which reads and writes headers and images: the primary array and `IMAGE`
extensions with every `BITPIX`, their values converted into any numeric type with the scaling, undefined values and
unsigned conventions of the standard, read whole or as regions with steps, into memory of the library or of the
application. Tables, checksums, tiled compression, gzip-compressed files and editing in place come in later releases,
and the API may change before 1.0. Files are read from, written to and edited through sources, sinks and devices of the
library or of the application; their header and data units are walked as they are asked for, and their headers read as
typed values and changed.

## Features and conformance

| Area | Reading | Writing | Editing |
|---|---|---|---|
| Primary array and `IMAGE` extensions, any number of header and data units, every `BITPIX` | Yes | Yes | Planned |
| Header cards: fixed and free format, strings, logicals, integers, reals, complex and undefined values, commentary, units, display formats, reserved header space | Yes | Yes | Planned |
| Long strings (`CONTINUE`), `HIERARCH` keywords, `INHERIT` and `LONGSTRN` as keywords | Yes | Yes | Planned |
| Pixel data: big-endian to native order, `BSCALE`, `BZERO` and `BLANK`, the unsigned conventions, conversion to any numeric type, regions with steps | Yes | Yes: whole images | Planned |
| Binary tables (`BINTABLE`): every column type, variable-length arrays, scaling and nulls | Planned | Planned | Planned |
| ASCII tables (`TABLE`) | Planned | Planned | Planned |
| Random groups | Planned | Not planned (deprecated by the standard) | Not planned |
| Checksums (`CHECKSUM` and `DATASUM`) | Planned | Planned | Planned |
| Tiled image compression: `RICE_1`, `GZIP_1`, `GZIP_2`, `PLIO_1`, `HCOMPRESS_1` and `NOCOMPRESS`, with quantization and dithering | Planned | Planned | Planned: keywords in place, regions re-encode the unit |
| Tiled table compression | Planned | Planned | Planned: keywords in place, cells through a rewrite |
| gzip-compressed files (`.fits.gz`) | Planned | Planned | Not applicable |
| Extensions of unknown type, special records | Yes (raw) | Planned: copied unchanged | Planned: copied unchanged |
| File specifications `file.fits[2]`, `file.fits[SCI,1]`, `file.fits[1][*,1:100]`, parsed when asked for | Yes | Not applicable | Planned: unit selection |
| WCS and time keywords, read and written as keywords, not evaluated | Yes | Yes | Planned |
| Verification of a file against the standard, each departure reported with its section | Planned | Not applicable | Not applicable |

Editing means changing an existing file in place: keywords, data values, appended rows and units, and a safe rewrite
for every change of layout. Not planned: evaluating world coordinates (the work of a coordinate library), the filtering
and calculator syntax of CFITSIO's extended file names, the hierarchical grouping convention, and writing random
groups. The table is updated at each release.

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
the Linux ones add UndefinedBehaviorSanitizer), `linux-clang-tsan` (ThreadSanitizer) and `linux-gcc-coverage`.

### Dependencies

- **vcpkg, manifest mode.** Set `VCPKG_ROOT` to a vcpkg checkout. The `msvc`, `clang-cl` and `linux-clang-libcxx`
  presets use it automatically, and `vcpkg.json` pins the versions with a baseline. `linux-clang-libcxx` builds the
  dependencies against libc++ too, with the triplet of `cmake/vcpkg`: it needs libc++, and `CC` and `CXX` set to
  Clang. With the other presets add `-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake`.
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
output.save(path);
```

A write into integer data rounds half away from zero, and refuses a value beyond the range of the stored type, or clamps
it on request. `save()` replaces a file only once the new one is complete, or refuses to replace one
(`if_exists::fail`), and writes into any sink as well: `samples/stream_io.cpp` writes through a `std::ofstream`, and
reads the file back through a `std::ifstream` and a `FILE*`. The examples of editing files arrive with that part of the
API.

## Thread safety, errors and text

These are the design rules. They take effect as the API arrives.

- **Thread safety.** There is no mutable global state. A `reader` never changes in the eyes of its callers once
  constructed (the walk of its units is internal and synchronized), so any number of threads can use it. Writers and
  values are thread-compatible, like standard containers. The file and memory sources can be read from any number of
  threads at once, and the file and memory devices read and written; the library calls a source that does not declare
  concurrent reads, or a device that does not declare concurrent writes, from one thread at a time.
- **Errors.** Failures are exceptions derived from `openfits::error`, one class per kind of failure, each with an error
  code. A problem confined to one header and data unit, such as an unsupported compression, makes that unit
  unavailable and is reported as a diagnostic; the rest of the file stays readable. Harmless departures from the
  standard, which files in the wild are full of, are read as intended, with a diagnostic. A strict mode turns such
  problems into failures.
- **Text.** The API carries text as UTF-8 in `std::string` and `std::string_view`, paths included, on every platform;
  invalid UTF-8 in a path is refused. A path is always the name of a file, so that `M31 [Ha].fits` opens as itself.
  Header cards hold printable ASCII, as the standard requires: other bytes are reported when read and refused when
  written.

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
