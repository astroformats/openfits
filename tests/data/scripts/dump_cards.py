# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz
"""Writes, next to each sample given, <sample>.cards: the cards of its headers as astropy reads them, which the tests of
the samples compare with the reader card by card. Run it with a Python that has astropy 8.0.1:

    python -I tests/data/scripts/dump_cards.py tests/data/astropy/*.fits tests/data/capture/*.fits ...

Each line is one card, CONTINUE records joined into the card they continue: the index of its unit, the keyword, the
type of the value (bool, int, float, complex, str, undefined, or commentary for COMMENT, HISTORY and the blank keyword),
the value, and the comment, separated by tabs. Integers are written in decimal, reals as the shortest text that reads
back as the same double, complex values as their two parts, and the bytes of a header that are not ASCII as \\xHH.
"""
import sys
import warnings
from pathlib import Path

import astropy
import numpy as np
from astropy.io import fits


def text(value: str) -> str:
    result = ''
    for c in value:
        result += c if ' ' <= c <= '~' and c != '\\' else f'\\x{ord(c):02X}'
    return result


def value_of(card):
    value = card.value
    if card.keyword in ('COMMENT', 'HISTORY', ''):
        return 'commentary', str(value)
    if isinstance(value, fits.card.Undefined):
        return 'undefined', ''
    if isinstance(value, (bool, np.bool_)):
        return 'bool', 'T' if value else 'F'
    if isinstance(value, (int, np.integer)):
        return 'int', str(int(value))
    if isinstance(value, (float, np.floating)):
        return 'float', repr(float(value))
    if isinstance(value, complex):
        return 'complex', f'{value.real!r},{value.imag!r}'
    if isinstance(value, str):
        return 'str', value
    raise TypeError(f'{card.keyword}: {type(value)}')


def dump(path: Path):
    lines = []
    with warnings.catch_warnings():
        warnings.simplefilter('ignore')
        with fits.open(path) as units:
            for index, unit in enumerate(units):
                for card in unit.header.cards:
                    kind, value = value_of(card)
                    lines.append('\t'.join([str(index), text(card.keyword), kind, text(value), text(card.comment)]))
    path.with_suffix('.cards').write_bytes(('\n'.join(lines) + '\n').encode('ascii'))


def main():
    assert astropy.__version__ == '8.0.1', astropy.__version__
    for name in sys.argv[1:]:
        dump(Path(name))
        print('dumped', name)


main()
