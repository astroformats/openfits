# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz
"""Writes the samples of astropy into tests/data/astropy.

The pattern image is 37 x 23 pixels: channel 0 is (x + 37 y) / 850, channel 1 is 1 - (x + 37 y) / 850 and channel 2
is x / 36, scaled to the range of the type of the sample and rounded half away from zero for integers: n / 850 of the
range of an unsigned type is the integer nearest n x range / 850, computed exactly, and a signed type holds the value of
the unsigned type of its size minus the half range. A4 alone, the first sample, rounds in double. Floating-point
samples hold the double n / 850, or the float nearest it. tests/samples/sample_catalog.cpp describes the samples. Run
it with a Python that has astropy 8.0.1:

    python -I tests/data/scripts/make_astropy.py tests/data/astropy

dump_cards.py then writes what astropy reads of each sample, which the tests compare with the reader.
"""
import sys
from pathlib import Path

import astropy
import numpy as np
from astropy.io import fits

WIDTH, HEIGHT = 37, 23


def pattern(kind: str) -> np.ndarray:
    y, x = np.mgrid[0:HEIGHT, 0:WIDTH]
    ramp = (x + WIDTH * y) / 850.0
    if kind == 'f32':
        return ramp.astype(np.float32)
    if kind == 'u16':
        # Rounded half away from zero: the values are not negative.
        return np.floor(ramp * 65535 + 0.5).astype(np.uint16)
    raise ValueError(kind)


def index() -> np.ndarray:
    """n = x + 37 y of each pixel."""
    y, x = np.mgrid[0:HEIGHT, 0:WIDTH]
    return x + WIDTH * y


