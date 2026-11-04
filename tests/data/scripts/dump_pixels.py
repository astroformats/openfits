# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz
"""Writes, next to each sample given, <sample>.pixels: the values of the images of its units as astropy reads them,
scaled and with NaN for the undefined values of a scaled image, which the tests of the samples compare with the reader.
Run it with a Python that has astropy 8.0.1:

    python -I tests/data/scripts/dump_pixels.py tests/data/astropy/a1-u8.fits ...

Each unit that holds an image starts with a line of its index, the numpy type of its values and their number,
separated by tabs, followed by its values, one per line, in the order of the data: integers in decimal, and reals as the
shortest text that reads back as the same double, nan, inf and -inf included. A unit without an image has no lines.
"""
import sys
import warnings
from pathlib import Path

import astropy
import numpy as np
from astropy.io import fits


def dump_path(path: Path, suffix: str) -> Path:
    """The dump of a sample, whose name ends in .fits or .fit, maybe followed by .fz or .gz: b15-mef.fits.fz has
    b15-mef.cards, as the tests of the samples find it."""
    return path.with_name(path.name[:path.name.rfind('.fit')] + suffix)


def text(value) -> str:
    if isinstance(value, (np.floating, float)):
        return repr(float(value))
    return str(int(value))


def dump(path: Path):
    lines = []
    with warnings.catch_warnings():
        warnings.simplefilter('ignore')
        with fits.open(path) as units:
            for index, unit in enumerate(units):
                if not isinstance(unit, (fits.PrimaryHDU, fits.ImageHDU)) or unit.data is None:
                    continue
                values = np.asarray(unit.data).ravel()
                lines.append('\t'.join([str(index), values.dtype.name, str(values.size)]))
                lines.extend(text(value) for value in values)
    dump_path(path, '.pixels').write_bytes(('\n'.join(lines) + '\n').encode('ascii'))


def main():
    assert astropy.__version__ == '8.0.1', astropy.__version__
    for name in sys.argv[1:]:
        dump(Path(name))


main()
