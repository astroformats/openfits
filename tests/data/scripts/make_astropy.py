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


def replace_card(header: bytearray, keyword: str, text: str):
    """Replaces the record of keyword in the blocks of a header with text, padded to 80 characters."""
    for start in range(0, len(header), 80):
        if header[start:start + 8].decode('ascii').rstrip() == keyword:
            header[start:start + 80] = text.ljust(80).encode('ascii')
            return
    raise KeyError(keyword)


def insert_card(header: bytearray, before: str, text: str):
    """Inserts text as a record before the record of keyword before, the header keeping its blocks."""
    for start in range(0, len(header), 80):
        if header[start:start + 8].decode('ascii').rstrip() == before:
            if header[-80:] != b' ' * 80:
                header.extend(b' ' * 2880)
            header[start:start] = text.ljust(80).encode('ascii')
            del header[-80:]
            return
    raise KeyError(before)


def d2(directory: Path):
    """A binary table of every TFORMn type, five rows; THEAP and a gap of 104 bytes before the heap set by hand."""
    rows = 5
    rng = np.random.default_rng(2)
    bits = rng.integers(0, 2, size=(rows, 13)).astype(bool)
    columns = [
        fits.Column(name='FLAG', format='L', array=np.array([True, False, True, True, False])),
        fits.Column(name='BITS', format='13X', array=bits),
        fits.Column(name='UBYTE', format='B', array=np.array([0, 1, 128, 254, 255], dtype=np.uint8)),
        fits.Column(name='SBYTE', format='B', bzero=-128, array=np.array([-128, -1, 0, 1, 127], dtype=np.int8)),
        fits.Column(name='SHORT', format='I', array=np.array([-32768, -1, 0, 1, 32767], dtype=np.int16)),
        fits.Column(name='USHORT', format='I', bzero=32768, array=np.array([0, 1, 32768, 65534, 65535], dtype=np.uint16)),
        fits.Column(name='INT', format='J', array=np.array([-2**31, -7, 0, 7, 2**31 - 1], dtype=np.int32)),
        fits.Column(name='UINT', format='J', bzero=2**31, array=np.array([0, 1, 2**31, 2**32 - 2, 2**32 - 1], dtype=np.uint32)),
        fits.Column(name='LONG', format='K', array=np.array([-2**63, -1, 0, 1, 2**63 - 1], dtype=np.int64)),
        fits.Column(name='ULONG', format='K', bzero=2**63, array=np.array([0, 1, 2**63, 2**64 - 2, 2**64 - 1], dtype=np.uint64)),
        fits.Column(name='FLOAT', format='E', array=np.array([1.5, -0.0, np.nan, np.inf, 1e-38], dtype=np.float32)),
        fits.Column(name='DOUBLE', format='D', unit='deg', disp='F12.5',
                    array=np.array([10.684708, -41.26875, 0.1, 1e300, -5e-324])),
        fits.Column(name='NAME', format='8A', array=np.array(['M 31', 'M 33', '', 'NGC 6992', 'IC 434'])),
        fits.Column(name='CPLX', format='C', array=np.array([1 + 2j, -1.5j, 3, 0, -2 - 2j], dtype=np.complex64)),
        fits.Column(name='DCPLX', format='M', array=np.array([1e100 + 1j, 0.1 - 0.2j, 0, 1j, -1], dtype=np.complex128)),
        fits.Column(name='MATRIX', format='6I', dim='(3,2)', array=np.arange(30, dtype=np.int16).reshape(rows, 2, 3)),
        fits.Column(name='EMPTY', format='0J', array=np.zeros((rows, 0), dtype=np.int32)),
        # Stored as 100 + 0.25 x stored: 100, 100.25, 0, -50.5 and 1e6; TSCAL18 and TZERO18 are set by hand below, since
        # astropy 8.0.1 refuses to write the physical values of a scaled column of integers.
        fits.Column(name='SCALED', format='J', array=np.array([0, 1, -400, -602, 3999600], dtype=np.int32)),
        fits.Column(name='NULLED', format='J', null=-999, array=np.array([1, -999, 3, -999, 5], dtype=np.int32)),
        fits.Column(name='VLA_P', format='PJ()', array=np.array(
            [np.array([1, 2, 3]), np.array([], dtype=np.int32), np.array([4]), np.array([5, 6]), np.array([7, 8, 9, 10])],
            dtype=object)),
        fits.Column(name='VLA_Q', format='QD()', array=np.array(
            [np.array([0.5]), np.array([1.5, 2.5]), np.array([], dtype=np.float64), np.array([3.5]), np.array([4.5])],
            dtype=object)),
        fits.Column(name='WORDS', format='12A4', array=np.array(['abcdefghijkl', 'mnop', 'qrstuvwx', '', 'yz'])),
        fits.Column(name='NOTES', format='PA()', array=np.array(['first', '', 'third note', 'x', 'the fifth'],
                                                               dtype=object)),
    ]
    table = fits.BinTableHDU.from_columns(columns, name='CATALOG')
    table.header['OBJECT'] = ('every TFORMn type', 'the sample D2')
    path = directory / 'd2-bintable.fits'
    fits.HDUList([fits.PrimaryHDU(), table]).writeto(path, overwrite=True)
    # THEAP with a gap before the heap (standard §7.3.5), by hand: the descriptors stay relative to the heap.
    raw = bytearray(path.read_bytes())
    with fits.open(path) as units:
        header_start = 2880
        data_start = header_start + len(units[1].header.tostring())
        row_bytes = units[1].header['NAXIS1'] * units[1].header['NAXIS2']
        pcount = units[1].header['PCOUNT']
    gap = 104
    header = raw[header_start:data_start]
    replace_card(header, 'PCOUNT', f"PCOUNT  = {pcount + gap:>20} / size of special data area")
    insert_card(header, 'EXTNAME', f"THEAP   = {row_bytes + gap:>20} / heap after a gap")
    insert_card(header, 'EXTNAME', 'TSCAL18 =                 0.25')
    insert_card(header, 'EXTNAME', 'TZERO18 =                100.0')
    data = raw[data_start:data_start + row_bytes] + bytes(gap) + raw[data_start + row_bytes:data_start + row_bytes +
                                                                     pcount]
    data += bytes((-len(data)) % 2880)
    path.write_bytes(bytes(raw[:header_start]) + bytes(header) + bytes(data))


