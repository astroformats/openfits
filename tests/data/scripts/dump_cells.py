# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz
"""Writes, next to each sample given, <sample>.cells: the cells of its tables and the parameters and arrays of its
random groups as astropy reads them, scaled, which the tests of the samples compare with the reader. Run it with a
Python that has astropy 8.0.1:

    python -I tests/data/scripts/dump_cells.py tests/data/astropy/d2-bintable.fits ...

Each column of a table starts with a line of 'column', the index of its unit, its index from 0, its name and its
number of rows, separated by tabs, followed by a line for each row: its elements separated by tabs, a string as it is
without its trailing spaces, a logical or a bit as T or F, an integer in decimal, a real as the shortest text that
reads back as the same double, nan and inf included, and a complex value as its real and imaginary parts. Random
groups start with a line of 'groups', the index of the unit, the number of groups and of parameters, followed by two
lines for each group, its parameters and its array, then by a line of 'parameter', a name and the summed value of the
parameters of that name for each group.
"""
import sys
import warnings
from pathlib import Path

import astropy
import numpy as np
from astropy.io import fits


def element(value) -> str:
    if isinstance(value, (bool, np.bool_)):
        return 'T' if value else 'F'
    if isinstance(value, (complex, np.complexfloating)):
        return repr(float(value.real)) + '\t' + repr(float(value.imag))
    if isinstance(value, (np.floating, float)):
        return repr(float(value))
    if isinstance(value, (np.integer, int)):
        return str(int(value))
    return str(value).rstrip(' ')


def cell(value) -> str:
    if isinstance(value, (str, bytes, np.str_, np.bytes_)):
        return value.decode('ascii').rstrip(' ') if isinstance(value, bytes) else str(value).rstrip(' ')
    array = np.asarray(value)
    if array.dtype.kind in 'US':
        # An array of characters, a variable-length one among them, is a string.
        return ''.join(str(item) for item in array.ravel()).rstrip(' ')
    if array.ndim == 0:
        return element(array.item() if array.dtype.kind != 'U' else str(array))
    return '\t'.join(element(item) for item in array.ravel())


def dump(path: Path):
    lines = []
    with warnings.catch_warnings():
        warnings.simplefilter('ignore')
        with fits.open(path) as units:
            for index, unit in enumerate(units):
                if isinstance(unit, fits.GroupsHDU):
                    groups = unit.data
                    lines.append('\t'.join(['groups', str(index), str(len(groups)), str(len(groups.parnames))]))
                    for g in range(len(groups)):
                        lines.append('\t'.join(element(groups.par(p)[g]) for p in range(len(groups.parnames))))
                        lines.append('\t'.join(element(v) for v in np.asarray(groups.data[g]).ravel()))
                    for name in dict.fromkeys(groups.parnames):
                        lines.append('\t'.join(['parameter', name] + [element(v) for v in groups.par(name)]))
                elif isinstance(unit, (fits.BinTableHDU, fits.TableHDU)):
                    for column, name in enumerate(unit.columns.names):
                        values = unit.data[name]
                        lines.append('\t'.join(['column', str(index), str(column), name, str(len(values))]))
                        lines.extend(cell(values[row]) for row in range(len(values)))
    path.with_suffix('.cells').write_bytes(('\n'.join(lines) + '\n').encode('ascii'))


def main():
    assert astropy.__version__ == '8.0.1', astropy.__version__
    for name in sys.argv[1:]:
        dump(Path(name))


main()
