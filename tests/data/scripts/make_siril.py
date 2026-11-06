# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz
"""Writes the samples of Siril into tests/data/siril: the pattern image saved by Siril as 32-bit floats (E3), the same
image saved compressed with RICE_1 (E5), a FITSEQ sequence of three frames of the pattern (E4), and a synthetic CFA frame
with its Bayer keywords (E6). E3 and E5 have the ICC profile of tests/data/icc assigned, a profile of this project, which
Siril writes as an ICCProfile unit after the image, and the ROWORDER of their input. astropy writes the inputs into a
working directory, and siril-cli loads and saves them there with make_siril.ssf, so that the samples have the headers
that Siril writes. Run it with a Python that has astropy:

    python -I tests/data/scripts/make_siril.py tests/data/siril <working directory>

tests/samples/sample_catalog.cpp describes the samples.
"""
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np
from astropy.io import fits

SIRIL = Path(r'C:\Program Files\Siril\bin\siril-cli.exe')
WIDTH, HEIGHT = 37, 23


def ramp() -> np.ndarray:
    y, x = np.mgrid[0:HEIGHT, 0:WIDTH]
    return (x + WIDTH * y) / 850.0


def inputs(work: Path):
    # The order of the rows of the input, which Siril keeps and writes as ROWORDER.
    pattern = fits.PrimaryHDU(ramp().astype(np.float32))
    pattern.header['ROWORDER'] = ('TOP-DOWN', 'order of the rows')
    pattern.writeto(work / 'pattern-f32.fits', overwrite=True)
    frames = work / 'frames'
    frames.mkdir()
    for index in range(1, 4):
        fits.PrimaryHDU(ramp().astype(np.float32)).writeto(frames / f'frame_{index}.fits', overwrite=True)
    # A CFA frame: 16-bit values of the pattern with the Bayer pattern of its sensor.
    cfa = fits.PrimaryHDU(np.floor(ramp() * 65535 + 0.5).astype(np.uint16))
    cfa.header['BAYERPAT'] = ('RGGB', 'Bayer pattern of the sensor')
    cfa.header['XBAYROFF'] = (0, 'X offset of the Bayer pattern')
    cfa.header['YBAYROFF'] = (0, 'Y offset of the Bayer pattern')
    cfa.writeto(work / 'cfa.fits', overwrite=True)


def main():
    output = Path(sys.argv[1]).resolve()
    work = Path(sys.argv[2]).resolve()
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    output.mkdir(parents=True, exist_ok=True)
    inputs(work)
    shutil.copyfile(Path(__file__).parents[1] / 'icc' / 'openfits-gray.icc', work / 'openfits-gray.icc')
    script = Path(__file__).with_name('make_siril.ssf')
    result = subprocess.run([str(SIRIL), '-d', str(work), '-s', str(script)], capture_output=True, text=True,
                            check=False)
    (work / 'siril.log').write_text(result.stdout + result.stderr)
    if result.returncode != 0:
        sys.exit(f'siril-cli failed ({result.returncode}), see {work / "siril.log"}')
    for produced, sample in (('e3-siril-f32.fits', 'e3-siril-f32.fits'),
                             ('e5-siril-rice.fits.fz', 'e5-siril-rice.fits.fz'),
                             ('e6-siril-cfa.fits', 'e6-siril-cfa.fits'), ('frames/frame.fits', 'e4-siril-fitseq.fits')):
        shutil.copyfile(work / produced, output / sample)
    print('siril', result.stdout.splitlines()[0] if result.stdout else '')


main()