def d3(directory: Path):
    """An ASCII table: Aw, Iw, Fw.d, Ew.d and Dw.d fields, a TNULLn string, and by hand a blank field and a field
    without its decimal point."""
    columns = [
        fits.Column(name='NAME', format='A10', array=np.array(['M 31', 'M 33', 'NGC 6992', 'IC 434'])),
        fits.Column(name='COUNT', format='I6', null='*', array=np.array([42, -17, 0, 123456])),
        fits.Column(name='FLUX', format='F10.3', unit='Jy', array=np.array([12.5, -0.125, 1.0, 9999.999])),
        fits.Column(name='MAG', format='E12.5', array=np.array([3.44, -1.0e-10, 5.72e20, 0.0])),
        fits.Column(name='DIST', format='D20.12', array=np.array([778000.0, 1e-300, 2.73e1, -1.0])),
    ]
    table = fits.TableHDU.from_columns(columns, name='ASCII')
    path = directory / 'd3-ascii-table.fits'
    fits.HDUList([fits.PrimaryHDU(), table]).writeto(path, overwrite=True)
    raw = bytearray(path.read_bytes())
    with fits.open(path) as units:
        header = units[1].header
        data_start = 2880 + len(header.tostring())
        width = header['NAXIS1']
        count_column = header['TBCOL2'] - 1
        flux_column = header['TBCOL3'] - 1
    # The COUNT of row 2 blank, which is 0 (standard §7.2.5); its FLUX without a decimal point, 12345 read as 12.345;
    # the COUNT of row 3 the TNULLn string.
    row = data_start + (2 * width)
    raw[row + count_column:row + count_column + 6] = b'      '
    raw[row + flux_column:row + flux_column + 10] = b'     12345'
    row = data_start + (3 * width)
    raw[row + count_column:row + count_column + 6] = b'*     '
    path.write_bytes(bytes(raw))


