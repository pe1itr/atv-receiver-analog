#!/bin/bash
# Fedora MinGW x64 cross-build. Run from any directory.
set -euo pipefail
cd "$(dirname "$0")/.."
stage='checking the Fedora cross-build environment'
trap 'echo "ERROR: Windows build failed while $stage. See INSTALL.md, section Windows x64 compileren op Fedora, for dependencies and recovery steps." >&2' ERR
if [[ ! -d /usr/x86_64-w64-mingw32/sys-root/mingw ]] || ! command -v rpm >/dev/null 2>&1; then
    echo 'ERROR: the Windows packaging route requires Fedora MinGW paths and RPM packages.' >&2
    echo 'On Debian, Ubuntu or Linux Mint, build the native Linux receiver with: bash scripts/build-linux.sh' >&2
    echo 'For a Windows ZIP, use the Fedora environment described in INSTALL.md or download the release ZIP.' >&2
    exit 1
fi
missing=0
for tool in make cmake pkg-config curl python3 tar gzip sha256sum x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++ x86_64-w64-mingw32-windres x86_64-w64-mingw32-objdump x86_64-w64-mingw32-strip; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: missing Windows build tool: $tool" >&2
        missing=1
    fi
done
packages=(mingw64-gcc mingw64-gcc-c++ mingw64-binutils mingw64-fftw mingw64-freetype
    mingw64-libpng mingw64-libxml2 mingw64-libusb1 mingw64-bzip2 mingw64-zlib
    mingw64-win-iconv mingw64-winpthreads mingw64-libstdc++ freetype
    google-noto-fonts-common google-noto-sans-mono-vf-fonts)
for package in "${packages[@]}"; do
    if ! rpm -q "$package" >/dev/null 2>&1; then
        echo "ERROR: missing Windows build/packaging package: $package" >&2
        missing=1
    fi
done
if ((missing)); then
    echo "Install dependencies first: sudo dnf install make cmake pkgconf-pkg-config curl python3 tar gzip ${packages[*]}" >&2
    exit 1
fi
if ! python3 -c 'import sys; sys.exit(sys.version_info < (3,12))'; then
    echo 'ERROR: Windows archive extraction requires Python 3.12 or newer. Update Python in the Fedora build environment.' >&2
    exit 1
fi
stage='downloading dependency archives (check internet access and the URL reported by curl)'
mkdir -p build/windows/deps
fetch() { test -f "build/windows/deps/$1" || curl -L --fail "$2" -o "build/windows/deps/$1"; }
fetch sdl.tar.gz https://www.libsdl.org/release/SDL2-devel-2.32.10-mingw.tar.gz
fetch soapy.tar.gz https://codeload.github.com/pothosware/SoapySDR/tar.gz/refs/tags/soapy-sdr-0.8.1
fetch soapyhackrf.tar.gz https://codeload.github.com/pothosware/SoapyHackRF/tar.gz/refs/tags/soapy-hackrf-0.3.4
fetch hackrf.tar.gz https://codeload.github.com/greatscottgadgets/hackrf/tar.gz/refs/tags/v2024.02.1
fetch iio.tar.gz https://codeload.github.com/analogdevicesinc/libiio/tar.gz/refs/tags/v0.26
fetch fftw.tar.gz https://www.fftw.org/fftw-3.3.10.tar.gz
stage='verifying SHA-256 checksums (remove only the archive reported as FAILED and retry; do not bypass verification)'
(cd build/windows/deps && sha256sum -c ../../../scripts/windows/deps.sha256)
stage='extracting dependency archives'
python3 - <<'PY'
import tarfile
from pathlib import Path
for archive in Path('build/windows/deps').glob('*.tar.gz'):
    with tarfile.open(archive) as src: src.extractall(archive.parent,filter='data')
# Upstream Windows header capitalization fails on a case-sensitive Linux host.
p=Path('build/windows/deps/libiio-0.26/utilities.c')
p.write_text(p.read_text().replace('<Windows.h>','<windows.h>'))
PY
stage='building Windows dependencies'
bash scripts/windows/build-deps.sh
stage='compiling the Windows receiver'
bash scripts/windows/compile.sh
stage='packaging DLLs, fonts and licenses'
python3 scripts/package-windows.py
echo 'Windows build successful: build/windows/atv-receiver-windows-x64.zip'
