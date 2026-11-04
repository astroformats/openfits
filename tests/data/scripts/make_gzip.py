# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz
"""Writes the gzip samples D6 and D6b into tests/data/gzip, from the sample A13 of tests/data/astropy.

D6 is A13 compressed whole with gzip, a convention outside the standard; D6b is A13 as two gzip members, the primary
HDU in the first and its extensions in the second, which a reader that inflates every member joins. Python's gzip
module writes them at level 9, without a name and with a time of 0, so that each run with one version of zlib writes
the same bytes. tests/samples/sample_catalog.cpp describes the samples. Run it with any Python 3:

    python -I tests/data/scripts/make_gzip.py tests/data
"""
import gzip
import sys
from pathlib import Path

# The first block of A13 is the header of its primary HDU, which has no data.
PRIMARY_SIZE = 2880


def main(data: Path) -> None:
    plain = (data / 'astropy' / 'a13-mef.fits').read_bytes()
    output = data / 'gzip'
    output.mkdir(exist_ok=True)
    (output / 'd6-gzip.fits.gz').write_bytes(gzip.compress(plain, compresslevel=9, mtime=0))
    members = (gzip.compress(plain[:PRIMARY_SIZE], compresslevel=9, mtime=0) +
               gzip.compress(plain[PRIMARY_SIZE:], compresslevel=9, mtime=0))
    (output / 'd6b-gzip-members.fits.gz').write_bytes(members)


if __name__ == '__main__':
    main(Path(sys.argv[1]))