def d4(directory: Path):
    """Random groups: three groups of a 7 x 5 array of 32-bit integers, each value 100 g plus its index, and four
    parameters, UU, VV and DATE twice, which are summed (standard §6.1.2). astropy 8.0.1 writes wrong stored values for
    parameters given with PSCALn and PZEROn, so the stored ones are given and PSCAL1 = 1E-9, PZERO3 = 2460000.5 and
    PSCAL4 = 0.001 set by hand: UU is 1e-9, -2e-9 and 3e-9, and DATE 2460000.625, 2460001.75 and 2460002.875."""
    groups = 3
    data = np.array([np.arange(35, dtype=np.int32).reshape(5, 7) + 100 * g for g in range(groups)],
                    dtype=np.int32).reshape(groups, 1, 5, 7)
    pardata = [np.array([1, -2, 3], dtype=np.int32), np.array([10, 20, 30], dtype=np.int32),
               np.array([0, 1, 2], dtype=np.int32), np.array([125, 250, 375], dtype=np.int32)]
    rows = fits.GroupData(data, parnames=['UU', 'VV', 'DATE', 'DATE'], pardata=pardata, bitpix=32)
    hdu = fits.GroupsHDU(rows)
    hdu.header['OBJECT'] = ('random groups', 'the sample D4')
    path = directory / 'd4-random-groups.fits'
    hdu.writeto(path, overwrite=True)
    raw = bytearray(path.read_bytes())
    header_size = len(fits.getheader(path).tostring())
    header = raw[:header_size]
    insert_card(header, 'OBJECT', 'PSCAL1  =                1E-09 / scale of UU')
    insert_card(header, 'OBJECT', 'PZERO3  =            2460000.5 / the day of the date')
    insert_card(header, 'OBJECT', 'PSCAL4  =                0.001 / the fraction of the day')
    path.write_bytes(bytes(header) + bytes(raw[header_size:]))


# The time of the checksums of the C samples, which astropy is given as their comment, so that every run writes the same
# bytes: astropy writes the time of the run otherwise.
CHECKSUM_TIME = '2026-11-02T00:00:00'


def with_checksums(source: Path, target: Path):
    """The units of source written to target with CHECKSUM and DATASUM in each, which astropy then verifies."""
    with fits.open(source, do_not_scale_image_data=True, memmap=False) as units:
        for unit in units:
            unit.add_checksum(when=CHECKSUM_TIME)
        units.writeto(target, overwrite=True, checksum=False)
    with fits.open(source) as original, fits.open(target, checksum=True) as units:
        assert len(units) == len(original), target
        for before, unit in zip(original, units):
            assert unit.verify_datasum() == 1 and unit.verify_checksum() == 1, target
            if before.data is None:
                assert unit.data is None, target
            else:
                assert np.array_equal(before.data, unit.data, equal_nan=True), target


def c1(directory: Path):
    with_checksums(directory / 'a4-u16.fits', directory / 'c1-u16-checksum.fits')


def c2(directory: Path):
    with_checksums(directory / 'a13-mef.fits', directory / 'c2-mef-checksum.fits')


def c4(directory: Path):
    """C1 with CHECKSUM and DATASUM blank strings, which assert nothing of the sums (standard §4.4.2.7). astropy 8.0.1
    takes a blank CHECKSUM for a mismatch and fails on a blank DATASUM, so that it does not verify this sample."""
    raw = bytearray((directory / 'c1-u16-checksum.fits').read_bytes())
    header = raw[:2880]
    replace_card(header, 'CHECKSUM', f"CHECKSUM= '{' ' * 16}'   / {CHECKSUM_TIME}")
    replace_card(header, 'DATASUM', f"DATASUM = '{' ' * 10}'         / {CHECKSUM_TIME}")
    (directory / 'c4-checksum-blank.fits').write_bytes(bytes(header) + bytes(raw[2880:]))


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
    for make in (a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, d1, d2, d3, d4, c1, c2, c4):
        make(directory)
    print('astropy', astropy.__version__, 'numpy', np.__version__)


main()
