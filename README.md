# OpenFITS

OpenFITS is a C++20 library for reading, writing and editing FITS, the Flexible Image Transport System, the standard
file format of astronomy. It is written from the standard of the IAU FITS Working Group and aims at complete
conformance with [FITS 4.0](https://fits.gsfc.nasa.gov/fits_standard.html): every structure the standard defines is
read, everything but the deprecated random groups is written, existing files are edited in place, and the conventions
in common use beyond the standard, such as gzip-compressed files and `HIERARCH` keywords, are supported; with an API
that does not depend on any application, so that it can replace CFITSIO in C++ programs. It is the second library of
[AstroFormats](https://github.com/astroformats), after [OpenXISF](https://github.com/astroformats/openxisf), and
shares its design.

**Status: 1.0.0.** It reads, writes and edits every structure of FITS 4.0: headers, the primary array and `IMAGE`
extensions with every `BITPIX`, binary and ASCII tables with every column type and variable-length arrays, random
groups (read), `DATASUM` and `CHECKSUM`, and images and tables compressed in tiles with every algorithm of the standard,
quantization and dithering among them; and the conventions in common use beyond it, gzip-compressed files and
`HIERARCH` keywords among them. The values of images and tables are converted into any numeric type with the scaling,
undefined values and unsigned conventions of the standard, read whole or as regions with steps, into memory of the
library or of the application. The writer checks every model before it writes, streams the values of images and the
rows of tables to the file as they are produced, reserves the data of images for later filling, and copies the units
of other files byte for byte; existing files are edited, keywords, values and rows changed in place where the layout of
the file allows it, and units appended, inserted, replaced and erased, their checksums kept up to date from the bytes
that change; and the conformance of a file to the standard is reported in a call, each departure with the section of
the standard that it departs from. On x86-64 the bytes of values are swapped, shuffled and summed by SSE2 kernels, and
by AVX2 ones on the processors that have it, chosen at run time; a file is read by any number of threads at once on
every system.

The API is stable: from 1.0 on, only a new major version may break it. The ABI holds within a minor version: the shared
library of each minor version has a soname of its own (`libopenfits.so.1.0` for 1.0.x) and a DLL name of its own on
Windows (`openfits-1.0.dll`, `libopenfits-1.0.dll` with MinGW), so a program built against 1.0 is rebuilt to use 1.1.
The import library keeps its name (`openfits.lib`, `libopenfits.dll.a`), so a program links as before. Files are read
from, written to and edited through sources, sinks and devices of the library or of the application; their header and
data units are walked as they are asked for, and their headers read as typed values and changed.

## What no other FITS library offers

- **Regions with a pitch, read in few reads.** A region of an image, a box with a step on each axis, a negative one
  reading the axis in reverse, lands in the application's buffer with a row pitch of its own: a cutout of a stack of
  frames goes straight into the place where the stack wants it. The runs of a region that lie close together are read
  in one request, so that a region costs reads in proportion to its bytes, not to its rows.
- **An editor that commits.** Changes to an existing file are pending until a commit, which writes in place what keeps
  the layout of the file, a header that fits its blocks, values, rows appended at the end, and rewrites the file through
  a copy that replaces it atomically otherwise, so that a failure leaves the old file or the new one.
- **Checksums that survive edits.** A unit whose `CHECKSUM` and `DATASUM` matched it still matches them after an edit:
  they are updated from the bytes that change alone (standard Appendix J.4), with no pass over the unit.
- **The verification report in a call.** `openfits::verify()` gives each departure of a file from the standard, with
  the section of the standard that it departs from: the work of `fitsverify`, as a function.
- **Reproducible output.** The same model and options give the same bytes on every platform: the library reads no
  clock and no environment, the dither seed of a quantization comes from the data unless given, and reals are written
  as the shortest text that reads back as the same value.
- **Concurrent readers without a global lock.** Any number of threads read one file at once, and each file is read
  apart from every other; the tiles of a compressed image are decompressed, and those of the writer compressed, on
  every core in a build with oneTBB.
- **Lossless defaults.** A file written with the default options holds the structures of the standard alone, which
  every FITS reader reads; a floating-point image is compressed losslessly unless a quantization level is given.
- **File names with brackets.** A path is always the name of a file, so that `M31 [Ha].fits` opens as itself; the
  extended file names of CFITSIO, such as `image.fits[SCI,2][1:100,1:100]`, are parsed only when asked for.

## Speed

Every operation that both libraries offer is measured against CFITSIO 4.7.0 in pairs that do the same work: the same
data in the same files, checksums verified on both sides or on neither, on one thread. In each of the 179 pairs of 1.0
the median time of OpenFITS is at most that of CFITSIO within the noise of the measurement, 5%, and lower where the work
vectorizes or parallelizes: a frame of 16-bit values is read in 0.57 of CFITSIO's time and written in 0.50, a region of
512 x 512 pixels of it read in 0.17 and its checksums verified in 0.16, a header of 100,000 cards opened in 0.82, the
columns of a table of a million rows read in about a quarter and written in 0.08, and a frame written into a gzip file
in 0.06. Where both run zlib at the same level, on the tiles of `GZIP_1` and `GZIP_2`, the two take the same time.

Where CFITSIO serializes every call behind one global lock, OpenFITS reads a file from any number of threads at once,
and decompresses and compresses tiles on every core in a build with oneTBB. The table gives the ratio of the median time
of OpenFITS to that of CFITSIO on the same work, with its 95% confidence interval, on one thread and on 4, 8 and 16;
below 1, OpenFITS is faster. The frames are read on as many threads on both sides, CFITSIO's each with a handle of its
own, in its thread-safe build; CFITSIO compresses and decompresses tiles on the one thread that its API gives the work.

| Operation | 1 thread | 4 threads | 8 threads | 16 threads |
|---|---|---|---|---|
| A file of a float frame compressed with quantization, as `fpack` does | 0.60 (0.60 to 0.61) | 0.20 (0.19 to 0.20) | 0.13 (0.13 to 0.14) | 0.12 (0.12 to 0.13) |
| A file of a 16-bit frame compressed with `RICE_1`, as `fpack` does | 0.73 (0.73 to 0.75) | 0.34 (0.32 to 0.35) | 0.23 (0.23 to 0.24) | 0.21 (0.20 to 0.21) |
| A float frame compressed with quantization | 0.65 (0.64 to 0.65) | 0.21 (0.21 to 0.21) | 0.14 (0.14 to 0.14) | 0.16 (0.15 to 0.16) |
| A 16-bit frame compressed with `RICE_1` | 0.88 (0.86 to 0.90) | 0.37 (0.37 to 0.39) | 0.31 (0.31 to 0.32) | 0.29 (0.29 to 0.29) |
| A float frame of `GZIP_2` tiles read | 0.90 (0.89 to 0.90) | 0.28 (0.28 to 0.28) | 0.18 (0.18 to 0.19) | 0.13 (0.13 to 0.14) |
| A quantized float frame read | 0.64 (0.64 to 0.65) | 0.19 (0.19 to 0.20) | 0.13 (0.13 to 0.13) | 0.11 (0.11 to 0.11) |
| A 16-bit frame of `GZIP_2` tiles read | 0.82 (0.82 to 0.83) | 0.26 (0.26 to 0.26) | 0.17 (0.17 to 0.17) | 0.12 (0.12 to 0.12) |
| A 16-bit frame of `HCOMPRESS_1` tiles read | 0.44 (0.44 to 0.45) | 0.12 (0.12 to 0.12) | 0.07 (0.07 to 0.07) | 0.05 (0.05 to 0.05) |
| A 16-bit frame of `RICE_1` tiles read | 0.48 (0.48 to 0.49) | 0.17 (0.17 to 0.17) | 0.12 (0.12 to 0.13) | 0.10 (0.10 to 0.10) |
| A 16-bit frame of `RICE_1` tiles of 16 rows read | 0.52 (0.52 to 0.52) | 0.18 (0.18 to 0.18) | 0.12 (0.12 to 0.12) | 0.10 (0.10 to 0.11) |
| Frames read from as many files | - | 0.57 (0.56 to 0.58) | 0.60 (0.59 to 0.61) | 0.65 (0.64 to 0.66) |
| Frames read from one file | - | 0.72 (0.71 to 0.74) | 0.74 (0.73 to 0.75) | 0.91 (0.89 to 0.92) |
| The `RICE_1` tiles of 100 MB decoded | - | 0.17 (0.17 to 0.17) | 0.12 (0.12 to 0.12) | 0.10 (0.10 to 0.10) |
| The `RICE_1` tiles of 100 MB encoded | - | 0.36 (0.35 to 0.37) | 0.27 (0.27 to 0.28) | 0.32 (0.31 to 0.32) |
| A `RICE_1` frame read row by row through a tile cache | 0.52 (0.51 to 0.52) | 0.14 (0.14 to 0.14) | 0.09 (0.09 to 0.10) | 0.08 (0.08 to 0.08) |
| A small `RICE_1` image read row by row through a tile cache | 0.76 (0.76 to 0.77) | 0.24 (0.24 to 0.25) | 0.27 (0.26 to 0.27) | 0.27 (0.27 to 0.28) |
| The checksums of 1 GB verified | 0.18 (0.18 to 0.18) | - | - | - |
| A float frame written in `GZIP_2` tiles | 0.98 (0.97 to 0.99) | 0.30 (0.30 to 0.31) | 0.19 (0.18 to 0.19) | 0.14 (0.14 to 0.15) |
| A float frame written quantized | 0.64 (0.64 to 0.65) | 0.19 (0.18 to 0.19) | 0.12 (0.12 to 0.12) | 0.14 (0.14 to 0.14) |
| A 16-bit frame written in `GZIP_2` tiles | 0.92 (0.90 to 0.94) | 0.33 (0.33 to 0.33) | 0.20 (0.19 to 0.20) | 0.14 (0.13 to 0.14) |
| A 16-bit frame written in `HCOMPRESS_1` tiles | 0.74 (0.74 to 0.75) | 0.23 (0.23 to 0.23) | 0.15 (0.15 to 0.15) | 0.11 (0.11 to 0.12) |
| A 16-bit frame written in `RICE_1` tiles | 0.80 (0.79 to 0.80) | 0.39 (0.38 to 0.39) | 0.26 (0.25 to 0.27) | 0.23 (0.22 to 0.24) |
| A 16-bit frame written in `RICE_1` tiles of 16 rows | 0.80 (0.79 to 0.81) | 0.31 (0.31 to 0.32) | 0.24 (0.23 to 0.24) | 0.21 (0.20 to 0.21) |

Measured on an AMD Ryzen 9 9950X (16 cores, 32 threads) under Windows 11, both libraries built by MSVC 14.51 in Release,
CFITSIO with its `pthreads` option and OpenFITS with oneTBB, the files on an NVMe disk; the frames are 6000 x 4000
pixels, and each pair of the table ran at least 20 times, in random order with the others.

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
hierarchical grouping convention, and writing random groups, which the standard deprecates.

The conformance matrix below lists each area of the standard and each convention that the library implements, whether
it is read, written and edited, and the suites of `tests/` that cover it. Every row has passing tests. A convention is
outside the standard.

| Standard | Area | Reading | Writing | Editing | Tests |
|---|---|---|---|---|---|
| §3.1, §4.1 | Blocks, cards, `END`, padding | ✓ | ✓ | ✓ headers in their blocks; a short last block filled before an append | `conformance/file_structure`, `unit/header_emission`, `conformance/tolerance`, `unit/writer`, `unit/edit_header`, `unit/editor_commit` |
| §4.1.2, §4.2, App. A | Keyword names, value forms, fixed and free format, undefined values | ✓ | ✓ | ✓ the records of the cards read kept | `conformance/keyword_record`, `conformance/value`, `unit/record`, `unit/header`, `unit/card`, `unit/write_validation`, `unit/edit_header` |
| §4.2.1.2, §4.4.2.4 | Commentary keywords, `CONTINUE` long strings | ✓ | ✓ | ✓ | `unit/long_string`, `conformance/headers`, `conformance/tolerance`, `unit/edit_header` |
| §4.4.2.4 | Reserved header space: blank cards before `END` | ✓ | ✓ reserved cards | ✓ taken before the header grows | `unit/header`, `unit/header_emission`, `conformance/headers`, `unit/editor`, `unit/edit_header` |
| Convention | `HIERARCH` keywords | ✓ | On request | On request | `conformance/headers`, `unit/record`, `unit/long_string` |
| §4.4.1 | Mandatory keywords, primary and extensions | ✓ | ✓ from the model | ✓ kept with their order | `conformance/file_structure`, `conformance/tolerance`, `unit/writer`, `unit/edit_header`, `unit/editor` |
| §4.3, §4.4.2, §7.3.4 | Reserved keywords, units in comments, display formats | ✓ | ✓ | ✓ | `conformance/headers`, `conformance/units`, `conformance/display_format` |
| §4.2.7, §9 | Dates and times as keywords | ✓ | ✓ | ✓ `DATE` never changed | `conformance/datetime` |
| §3.3.2, §5, §7.1 | Primary array, `IMAGE` extensions, every `BITPIX` | ✓ | ✓ | ✓ units appended, inserted, replaced and erased | `conformance/images`, `unit/image_info`, `unit/image_read`, `unit/writer`, `samples/images`, `concurrency/pixels`, `unit/editor`, `concurrency/editor` |
| §4.4.2.5, §5.2.5 | `BSCALE`, `BZERO`, `BLANK`, the unsigned and signed-byte conventions, conversion into any numeric type | ✓ | ✓ | ✓ | `unit/conversion`, `conformance/images`, `unit/writer`, `unit/editor_data` |
| – | Regions of interest in every dimension, with steps and a pitch; reserved units filled by regions | ✓ | ✓ reserved units | ✓ regions | `unit/image_read`, `concurrency/pixels`, `unit/write_stream`, `unit/edit_patch`, `unit/editor`, `unit/editor_commit` |
| §6 | Random groups, parameters of one name summed | ✓ | – deprecated | Keywords and raw bytes | `unit/random_groups`, `conformance/tables`, `conformance/tolerance`, `samples/tables`, `unit/editor_commit` |
| §7.3 | `BINTABLE`: every `TFORMn`, `TDIMn`, nulls, scaling, case-insensitive names | ✓ | ✓ | ✓ fields in place, rows appended | `unit/column_format`, `unit/table_read`, `unit/table_write`, `conformance/tables`, `conformance/tolerance`, `samples/tables`, `concurrency/columns`, `unit/editor_data`, `unit/editor` |
| Convention | `TFORMn = 'rAw'` substrings | ✓ | On request | ✓ | `unit/column_format`, `unit/table_read`, `unit/table_write`, `samples/tables` |
| §7.3.5 | Variable-length arrays and the heap | ✓ | ✓ | Through a rewrite: rows appended, their arrays after the heap | `unit/table_heap`, `unit/table_write`, `conformance/tables`, `samples/tables`, `unit/editor`, `unit/editor_data` |
| §7.2 | ASCII `TABLE`, its fill of spaces | ✓ | ✓ | ✓ fields in place, rows appended | `unit/ascii_table`, `unit/table_write`, `conformance/tables`, `samples/tables`, `unit/editor_data`, `unit/editor` |
| §3.4.1, App. F | Extensions of unknown type, the reserved names | ✓ as bytes | ✓ copied | ✓ kept, raw bytes written | `conformance/file_structure`, `unit/hdu_copy`, `unit/editor` |
| §3.3, §4.4.1, §7, §10 | The writer: every model checked before a byte is written, data streamed chunk by chunk, units copied as they are or converted to their place | – | ✓ | ✓ the units of a writer placed in a file | `unit/write_validation`, `unit/write_stream`, `unit/write_matrix`, `unit/hdu_copy`, `unit/writer`, `unit/editor` |
| §3.5 | Special records | ✓ tolerated | – | ✓ kept after the units | `conformance/file_structure`, `conformance/tolerance`, `unit/editor_commit` |
| §4.4.2.7, App. J | `CHECKSUM` and `DATASUM` | ✓ | ✓ | ✓ updated from the bytes that change (App. J.4) | `unit/checksum`, `conformance/checksums`, `conformance/tolerance`, `unit/writer`, `unit/reader`, `samples/checksums`, `concurrency/pixels`, `unit/edit_patch`, `unit/edit_header`, `unit/editor`, `unit/editor_commit` |
| §10.1, §10.4 | Tiled image compression: `RICE_1` with every `BYTEPIX`, `GZIP_1`, `GZIP_2`, `PLIO_1`, `HCOMPRESS_1`; a cache of decoded tiles | ✓ | ✓ | ✓ keywords through the `Z` keywords, regions re-encode the tiles they meet | `unit/rice`, `unit/gzip_tiles`, `unit/plio`, `unit/hcompress`, `unit/tile_grid`, `unit/tile_cache`, `unit/compressed_image`, `conformance/compression`, `samples/images`, `samples/checksums`, `concurrency/tiles`, `unit/editor_data` |
| §10.3.5, Table 36 | `NOCOMPRESS` | ✓ | On request | As compressed images | `conformance/compression`, `unit/compressed_image`, `unit/compressed_table`, `samples/images` |
| Convention | `ZQUANTIZ = 'NONE'`; old compressed files: `RICE_ONE`, reversed `ZVAL1` and `ZVAL2`, `NOISEBIT`, `ZSCALE` and `ZZERO` as keywords | ✓ | – the conforming form is written | – | `conformance/tolerance`, `samples/images` |
| §10.2, §10.1.3, App. I | Quantization and dithering, undefined pixels, `ZBLANK` as a keyword and a column, the reserved integers as the files have them | ✓ | ✓ | ✓ tiles quantized again at the spacing of their levels | `unit/quantize`, `unit/compressed_image`, `conformance/compression`, `samples/images`, `unit/editor_data` |
| §10.3 | Tiled table compression | ✓ | ✓ | Keywords; cells through a replaced unit | `unit/compressed_table`, `conformance/compression`, `samples/tables`, `unit/editor_data` |
| §10.1.2 | Compressed primary array: an empty primary HDU and the compressed unit with `ZSIMPLE`, named `COMPRESSED_IMAGE` by convention | ✓ | ✓ | As compressed images | `unit/compressed_image`, `samples/images` |
| Convention | gzip-compressed files | ✓ | On request | – written again whole | `unit/gzip_file`, `samples/gzip`, `concurrency/io` |
| Convention | File specifications: a unit and an image section | ✓ | – | ✓ a unit | `unit/file_specification`, `unit/editor` |
| Convention | `LONGSTRN` | ✓ as a keyword | On request | On request | `unit/header_emission`, `unit/edit_header` |
| App. K | `INHERIT` | As a keyword | As a keyword | As a keyword | `conformance/headers` |
| §8, §9 | WCS and time keywords | As keywords, their types checked | As keywords | As keywords | `conformance/headers`, `unit/keywords` |
| App. L | Green Bank convention | As a binary table | As a binary table | As a binary table | `conformance/tables` |
| Every section | Verification report: every departure with its section | ✓ | – | – | `unit/verify`, `conformance/tolerance`, `samples/verify` |
| §3, §4.4.2.7, App. J.4 | Editing: keywords, values and rows in place, units appended, inserted, replaced and erased, relayout through a copy or in place, atomic commits, incremental checksums | – | – | ✓ | `unit/editor`, `unit/editor_data`, `unit/editor_commit`, `unit/edit_patch`, `unit/edit_layout`, `unit/edit_header`, `concurrency/editor` |

### Known departures

Where the text of the standard and every file in existence disagree, the writer follows the files, and the reader reads
both forms where it can tell them apart:

- **The reserved integers of `SUBTRACTIVE_DITHER_2`.** Standard §10.2.1 reserves -2147483647 for the pixels that are
  exactly zero, and the `ZBLANK` value for undefined ones, for which §10.1.3 recommends -2147483648. CFITSIO, astropy
  and every file that uses the method store undefined pixels as -2147483647, given by a `ZBLANK` keyword, and zeros as
  -2147483646. The writer writes that form, so that its tiles are those of CFITSIO; the reader takes zero for
  -2147483646 when `ZBLANK` is -2147483647, and for -2147483647 when `ZBLANK` is -2147483648.
- **`ZQUANTIZ = 'NONE'`.** A value that standard §10.2.1 does not list, which CFITSIO writes for a floating-point image
  compressed without quantization: the reader reads it as such an image. The writer omits the keyword, the form of the
  standard, which CFITSIO, `funpack`, `fitsverify` and astropy read.

### Tiled compression

Compressed images and tables are read as the files of `fpack` and of the other writers have them, their older markings
included (`ZCMPTYPE = 'RICE_ONE'`, `ZQUANTIZ = 'NONE'`, `ZSCALE` and `ZZERO` as keywords). The writer compresses with
the parameters of `fpack`, and its `RICE_1`, `PLIO_1`, `HCOMPRESS_1` and quantized tiles are the bytes that CFITSIO
writes for the same parameters, but where CFITSIO's would not restore the image: a `PLIO_1` run of zeros of a multiple
of 4095 pixels before a single pixel, a lossy `HCOMPRESS_1` tile that holds undefined pixels, which the writer
compresses losslessly, and a floating-point tile whose quantization would restore infinities or other values, its codes
beyond 32 bits, which it keeps as its values. A lossy `HCOMPRESS_1` tile restores values a little beyond those it
compressed, which a read takes to the nearest value of the type of the image; a lossless one that decodes beyond them is
corrupt data. A floating-point image compressed without a quantization level is lossless: its tiles
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

### Interoperability

Every file that the tests write is checked by `fitsverify`, `funpack` and astropy in CI. PixInsight 1.9.6 and Siril
1.4.4 read the images that the writer writes, compressed or not, with the values that OpenFITS reads, within what
each supports: PixInsight has no images of 64-bit integers or of one dimension, and rescales data beyond [0, 1] by
their range; Siril brings 32-bit integers into 16 bits by a scaling of its own.

The files of the imaging programs are read, among them the FITSEQ sequences of Siril, one image for each unit, its
compressed files and its `ROWORDER` keyword, and the `ICCProfile` unit that Siril writes after an image with an ICC
profile, a convention of its own: an `IMAGE` extension of `BITPIX = 8` and `NAXIS = 1` whose data are the bytes of the
profile, which `reader::hdu("ICCProfile")` finds, `writer::add_hdu()` copies byte for byte and the editor keeps through
every change of layout. In a compressed file Siril gives that unit the keywords of a compressed image as well, which
the reader reads through with warnings, the unit an image all the same. PixInsight 1.9.6 writes no ICC profile into a
FITS file, and reads none.

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
| `OPENFITS_ENABLE_SIMD` | `ON` | Use the SIMD kernels of byte swapping, shuffling and checksums on x86-64: SSE2, and AVX2 where the processor has it, chosen at run time; the scalar code is always built |
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

## Coming from CFITSIO

Every operation of CFITSIO's C API that concerns the format has its counterpart, but those of the exclusions above:

| CFITSIO | OpenFITS |
|---|---|
| `fits_open_file`, `fits_open_diskfile`, `fits_open_image`, `fits_open_table`, `fits_open_data`, `fits_open_memfile` | `reader` of a path, always the name of a file, of a source (`file_source`, `memory_source`, one of the application) or of a `file_specification` |
| `fits_movabs_hdu`, `fits_movnam_hdu`, `fits_movrel_hdu`, `fits_get_num_hdus`, `fits_get_hdu_type`, `fits_get_hduaddr` | `reader::hdu(index)`, `hdu(name, version)`, `find_hdu()`, `first_image()`, `first_table()`, `images()`, `tables()`, `hdu_count()`, `hdus()`; `hdu_info::kind()`, `header_offset()`, `data_offset()`, `data_size()` |
| `fits_read_record`, `fits_read_card`, `fits_read_key*`, `fits_read_keyn`, `fits_find_nextkey`, `fits_read_keys_*`, `fits_get_hdrspace`, `fits_hdr2str`, `fits_read_key_unit`, `fits_parse_value`, `fits_get_keytype`, `fits_get_keyclass` | `header`: its cards in order, `value<T>()`, `find<T>()`, `values<T>()`, `matching()` with `*`, `?` and `#`, `free_cards()`, `to_string()`; `card::unit()`, `card_value`, `is_structural()` |
| `fits_write_key*`, `fits_update_key*`, `fits_write_record`, `fits_insert_*`, `fits_modify_*`, `fits_delete_*`, `fits_write_comment`, `fits_write_history`, `fits_write_date`, `fits_write_key_longstr`, `fits_set_hdrsize` | `header::set()`, `append()`, `insert()`, `erase()`, `erase_at()`, `rename()`, `add_comment()`, `add_history()`; long strings continued by the library, `LONGSTRN` on request; `format_date()` of a time that the application gives; `reserved_cards` |
| `fits_read_pix*`, `fits_read_img*`, `fits_read_subset`, `fits_read_imgnull`, `fits_set_bscale`, `fits_get_img_param*`, `fits_get_img_equivtype` | `reader::image()`, its `bitpix`, `geometry`, `scaling` and `value_type()`; `read_pixels<T>()` into memory of the library or of the application, with the `region`, `pitch`, `blank` and `scaling` of `pixel_read_options` |
| `fits_write_pix*`, `fits_write_img*`, `fits_write_subset`, `fits_write_null_img`, `fits_create_img`, `fits_insert_img`, `fits_resize_img`, `fits_copy_image_section` | `writer::add_image()` of any type and scaling, from memory, streamed or reserved; `editor::write_pixels()`, `write_blank()`, `append()`, `replace_hdu()` |
| `fits_read_grppar*`, `fits_read_grpdata` | `reader::groups()`, `read_parameter<T>()`, `read_group<T>()` |
| `fits_get_colnum`, `fits_get_colname`, `fits_get_coltype*`, `fits_get_num_rows`, `fits_get_num_cols`, `fits_read_col*`, `fits_read_colnull*`, `fits_read_descript*`, `fits_read_tdim`, `fits_set_tscale`, `fits_read_col_bit`, `fits_iterate_data` | `reader::table()`, its `table_info` and `column_info`, `find_column()`, `matching_columns()`; `read_column<T>()`, `read_columns()`, `read_strings()` and `read_arrays<T>()` over ranges of rows |
| `fits_create_tbl`, `fits_write_col*`, `fits_write_colnull*`, `fits_insert_rows`, `fits_delete_rows`, `fits_insert_col*`, `fits_delete_col`, `fits_modify_vector_len`, `fits_write_tdim`, `fits_compress_heap`, `fits_copy_col`, `fits_copy_rows`, `fits_binary_tform`, `fits_ascii_tform` | `writer::add_table()`; `editor::write_cells()`, `append_rows()`, and the other changes of a table's structure through `replace_hdu()`, with columns read from it; `column_format::parse_binary()` and `parse_ascii()` |
| `fits_read_tblbytes`, `fits_write_tblbytes` | `reader::read_raw()`, `editor::write_raw()` |
| `fits_create_file` and its `!` prefix, `fits_create_diskfile`, `fits_create_memfile` | `writer::save()` to a path, replaced only once complete or refused (`write_options::if_exists`), or to a sink, `memory_sink` among them |
| `fits_write_chksum`, `fits_update_chksum`, `fits_verify_chksum`, `fits_get_chksum`, `fits_encode_chksum`, `fits_decode_chksum` | `write_options::checksums`, the updates of the editor, `reader::verify()`, `encode_checksum()`, `decode_checksum()` |
| `fits_set_compression_type`, `fits_set_tile_dim`, `fits_set_quantize_level`, `fits_set_quantize_method`, `fits_set_dither_seed`, `fits_set_hcomp_scale`, `fits_set_hcomp_smooth`, `fits_img_compress`, `fits_img_decompress`, `fits_is_compressed_image`, `fits_compress_table`, `fits_uncompress_table` | `compression_options`, given to an image, a table or a whole save; compressed images and tables read as the images and tables that they hold; `writer::add_hdu()` |
| `fits_copy_file`, `fits_copy_hdu`, `fits_copy_header`, `fits_copy_data`, `fits_delete_hdu`, `fits_write_hdu` | `writer::add_hdu()`, `editor::append()`, `insert_hdu()`, `erase_hdu()` |
| `.gz` files read and written, files in memory | `gzip_source`, `gzip_sink`, `write_options::gzip_level`, `memory_source`, `memory_sink` |
| Extended file names `file.fits[3]`, `[SCI,2]`, `[*,100:200]` | `file_specification::parse()`, whose result `reader` and `editor` take; `adjust_for_section()` |
| `fits_report_error`, `fits_get_errstatus`, `fits_read_errmsg` | Exceptions derived from `openfits::error`, each with an `errc` and a context; the diagnostics of units and readers |
| Data display formats, `TDISPn` | `display_format::parse()`, `format_display()`, `reader::read_strings()` |
| `fitsverify` | `verify()` |

The differences of behaviour that a migration meets, each on purpose:

- A path is the name of a file, brackets included; a file specification is parsed only by `file_specification::parse()`.
- A value converts to the type asked for only without a loss: a logical is no number, a real with a fraction is no
  integer, and a value beyond the type is refused (CFITSIO's `OVERFLOW_ERR`), or clamped and counted on request.
- Reads into integers truncate toward zero, as CFITSIO's do, or round half away from zero on request; writes into
  integers round half away from zero, as CFITSIO's do.
- Floating-point values are read as they are stored: negative zeros and subnormals stay, and an infinity is a value, not
  an undefined one. `' '` is a string of one space.
- A file that departs from the standard in ways that leave its meaning clear is read, with a diagnostic for each
  departure, where CFITSIO refuses some of them: a header without `END`, a short last block, `SIMPLE = 'T'`.
- There is no status variable and no global state: each call throws or completes, and a reader is used by any number
  of threads at once.

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
compressed tiles decode to exactly their declared size or fail; there is no network access. A header takes about four
times its bytes in memory once parsed, which `limits.h` details: lower the limits for a service that opens files from
unknown parties. An allocation that fails throws `std::bad_alloc` and leaves the reader, the writer and the editor as
they were, the changes of a commit pending, which tests that make each allocation fail in turn check. They are checked
continuously by fuzzing (the targets of `fuzz/`, which `fuzz/oss-fuzz/build.sh` builds as OSS-Fuzz does), sanitizers
and static analysis.

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
