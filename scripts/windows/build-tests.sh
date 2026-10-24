#!/bin/bash
set -eu
root="$PWD/build/windows"
out="$root/tests"
mkdir -p "$out"
sdl="$root/deps/SDL2-2.32.10/x86_64-w64-mingw32"
export PKG_CONFIG_PATH="$root/prefix/lib/pkgconfig"
export PKG_CONFIG_LIBDIR=/usr/x86_64-w64-mingw32/sys-root/mingw/lib/pkgconfig
flags=(-O3 -flto -fno-math-errno -std=c11 -D_POSIX_C_SOURCE=200809L -DSDL_MAIN_HANDLED -DATV_NO_UHD -DHAVE_LIBIIO -Isrc -I"$root/prefix/include" -I"$sdl/include/SDL2")
x86_64-w64-mingw32-gcc "${flags[@]}" tests/test_settings.c src/settings.c -o "$out/test-settings.exe" -lm
x86_64-w64-mingw32-gcc "${flags[@]}" tests/test_fm_math.c -o "$out/test-fm-math.exe" -lm
x86_64-w64-mingw32-gcc "${flags[@]}" tests/test_pal.c src/pal.c -o "$out/test-pal.exe" -lm
x86_64-w64-mingw32-gcc "${flags[@]}" tests/test_fm.c src/dsp.c src/pal.c -o "$out/test-fm.exe" -lm
x86_64-w64-mingw32-gcc "${flags[@]}" -DFM1_NO_SIMD tests/test_fm.c src/dsp.c src/pal.c -o "$out/test-fm-scalar.exe" -lm
x86_64-w64-mingw32-gcc "${flags[@]}" tests/test_sound.c src/sound.c src/filter.c src/dsp.c src/pal.c -o "$out/test-sound.exe" $(pkg-config --libs fftw3f) -lm
x86_64-w64-mingw32-gcc "${flags[@]}" tests/test_windows_drivers.c src/driver_status.c -o "$out/test-drivers.exe" -L"$root/prefix/lib" -L"$sdl/lib" -lSDL2 -lSoapySDR -liio
x86_64-w64-mingw32-gcc "${flags[@]}" tests/test_dsp.c src/filter.c src/dsp.c src/pal.c -o "$out/test-dsp.exe" $(pkg-config --libs fftw3f) -lm
x86_64-w64-mingw32-gcc "${flags[@]}" tests/bench_if_filter.c src/filter.c -o "$out/bench-if-filter.exe" -L"$sdl/lib" -lSDL2 $(pkg-config --libs fftw3f) -lm
x86_64-w64-mingw32-gcc "${flags[@]}" tests/bench_pipeline.c src/test_signal.c src/sound.c src/nicam.c src/nicam_core/core.c src/alignment.c src/filter.c src/dsp.c src/pal.c -o "$out/bench-pipeline.exe" -L"$sdl/lib" -lSDL2 $(pkg-config --libs fftw3f) -lm
x86_64-w64-mingw32-gcc "${flags[@]}" tests/test_nicam.c src/nicam.c src/nicam_core/core.c src/sound.c src/filter.c src/dsp.c src/pal.c -o "$out/test-nicam.exe" -L"$sdl/lib" -lSDL2 $(pkg-config --libs fftw3f) -lm

x86_64-w64-mingw32-gcc "${flags[@]}" tests/test_video_quality.c src/video_quality.c -o "$out/test-video-quality.exe" -lm
