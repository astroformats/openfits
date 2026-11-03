# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz
"""Writes the samples of fpack into tests/data/fpack, from the samples of astropy in tests/data/astropy.

Each sample is a sample of astropy compressed by fpack with the options of its row of COMMANDS, without checksums (-C),
whose comments would hold the time of the run, but C3, which keeps them. Two inputs are made here first, with astropy:
the floating-point pattern of A9 with exact zeros in its first column and a NaN, for the subtractive dithering that
keeps zeros, and a table of 100 rows of the column types of D2 for B16, since fpack leaves a table of one block, as D2
is, uncompressed; astropy reads no compressed table, so that the cells of B16 are those of that input, which
dump_cells.py writes from the directory of the inputs given as a third argument. fpack 1.7.0 refuses 64-bit integer images ("Bad image datatype"), so
that B14 is written by astropy with GZIP_2, the algorithm the writer of the library takes for them by default.
tests/samples/sample_catalog.cpp describes the samples. Run it with a Python that has astropy 8.0.1, and the directory
of fpack.exe:

    python -I tests/data/scripts/make_fpack.py tests/data E:/toolchains/cfitsio-tools/vcpkg_installed/x64-windows

dump_cards.py and dump_pixels.py then write what astropy reads of each sample, which the tests compare with the reader.
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from astropy.io import fits

# Sample file, input under tests/data/astropy or made here, and the options of fpack.
COMMANDS = [
    ('b1-u16-rice.fits', 'a4-u16.fits', ['-C', '-r']),
    ('b2-u16-gzip1.fits', 'a4-u16.fits', ['-C', '-g1']),
    ('b3-u16-gzip2.fits', 'a4-u16.fits', ['-C', '-g2']),
    ('b4-u16-plio.fits', 'a4-u16.fits', ['-C', '-p']),
    ('b5-u16-hcomp.fits', 'a4-u16.fits', ['-C', '-h']),
    ('b6-u16-rice-tiles.fits', 'a4-u16.fits', ['-C', '-r', '-t', '16,16']),
    ('b7-u16-nocompress.fits', 'a4-u16.fits', ['-C', '-d']),
    ('b8-f32-rice-q4.fits', 'a9-f32.fits', ['-C', '-r', '-q1', '4']),
    ('b9-f32-gzip-lossless.fits', 'a9-f32.fits', ['-C', '-g', '-q', '0']),
    ('b10-f32-rice-dither2.fits', 'zeros-f32.fits', ['-C', '-r', '-qz1', '-0.0001']),
    ('b11-f32-hcomp-q4.fits', 'a9-f32.fits', ['-C', '-h', '-q1', '-0.0001']),
    ('b12-u8-rice.fits', 'a1-u8.fits', ['-C', '-r']),
    ('b13-i32-rice.fits', 'a5-i32.fits', ['-C', '-r']),
    ('b15-mef.fits.fz', 'a13-mef.fits', ['-C']),
    ('b16-table.fits.fz', 'b16-input.fits', ['-C', '-table']),
    ('c3-rice-checksum.fits', 'c1-u16-checksum.fits', []),
]


def zeros_f32(path: Path):
    """The pattern of A9 with its first column 0, and a NaN at pixel (5, 7)."""
    y, x = np.mgrid[0:23, 0:37]
    values = ((x + 37 * y) / 850.0).astype(np.float32)
    values[:, 0] = 0.0
    values[7, 5] = np.nan
    unit = fits.PrimaryHDU(values)
    unit.header['OBJECT'] = ('pattern', 'the pattern with zeros and a NaN')
    unit.writeto(path, overwrite=True)


def b16_input(path: Path):
    """A binary table of the column types of D2 over 100 rows, its variable-length arrays included: fpack leaves a table
    of one block uncompressed, as D2 is."""
    rows = 100
    n = np.arange(rows)
    columns = [
        fits.Column(name='FLAG', format='L', array=n % 3 == 0),
        fits.Column(name='BITS', format='13X', array=np.array([[(i + k) % 3 == 0 for k in range(13)] for i in n])),
        fits.Column(name='UBYTE', format='B', array=(n * 7 % 256).astype(np.uint8)),
        fits.Column(name='SHORT', format='I', array=(n * 37 - 1000).astype(np.int16)),
        fits.Column(name='USHORT', format='I', bzero=32768, array=(n * 600).astype(np.uint16)),
        fits.Column(name='INT', format='J', array=(n * 100003 - 5000000).astype(np.int32)),
        fits.Column(name='LONG', format='K', array=n.astype(np.int64) * 10**12),
        fits.Column(name='FLOAT', format='E', array=(n / 7).astype(np.float32)),
        fits.Column(name='DOUBLE', format='D', array=n / 3),
        fits.Column(name='NAME', format='8A', array=np.array([f'row {i}' for i in n])),
        fits.Column(name='CPLX', format='C', array=(n + 1j * n / 2).astype(np.complex64)),
        fits.Column(name='DCPLX', format='M', array=n / 4 - 1j * n),
        fits.Column(name='MATRIX', format='6I', dim='(3,2)',
                    array=np.arange(rows * 6, dtype=np.int16).reshape(rows, 2, 3)),
        fits.Column(name='VLA_P', format='PJ()', array=[np.arange(i % 5, dtype=np.int32) * i for i in n]),
        fits.Column(name='VLA_Q', format='QD()', array=[np.arange(i % 3, dtype=np.float64) / (i + 1) for i in n]),
    ]
    table = fits.BinTableHDU.from_columns(columns, name='CATALOG')
    fits.HDUList([fits.PrimaryHDU(), table]).writeto(path, overwrite=True)


def b14(directory: Path, source: Path):
    """A7 compressed by astropy with GZIP_2, which fpack refuses."""
    with fits.open(source) as units:
        data = units[0].data
        header = units[0].header.copy()
    for keyword in ('SIMPLE', 'BITPIX', 'NAXIS', 'NAXIS1', 'NAXIS2', 'EXTEND'):
        header.remove(keyword, ignore_missing=True)
    compressed = fits.CompImageHDU(data, header, compression_type='GZIP_2')
    fits.HDUList([fits.PrimaryHDU(), compressed]).writeto(directory / 'b14-i64.fits', overwrite=True)


def main():
    data = Path(sys.argv[1])
    tools = Path(sys.argv[2])
    fpack = tools / 'tools' / 'cfitsio' / 'fpack.exe'
    environment = dict(os.environ, PATH=str(tools / 'bin') + os.pathsep + os.environ.get('PATH', ''))
    target = data / 'fpack'
    target.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory() as scratch:
        # The inputs made here, kept in the directory of a third argument when it is given.
        inputs = Path(sys.argv[3]) if len(sys.argv) > 3 else Path(scratch)
        inputs.mkdir(exist_ok=True)
        zeros_f32(inputs / 'zeros-f32.fits')
        b16_input(inputs / 'b16-input.fits')
        for name, source, options in COMMANDS:
            origin = inputs / source if (inputs / source).exists() else data / 'astropy' / source
            output = target / name
            output.unlink(missing_ok=True)
            subprocess.run([str(fpack), *options, '-O', str(output), str(origin)], check=True, env=environment)
        b14(target, data / 'astropy' / 'a7-i64.fits')


if __name__ == '__main__':
    main()
