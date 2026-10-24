#!/bin/bash
set -eu
root="$PWD/build/windows"
sdl="$root/deps/SDL2-2.32.10/x86_64-w64-mingw32"
export PKG_CONFIG_PATH="$root/prefix/lib/pkgconfig"
export PKG_CONFIG_LIBDIR=/usr/x86_64-w64-mingw32/sys-root/mingw/lib/pkgconfig
mkdir -p "$root/atv-receiver-windows-x64"
x86_64-w64-mingw32-windres -I scripts/windows scripts/windows/app.rc -o "$root/app-resource.o"
# Inline across DSP/PAL translation units without relaxing IEEE math or
# requiring instructions newer than the portable x64 baseline.
x86_64-w64-mingw32-gcc -O3 -flto -fno-math-errno -std=c11 -Wall -Wextra -Wpedantic -D_POSIX_C_SOURCE=200809L -DATV_BUILD_UTC=\"$(date -u +%Y-%m-%dT%H:%M:%SZ)\" -DATV_NO_UHD -DHAVE_LIBIIO -Isrc -I"$root/prefix/include" -I"$sdl/include/SDL2" $(pkg-config --cflags fftw3f libpng freetype2) "$root/app-resource.o" src/main.c src/video_quality.c src/status_server.c src/driver_status.c src/ui_text.c src/settings.c src/sound.c src/source.c src/source_pluto.c src/rx_queue.c src/afc.c src/alignment.c src/filter.c src/dsp.c src/pal.c src/stack.c src/nicam.c src/nicam_core/core.c -o "$root/atv-receiver-windows-x64/atv-rx.exe" -L"$root/prefix/lib" -L"$sdl/lib" -lmingw32 -lSDL2main -lSDL2 -lSoapySDR -liio $(pkg-config --libs fftw3f libpng freetype2) -lws2_32 -lm -pthread -mwindows
