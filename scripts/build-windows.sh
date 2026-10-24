#!/bin/bash
# Fedora MinGW x64 cross-build. Run from any directory.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/windows/deps
fetch() { test -f "build/windows/deps/$1" || curl -L --fail "$2" -o "build/windows/deps/$1"; }
fetch sdl.tar.gz https://www.libsdl.org/release/SDL2-devel-2.32.10-mingw.tar.gz
fetch soapy.tar.gz https://codeload.github.com/pothosware/SoapySDR/tar.gz/refs/tags/soapy-sdr-0.8.1
fetch soapyhackrf.tar.gz https://codeload.github.com/pothosware/SoapyHackRF/tar.gz/refs/tags/soapy-hackrf-0.3.4
fetch hackrf.tar.gz https://codeload.github.com/greatscottgadgets/hackrf/tar.gz/refs/tags/v2024.02.1
fetch iio.tar.gz https://codeload.github.com/analogdevicesinc/libiio/tar.gz/refs/tags/v0.26
fetch fftw.tar.gz https://www.fftw.org/fftw-3.3.10.tar.gz
(cd build/windows/deps && sha256sum -c ../../../scripts/windows/deps.sha256)
python3 - <<'PY'
import tarfile
from pathlib import Path
for archive in Path('build/windows/deps').glob('*.tar.gz'):
    with tarfile.open(archive) as src: src.extractall(archive.parent,filter='data')
# Upstream Windows header capitalization fails on a case-sensitive Linux host.
p=Path('build/windows/deps/libiio-0.26/utilities.c')
p.write_text(p.read_text().replace('<Windows.h>','<windows.h>'))
PY
bash scripts/windows/build-deps.sh
bash scripts/windows/compile.sh
python3 scripts/package-windows.py