def exact(numerators, denominator: int, maximum: int) -> list:
    """The integers nearest numerator x maximum / denominator, halves up, exactly: the values are not negative."""
    return [(2 * int(n) * maximum + denominator) // (2 * denominator) for n in np.ravel(numerators)]


def integers(maximum: int, offset: int, dtype, channel: int = 0) -> np.ndarray:
    """A channel of the pattern scaled to [0, maximum], minus offset, as dtype."""
    n = index()
    if channel == 0:
        values = exact(n, 850, maximum)
    elif channel == 1:
        values = exact(850 - n, 850, maximum)
    else:
        values = exact(n % WIDTH, 36, maximum)
    return np.array([v - offset for v in values], dtype=dtype).reshape(HEIGHT, WIDTH)


def mask() -> np.ndarray:
    y, x = np.mgrid[0:7, 0:11]
    return ((x + 11 * y) % 256).astype(np.uint8)


def a4(directory: Path):
    hdu = fits.PrimaryHDU(pattern('u16'))
    hdu.header['OBJECT'] = ('pattern', 'the pattern image of the samples')
    hdu.writeto(directory / 'a4-u16.fits', overwrite=True)


def a13(directory: Path):
    primary = fits.PrimaryHDU()
    primary.header['ORIGIN'] = ('OpenFITS samples', 'an empty primary HDU')
    first = fits.ImageHDU(pattern('u16'), name='SCI', ver=1)
    first.header['FILTER'] = ('L', 'the first image')
    second = fits.ImageHDU(pattern('f32'), name='SCI', ver=2)
    second.header['GAIN'] = (1.25, '[e-/ADU] the second image')
    third = fits.ImageHDU(mask(), name='MASK')
    third.header['MASKED'] = (True, 'the mask')
    fits.HDUList([primary, first, second, third]).writeto(directory / 'a13-mef.fits', overwrite=True)


def a14(directory: Path):
    hdu = fits.PrimaryHDU()
    header = hdu.header
    header['TELESCOP'] = ('Invented 200', 'telescope')
    header['INSTRUME'] = ('Camera X', 'instrument')
    header['OBSERVER'] = ('Nobody', 'observer')
    header['OBJECT'] = ('M 31', 'object')
    header['DATE-OBS'] = ('2026-10-30T21:15:00.250', 'start of the exposure, UTC')
    header['EXPTIME'] = (300.0, '[s] exposure time')
    header['CCD-TEMP'] = (-10.5, '[degC] sensor temperature')
    header['XBINNING'] = (2, 'binning')
    header['RA'] = (10.684708, '[deg] right ascension')
    header['DEC'] = (41.26875, '[deg] declination')
    header['IMAGETYP'] = ('Light Frame', 'type of the frame')
    header['COMMENT'] = 'a header without data'
    header['HISTORY'] = 'written by astropy for the samples of OpenFITS'
    hdu.writeto(directory / 'a14-header-only.fits', overwrite=True)


def d1(directory: Path):
    hdu = fits.PrimaryHDU(pattern('u16'))
    header = hdu.header
    records = [
        "LOGICAL =                    T / a logical",
        "FALSE   =                    F",
        "INTMAX  =  9223372036854775807 / the largest signed 64-bit integer",
        "INTMIN  = -9223372036854775808 / the smallest one",
        "UINTMAX = 18446744073709551615 / the largest unsigned 64-bit integer",
        "REALE   =           1.2345E-10 / an E exponent",
        "REALD   =           6.0221D+23 / a D exponent",
        "REALFIX =                0.125",
        "CINT    =             (12, -7) / a complex integer",
        "CREAL   =        (1.5, -2.5E3) / a complex real",
        "UNDEF   =                      / an undefined value",
        "QUOTES  = 'O''Hara''s     '    / quotes and trailing spaces",
        "LEADING = '   leading'         / leading spaces are significant",
        "NULLSTR = ''                   / the null string",
        "EMPTY   = ' '                  / the empty string",
        "EXPTIME =                300.0 / [s] exposure time",
        "DATE    = '2026-10-31T10:00:00' / the date of the file",
        "DATE-OBS= '2026-10-30T21:15:00.250' / the start of the exposure",
        "WCSAXES =                    2",
        "CTYPE1  = 'RA---TAN'",
        "CTYPE2  = 'DEC--TAN'",
        "CRPIX1  =                 19.0",
        "CRPIX2  =                 12.0",
        "CRVAL1  =            10.684708",
        "CRVAL2  =             41.26875",
        "CDELT1  =           -2.778E-04 / [deg]",
        "CDELT2  =            2.778E-04 / [deg]",
        "CUNIT1  = 'deg     '",
        "CUNIT2  = 'deg     '",
        "WCSNAMEA= 'pixels  '           / an alternate description",
        "CTYPE1A = 'X       '",
        "CTYPE2A = 'Y       '",
        "CRPIX1A =                  1.0",
        "CRPIX2A =                  1.0",
        "CRVAL1A =                  0.0",
        "CRVAL2A =                  0.0",
        "INHERIT =                    F / a keyword of extensions, here as any",
        "LONGSTRN= 'OGIP 1.0'           / CONTINUE records may follow",
        "COMMENT   a comment",
        "HISTORY   a step",
        "          text of the blank keyword",
    ]
    for text in records:
        header.append(fits.Card.fromstring(text.ljust(80)), end=True)
    # A long string whose comment is continued over records of null strings (standard 4.2.1.2).
    pieces = ["LONGSTR = 'This string is longer than a record holds, and continues over &'",
              "CONTINUE  'CONTINUE records, which the standard defines since FITS 4.0.&'",
              "CONTINUE  '&' / Its comment is continued as well, on records",
              "CONTINUE  '' / of the null string."]
    assert all(len(piece) <= 80 for piece in pieces)
    header.append(fits.Card.fromstring(''.join(piece.ljust(80) for piece in pieces)), end=True)
    for text in ["HIERARCH ESO DET DIT = 1.5 / [s] a spaced name",
                 "HIERARCH ESO DET NDIT= 10 / without the space before the indicator",
                 "HIERARCH ESO INS FILT1 NAME = 'Ha      ' / a string",
                 "HIERARCH A LONGER HIERARCHICAL NAME OF THE CONVENTION = T"]:
        header.append(fits.Card.fromstring(text.ljust(80)), end=True)
    for _ in range(40):
        header.append(fits.Card(), end=True, useblanks=False)
    hdu.writeto(directory / 'd1-rich-header.fits', overwrite=True, output_verify='silentfix')


def write_image(directory: Path, name: str, data: np.ndarray, description: str):
    hdu = fits.PrimaryHDU(data)
    hdu.header['OBJECT'] = ('pattern', description)
    hdu.writeto(directory / name, overwrite=True)


def a1(directory: Path):
    write_image(directory, 'a1-u8.fits', integers(255, 0, np.uint8), 'the pattern as unsigned bytes')


def a2(directory: Path):
    write_image(directory, 'a2-s8.fits', integers(255, 128, np.int8), 'the pattern as signed bytes, BZERO -128')


def a3(directory: Path):
    write_image(directory, 'a3-i16.fits', integers(65535, 32768, np.int16), 'the pattern as 16-bit integers')


def a5(directory: Path):
    write_image(directory, 'a5-i32.fits', integers(2**32 - 1, 2**31, np.int32), 'the pattern as 32-bit integers')


def a6(directory: Path):
    write_image(directory, 'a6-u32.fits', integers(2**32 - 1, 0, np.uint32), 'the pattern as unsigned 32-bit integers')


def a7(directory: Path):
    write_image(directory, 'a7-i64.fits', integers(2**64 - 1, 2**63, np.int64), 'the pattern as 64-bit integers')


def a8(directory: Path):
    write_image(directory, 'a8-u64.fits', integers(2**64 - 1, 0, np.uint64), 'the pattern as unsigned 64-bit integers')


def a9(directory: Path):
    write_image(directory, 'a9-f32.fits', pattern('f32'), 'the pattern as 32-bit floating point')


def a10(directory: Path):
    data = (index() / 850.0).astype(np.float64)
    flat = data.reshape(-1)
    flat[1] = np.nan
    flat[2] = np.inf
    flat[3] = -0.0
    write_image(directory, 'a10-f64.fits', data, 'doubles; NaN, +inf and -0.0 at 1, 2 and 3')


def a11(directory: Path):
    # The stored values of A3; BLANK at pixels 0, which A3 holds as -32768 already, 400 and 850.
    stored = integers(65535, 32768, np.int16)
    flat = stored.reshape(-1)
    flat[400] = -32768
    flat[850] = -32768
    hdu = fits.PrimaryHDU(stored)
    hdu.header['BSCALE'] = 0.01
    hdu.header['BZERO'] = 100.0
    hdu.header['BLANK'] = -32768
    hdu.header['OBJECT'] = ('pattern', 'A3 scaled, BLANK at pixels 0, 400 and 850')
    hdu.writeto(directory / 'a11-scaled.fits', overwrite=True)


def a12(directory: Path):
    cube = np.stack([integers(65535, 0, np.uint16, channel) for channel in range(3)])
    hdu = fits.PrimaryHDU(cube)
    hdu.header['CTYPE3'] = ('RGB', 'the channels of a colour image')
    hdu.header['OBJECT'] = ('pattern', 'the three channels as unsigned 16-bit integers')
    hdu.writeto(directory / 'a12-rgb-cube.fits', overwrite=True)


def a15(directory: Path):
    # NAXIS1 = 5, the fastest axis, to NAXIS4 = 2: value i is the i-th of the data.
    write_image(directory, 'a15-4d.fits', np.arange(120, dtype=np.int16).reshape(2, 3, 4, 5),
                'a 5 x 4 x 3 x 2 cube, each value its index')


def main():
    directory = Path(sys.argv[1])
    directory.mkdir(parents=True, exist_ok=True)
    assert astropy.__version__ == '8.0.1', astropy.__version__
    for make in (a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, d1):
        make(directory)
    print('astropy', astropy.__version__, 'numpy', np.__version__)


main()
