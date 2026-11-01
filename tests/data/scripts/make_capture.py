# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz
"""Writes the samples of group E7 into tests/data/capture: the headers that capture programs of amateur astronomy
write, rebuilt from their documented formats with invented values, over the 37 x 23 pattern as BITPIX 16 with
BZERO 32768. The bytes are written here, without a FITS library, since some of them are not ASCII.

    python -I tests/data/scripts/make_capture.py tests/data/capture

tests/samples/sample_catalog.cpp describes the samples.
"""
import struct
import sys
from pathlib import Path

WIDTH, HEIGHT = 37, 23


def record(text: str) -> bytes:
    data = text.encode('latin-1')
    assert len(data) <= 80, text
    return data.ljust(80, b' ')


def card(keyword: str, value: str, comment: str = '') -> bytes:
    text = keyword.ljust(8) + '= '
    text += value if value.startswith("'") else value.rjust(20)
    if comment:
        text += ' / ' + comment
    return record(text)


def string(text: str) -> str:
    return "'" + text.replace("'", "''").ljust(8) + "'"


def pattern_data() -> bytes:
    values = []
    for y in range(HEIGHT):
        for x in range(WIDTH):
            value = int((x + WIDTH * y) / 850.0 * 65535 + 0.5)
            values.append(value - 32768)
    data = struct.pack(f'>{len(values)}h', *values)
    return data + b'\0' * ((2880 - len(data) % 2880) % 2880)


def mandatory() -> list:
    return [card('SIMPLE', 'T'), card('BITPIX', '16'), card('NAXIS', '2'), card('NAXIS1', str(WIDTH)),
            card('NAXIS2', str(HEIGHT)), card('BZERO', '32768'), card('BSCALE', '1')]


def write(path: Path, records: list):
    header = b''.join(records) + record('END')
    header += b' ' * ((2880 - len(header) % 2880) % 2880)
    path.write_bytes(header + pattern_data())


def sbfitsext(directory: Path):
    # The keywords of the SBFITSEXT 1.0 convention of SBIG, which MaxIm DL and others write.
    write(directory / 'e7-capture-sbfitsext.fits', mandatory() + [
        card('EXTEND', 'T'),
        card('OBJECT', string('NGC 7000')),
        card('TELESCOP', string('Invented 80 ED')),
        card('INSTRUME', string('Invented CCD')),
        card('OBSERVER', string('Nobody')),
        card('DATE-OBS', string('2026-08-14T22:10:05')),
        card('EXPTIME', '600.00000000000000', 'Exposure time in seconds'),
        card('EXPOSURE', '600.00000000000000', 'Exposure time in seconds'),
        card('SET-TEMP', '-15.000000000000000', 'CCD temperature setpoint in C'),
        card('CCD-TEMP', '-14.949999999999999', 'CCD temperature at start of exposure in C'),
        card('XPIXSZ', '9.0000000000000000', 'Pixel Width in microns (after binning)'),
        card('YPIXSZ', '9.0000000000000000', 'Pixel Height in microns (after binning)'),
        card('XBINNING', '1', 'Binning factor in width'),
        card('YBINNING', '1', 'Binning factor in height'),
        card('XORGSUBF', '0', 'Subframe X position in binned pixels'),
        card('YORGSUBF', '0', 'Subframe Y position in binned pixels'),
        card('READOUTM', string('Normal'), 'Readout mode of image'),
        card('FILTER', string('Ha'), 'Filter used when taking image'),
        card('IMAGETYP', string('Light Frame'), 'Type of image'),
        card('FOCALLEN', '560.00000000000000', 'Focal length of telescope in mm'),
        card('APTDIA', '80.000000000000000', 'Aperture diameter of telescope in mm'),
        card('APTAREA', '5026.5482457436692', 'Aperture area of telescope in mm^2'),
        card('EGAIN', '1.3999999761581421', 'Electronic gain in e-/ADU'),
        card('SBSTDVER', string('SBFITSEXT Version 1.0'), 'SBIG FITS extensions ver'),
        card('SWCREATE', string('Invented Capture 6.0'), 'Name of software that created the image'),
        card('SITELAT', string('-34 36 00'), 'Latitude of the imaging location'),
        card('SITELONG', string('-58 22 00'), 'Longitude of the imaging location'),
        card('JD', '2461267.4236689815', 'Julian Date at start of exposure'),
        card('OBJCTRA', string('20 59 17'), 'Nominal object right ascension'),
        card('OBJCTDEC', string('+44 31 44'), 'Nominal object declination'),
        card('AIRMASS', '1.8213460000000001', 'Relative optical path length through atmosphere'),
        card('PEDESTAL', '-100', 'Correction to add for zero-based ADU'),
        card('SWOWNER', string('Nobody'), 'Licensed owner of software'),
    ])


