#!/bin/bash
set -eu
root="$PWD/build/windows"
export PKG_CONFIG_PATH=
export PKG_CONFIG_LIBDIR="/usr/x86_64-w64-mingw32/sys-root/mingw/lib/pkgconfig"
configure() { cmake -S "$root/deps/$1" -B "$root/$2" -DCMAKE_TOOLCHAIN_FILE="$PWD/scripts/windows/toolchain.cmake" -DCMAKE_INSTALL_PREFIX="$root/prefix" -DCMAKE_BUILD_TYPE=Release "${@:3}"; cmake --build "$root/$2" -j8; cmake --install "$root/$2"; }
configure SoapySDR-soapy-sdr-0.8.1 soapy-build -DENABLE_PYTHON=OFF -DENABLE_PYTHON3=OFF
configure hackrf-2024.02.1/host/libhackrf hackrf-build -DLIBUSB_INCLUDE_DIR=/usr/x86_64-w64-mingw32/sys-root/mingw/include/libusb-1.0 -DLIBUSB_LIBRARIES=/usr/x86_64-w64-mingw32/sys-root/mingw/lib/libusb-1.0.dll.a
configure SoapyHackRF-soapy-hackrf-0.3.4 soapyhackrf-build
configure libiio-0.26 iio-build -DWITH_TESTS=OFF -DWITH_EXAMPLES=OFF -DHAVE_DNS_SD=OFF -DWITH_LOCAL_BACKEND=OFF -DWITH_IIOD=OFF
# The distro MinGW FFTW DLL is scalar. SSE2 is part of the x64 baseline;
# avoid global AVX flags so the portable build also runs on older x64 CPUs.
# Upstream 3.3.10's CMake file still reports 3.3.9; fix the build provenance.
python3 - <<'PY'
from pathlib import Path
p=Path('build/windows/deps/fftw-3.3.10/CMakeLists.txt')
p.write_text(p.read_text().replace('set (FFTW_VERSION 3.3.9)', 'set (FFTW_VERSION 3.3.10)'))
PY
configure fftw-3.3.10 fftw-build -DBUILD_SHARED_LIBS=ON -DENABLE_FLOAT=ON -DENABLE_SSE2=ON -DBUILD_TESTS=OFF -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_C_FLAGS=-DWITH_OUR_MALLOC
