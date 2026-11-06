# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz
"""Writes the ICC profile that the samples of Siril and PixInsight embed in their ICCProfile units: a display profile of
the gray color space, version 4.3, with the four tags that such a profile needs (ICC.1:2010, section 8.3): its
description, its copyright, the white point D50 and a gamma of 2.2. The samples embed a profile of this project, so that
the repository holds no profile of another; the date of the profile is fixed, and its identifier is the MD5 digest that
the specification defines, so that the file is the same at every run.

    python -I tests/data/scripts/make_icc.py tests/data/icc/openfits-gray.icc
"""
import hashlib
import struct
import sys
from pathlib import Path

DESCRIPTION = 'OpenFITS test gray, gamma 2.2'
COPYRIGHT = 'No copyright, use freely'


def s15fixed16(value: float) -> bytes:
    return struct.pack('>i', round(value * 65536))


def xyz_number(x: float, y: float, z: float) -> bytes:
    return s15fixed16(x) + s15fixed16(y) + s15fixed16(z)


# The D50 illuminant of the profile connection space, as ICC.1:2010 section 7.2.16 gives it.
D50 = xyz_number(0.9642, 1.0, 0.8249)


def multi_localized(text: str) -> bytes:
    """A multiLocalizedUnicodeType of one record, en-US (section 10.15)."""
    encoded = text.encode('utf-16-be')
    return (b'mluc' + bytes(4) + struct.pack('>II', 1, 12) + b'enUS' + struct.pack('>II', len(encoded), 28) +
            encoded)


def tags() -> list[tuple[bytes, bytes]]:
    return [
        (b'desc', multi_localized(DESCRIPTION)),
        (b'cprt', multi_localized(COPYRIGHT)),
        (b'wtpt', b'XYZ ' + bytes(4) + D50),
        # A parametricCurveType of function type 0, Y = X^g (section 10.18).
        (b'kTRC', b'para' + bytes(4) + struct.pack('>HH', 0, 0) + s15fixed16(2.2)),
    ]


def pad(data: bytes) -> bytes:
    return data + bytes(-len(data) % 4)


def profile() -> bytes:
    entries = tags()
    table_size = 4 + 12 * len(entries)
    offset = 128 + table_size
    table = struct.pack('>I', len(entries))
    body = b''
    for signature, data in entries:
        table += signature + struct.pack('>II', offset + len(body), len(data))
        body += pad(data)
    size = 128 + table_size + len(body)
    header = (struct.pack('>I', size) + bytes(4) + struct.pack('>I', 0x04300000) + b'mntr' + b'GRAY' + b'XYZ ' +
              struct.pack('>6H', 2026, 1, 1, 0, 0, 0) + b'acsp' + bytes(4) + bytes(4) + bytes(4) + bytes(4) + bytes(8) +
              struct.pack('>I', 0) + D50 + bytes(4) + bytes(16) + bytes(28))
    assert len(header) == 128
    data = bytearray(header + table + body)
    # The profile identifier: the MD5 digest of the profile with the flags, the rendering intent and the identifier set
    # to zero (section 7.2.18), which they are here.
    data[84:100] = hashlib.md5(bytes(data)).digest()
    return bytes(data)


def main():
    output = Path(sys.argv[1])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(profile())
    print(output, len(profile()), 'bytes')


main()