def roworder(directory: Path):
    # A colour camera: the Bayer pattern, its offsets and the order of the rows, as capture programs write them.
    write(directory / 'e7-capture-roworder.fits', mandatory() + [
        card('EXTEND', 'T'),
        card('IMAGETYP', string('LIGHT'), 'Type of exposure'),
        card('EXPOSURE', '120.0', '[s] Exposure duration'),
        card('EXPTIME', '120.0', '[s] Exposure duration'),
        card('DATE-LOC', string('2026-08-14T19:12:44.123'), 'Time of observation (local)'),
        card('DATE-OBS', string('2026-08-14T22:12:44.123'), 'Time of observation (UTC)'),
        card('XBINNING', '1', 'X axis binning factor'),
        card('YBINNING', '1', 'Y axis binning factor'),
        card('GAIN', '100', 'Sensor gain'),
        card('OFFSET', '30', 'Sensor gain offset'),
        card('EGAIN', '0.25', '[e-/ADU] Electrons per A/D unit'),
        card('XPIXSZ', '3.76', '[um] Pixel X axis size'),
        card('YPIXSZ', '3.76', '[um] Pixel Y axis size'),
        card('INSTRUME', string('Invented Colour 2600'), 'Imaging instrument name'),
        card('SET-TEMP', '-10.0', '[degC] CCD temperature setpoint'),
        card('CCD-TEMP', '-10.1', '[degC] CCD temperature'),
        card('READOUTM', string('Normal'), 'Sensor readout mode'),
        card('BAYERPAT', string('RGGB'), 'Sensor Bayer pattern'),
        card('XBAYROFF', '0', 'Bayer pattern X axis offset'),
        card('YBAYROFF', '1', 'Bayer pattern Y axis offset'),
        card('USBLIMIT', '40', 'Camera-specific USB setting'),
        card('TELESCOP', string('Invented 72 APO'), 'Name of telescope'),
        card('FOCALLEN', '432.0', '[mm] Focal length'),
        card('FOCRATIO', '6.0', 'Focal ratio'),
        card('RA', '314.8208333', '[deg] RA of telescope'),
        card('DEC', '44.5288889', '[deg] Declination of telescope'),
        card('CENTALT', '38.12', '[deg] Altitude of telescope'),
        card('CENTAZ', '31.40', '[deg] Azimuth of telescope'),
        card('SITEELEV', '25.0', '[m] Observation site elevation'),
        card('SITELAT', '-34.6', '[deg] Observation site latitude'),
        card('SITELONG', '-58.36666', '[deg] Observation site longitude'),
        card('OBJECT', string('NGC 7000'), 'Name of the object of interest'),
        card('OBJCTRA', string('20 59 17'), '[H M S] RA of imaged object'),
        card('OBJCTDEC', string('+44 31 44'), '[D M S] Declination of imaged object'),
        card('FILTER', string('L-eXtreme'), 'Active filter name'),
        card('FOCPOS', '12345', '[step] Focuser position'),
        card('FOCTEMP', '14.25', '[degC] Focuser temperature'),
        card('ROWORDER', string('TOP-DOWN'), 'FITS Image Orientation'),
        card('EQUINOX', '2000.0', 'Equinox of celestial coordinate system'),
        card('SWCREATE', string('Invented Imaging 3.1'), 'Software that created this file'),
    ])


def latin1(directory: Path):
    # Comments with the degree sign of Latin-1, byte 0xB0, which some programs write, and a string with an accent.
    write(directory / 'e7-capture-latin1.fits', mandatory() + [
        card('INSTRUME', string('Invented Cam'), 'camera'),
        card('EXPTIME', '30.0', 'exposure in seconds'),
        card('CCD-TEMP', '-5.3', 'sensor temperature in \xb0C'),
        card('SITELAT', '-34.6', 'latitude in \xb0'),
        card('OBSERVER', string('Mar\xeda'), 'observer'),
        card('ROWORDER', string('BOTTOM-UP'), 'order of the rows'),
        card('SWCREATE', string('Invented Live 4'), 'software'),
    ])


def main():
    directory = Path(sys.argv[1])
    directory.mkdir(parents=True, exist_ok=True)
    for make in (sbfitsext, roworder, latin1):
        make(directory)


main()
