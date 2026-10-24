CC ?= cc
PKGS = sdl2 fftw3f SoapySDR uhd libpng freetype2 fontconfig
# Match datv-transmitter: optional legacy libiio API. Override HAVE_LIBIIO=0 to disable.
HAVE_LIBIIO ?= $(shell pkg-config --exists 'libiio >= 0.23' 'libiio < 1.0' && echo 1)
ifeq ($(HAVE_LIBIIO),1)
PKGS += libiio
CPPFLAGS += -DHAVE_LIBIIO
endif
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Isrc $(shell pkg-config --cflags $(PKGS))
CFLAGS ?= -O3 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -fno-math-errno
RX_BUILD_FLAGS = -DATV_BUILD_UTC='"$(shell date -u +%Y-%m-%dT%H:%M:%SZ)"'
NICAM_SRC = src/nicam.c src/nicam_core/core.c
NICAM_DEPS = src/nicam.h $(wildcard src/nicam_core/*)
LDLIBS += $(shell pkg-config --libs $(PKGS)) -lm -pthread
.PHONY: all tools test bench clean
all: build/atv-rx
tools: build/atv-tx build/atv-eval
build:
	mkdir -p $@
build/atv-rx: src/video_quality.c src/status_server.c src/driver_status.c src/driver_status.h src/ui_text.c src/ui_text.h src/version.h Makefile src/main.c src/settings.c src/settings.h src/sound.c src/source.c src/source_pluto.c src/rx_queue.c src/afc.h src/afc.c src/alignment.c src/filter.c src/dsp.c src/pal.c src/stack.c src/atv.h src/pal.h src/rx_queue.h src/alignment.h src/sound.h src/source_pluto.h $(NICAM_SRC) $(NICAM_DEPS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(RX_BUILD_FLAGS) src/main.c src/video_quality.c src/status_server.c src/driver_status.c src/ui_text.c src/settings.c src/sound.c src/source.c src/source_pluto.c src/rx_queue.c src/afc.c src/alignment.c src/filter.c src/dsp.c src/pal.c src/stack.c $(NICAM_SRC) -o $@ $(LDLIBS)
build/atv-tx: src/atv_tx.c src/test_signal.c src/test_signal.h src/atv.h src/pal.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/atv_tx.c src/test_signal.c -o $@ -lm
build/atv-eval: src/atv_eval.c src/filter.c src/dsp.c src/pal.c src/atv.h src/pal.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/atv_eval.c src/filter.c src/dsp.c src/pal.c -o $@ -lfftw3f -lm
build/test-dsp: tests/test_dsp.c src/filter.c src/dsp.c src/pal.c src/atv.h src/pal.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_dsp.c src/filter.c src/dsp.c src/pal.c -o $@ -lfftw3f -lm
build/test-signal: tests/test_signal.c src/test_signal.c src/test_signal.h src/atv.h src/pal.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_signal.c src/test_signal.c -o $@ -lm
build/test-stack: tests/test_stack.c src/stack.c src/filter.c src/dsp.c src/pal.c src/atv.h src/pal.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_stack.c src/stack.c src/filter.c src/dsp.c src/pal.c -o $@ -lfftw3f -lm
build/test-registration: tests/test_registration.c src/test_signal.c src/stack.c src/filter.c src/dsp.c src/pal.c src/atv.h src/pal.h src/test_signal.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_registration.c src/test_signal.c src/stack.c src/filter.c src/dsp.c src/pal.c -o $@ -lfftw3f -lm
build/test-stack-worker: src/video_quality.c src/status_server.c src/driver_status.c src/driver_status.h src/ui_text.c src/ui_text.h src/version.h Makefile tests/test_stack_worker.c src/main.c src/settings.c src/settings.h src/sound.c src/source.c src/source_pluto.c src/rx_queue.c src/afc.h src/afc.c src/alignment.c src/filter.c src/dsp.c src/pal.c src/stack.c src/test_signal.c src/atv.h src/pal.h src/test_signal.h src/rx_queue.h src/alignment.h src/sound.h src/source_pluto.h $(NICAM_SRC) $(NICAM_DEPS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(RX_BUILD_FLAGS) tests/test_stack_worker.c src/video_quality.c src/status_server.c src/driver_status.c src/ui_text.c src/settings.c src/sound.c src/source.c src/source_pluto.c src/rx_queue.c src/afc.c src/alignment.c src/filter.c src/dsp.c src/pal.c src/stack.c src/test_signal.c $(NICAM_SRC) -o $@ $(LDLIBS)
build/test-rx-queue: tests/test_rx_queue.c src/rx_queue.c src/rx_queue.h src/atv.h src/pal.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_rx_queue.c src/rx_queue.c -o $@ $(LDLIBS)
build/test-alignment: tests/test_alignment.c src/alignment.c src/alignment.h src/filter.c src/dsp.c src/pal.c src/atv.h src/pal.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_alignment.c src/alignment.c src/filter.c src/dsp.c src/pal.c -o $@ $(LDLIBS)
build/test-pal: tests/test_pal.c src/pal.c src/pal.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_pal.c src/pal.c -o $@ -lm
test: build/test-video-deemphasis build/test-nicam-sensitivity build/test-nicam build/test-sound build/test-pal build/test-alignment build/test-rx-queue build/test-registration build/test-dsp build/test-signal build/test-stack build/test-stack-worker all tools
	./build/test-nicam
	./build/test-nicam-sensitivity
	./build/test-sound
	./build/test-pal
	./build/test-video-deemphasis
	./build/test-alignment
	./build/test-rx-queue
	./build/test-stack-worker
	./build/test-registration
	./build/test-stack
	./build/test-dsp
	./build/test-signal
	SDL_AUDIODRIVER=dummy python3 tests/test_iq_pipeline.py
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./build/atv-rx --no-config --no-wizard --demo --seconds 1
clean:
	rm -rf build

build/bench-pipeline: tests/nicam_signal.h src/sound.c src/sound.h src/test_signal.c src/test_signal.h tests/bench_pipeline.c src/alignment.c src/alignment.h src/filter.c src/dsp.c src/pal.c src/atv.h src/pal.h $(NICAM_SRC) $(NICAM_DEPS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/bench_pipeline.c src/sound.c src/test_signal.c src/alignment.c src/filter.c src/dsp.c src/pal.c $(NICAM_SRC) -o $@ $(LDLIBS)
bench: build/bench-pipeline
	./build/bench-pipeline

build/bench-if-filter: tests/bench_if_filter.c src/filter.c src/atv.h src/pal.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/bench_if_filter.c src/filter.c -o $@ $(LDLIBS)

build/test-sound: tests/test_sound.c src/sound.c src/sound.h src/filter.c src/dsp.c src/pal.c src/atv.h src/pal.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_sound.c src/sound.c src/filter.c src/dsp.c src/pal.c -o $@ -lfftw3f -lm

build/test-nicam: tests/nicam_signal.h tests/test_nicam.c $(NICAM_SRC) $(NICAM_DEPS) src/sound.c src/sound.h src/filter.c src/dsp.c src/pal.c src/atv.h src/pal.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_nicam.c $(NICAM_SRC) src/sound.c src/filter.c src/dsp.c src/pal.c -o $@ $(LDLIBS)

build/test-nicam-sensitivity: tests/test_nicam_sensitivity.c tests/nicam_signal.h $(NICAM_DEPS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_nicam_sensitivity.c -o $@ -lm

build/test-video-deemphasis: tests/test_video_deemphasis.c src/filter.c src/pal.c src/atv.h src/pal.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_video_deemphasis.c src/filter.c src/pal.c -o $@ -lfftw3f -lm

ifeq ($(HAVE_LIBIIO),1)
build/test-pluto: tests/test_pluto.c src/source_pluto.c src/source_pluto.h src/atv.h src/source.c src/settings.h src/rx_queue.c src/rx_queue.h src/dsp.c src/pal.c src/pal.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_pluto.c src/source_pluto.c src/source.c src/rx_queue.c src/dsp.c src/pal.c -o $@ $(LDLIBS)
test: test-pluto
.PHONY: test-pluto
test-pluto: build/test-pluto
	./build/test-pluto
endif

build/test-settings: tests/test_settings.c src/settings.c src/settings.h src/atv.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_settings.c src/settings.c -o $@ -lm
.PHONY: test-settings
test-settings: build/test-settings
	./build/test-settings
test: test-settings

build/test-startup: src/video_quality.c src/status_server.c src/driver_status.c src/driver_status.h src/ui_text.c src/ui_text.h src/version.h Makefile tests/test_startup.c src/main.c src/settings.c src/settings.h src/sound.c src/source.c src/source_pluto.c src/rx_queue.c src/afc.h src/afc.c src/alignment.c src/filter.c src/dsp.c src/pal.c src/stack.c $(NICAM_SRC) $(NICAM_DEPS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(RX_BUILD_FLAGS) tests/test_startup.c src/video_quality.c src/status_server.c src/driver_status.c src/ui_text.c src/settings.c src/sound.c src/source.c src/source_pluto.c src/rx_queue.c src/afc.c src/alignment.c src/filter.c src/dsp.c src/pal.c src/stack.c $(NICAM_SRC) -o $@ $(LDLIBS)
.PHONY: test-startup
test-startup: build/test-startup
	./build/test-startup
test: test-startup

build/test-driver-status: tests/test_driver_status.c src/driver_status.c src/driver_status.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_driver_status.c src/driver_status.c -o $@
build/test-driver-status-no-pluto: tests/test_driver_status.c src/driver_status.c src/driver_status.h | build
	$(CC) $(CPPFLAGS) -UHAVE_LIBIIO $(CFLAGS) tests/test_driver_status.c src/driver_status.c -o $@
.PHONY: test-driver-status
test-driver-status: build/test-driver-status build/test-driver-status-no-pluto
	./build/test-driver-status
	./build/test-driver-status-no-pluto
test: test-driver-status

# Windows x64 cross-build and portable ZIP (Fedora MinGW dependencies).
.PHONY: windows windows-package
windows windows-package:
	bash scripts/build-windows.sh

build/atv-rx build/test-startup build/test-stack-worker build/test-settings: src/platform.h
build/atv-rx build/atv-eval build/test-dsp build/test-stack build/test-registration \
build/test-stack-worker build/test-alignment build/test-sound build/test-nicam \
build/test-startup build/test-afc build/test-fm build/test-fm-scalar build/test-sdrplay \
build/bench-pipeline build/bench-scatter build/bench-fm-pipeline: src/fm_math.h

build/test-afc: tests/test_afc.c src/afc.c src/afc.h src/test_signal.c src/filter.c src/dsp.c src/pal.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_afc.c src/afc.c src/test_signal.c src/filter.c src/dsp.c src/pal.c -o $@ -lfftw3f -lm
.PHONY: test-afc
test-afc: build/test-afc
	./build/test-afc
test: test-afc

build/test-fm: tests/test_fm.c src/dsp.c src/pal.c src/atv.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_fm.c src/dsp.c src/pal.c -o $@ -lm
test: test-fm
.PHONY: test-fm
test-fm: build/test-fm build/test-fm-scalar
	./build/test-fm
	./build/test-fm-scalar

build/bench-scatter: src/video_quality.c src/status_server.c src/driver_status.c src/driver_status.h src/ui_text.c src/ui_text.h src/version.h Makefile tests/bench_scatter.c src/test_signal.c src/test_signal.h src/main.c src/settings.c src/settings.h src/sound.c src/source.c src/source_pluto.c src/rx_queue.c src/afc.h src/afc.c src/alignment.c src/filter.c src/dsp.c src/pal.c src/stack.c $(NICAM_SRC) $(NICAM_DEPS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(RX_BUILD_FLAGS) tests/bench_scatter.c src/test_signal.c src/video_quality.c src/status_server.c src/driver_status.c src/ui_text.c src/settings.c src/sound.c src/source.c src/source_pluto.c src/rx_queue.c src/afc.c src/alignment.c src/filter.c src/dsp.c src/pal.c src/stack.c $(NICAM_SRC) -o $@ $(LDLIBS)

build/bench-fm-pipeline: tests/bench_fm_pipeline.c src/alignment.c src/alignment.h src/filter.c src/dsp.c src/pal.c src/atv.h src/pal.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/bench_fm_pipeline.c src/alignment.c src/filter.c src/dsp.c src/pal.c -o $@ $(LDLIBS)

build/test-fm-scalar: tests/test_fm.c src/dsp.c src/pal.c src/atv.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DFM1_NO_SIMD tests/test_fm.c src/dsp.c src/pal.c -o $@ -lm

build/test-fm-math: tests/test_fm_math.c src/fm_math.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_fm_math.c -o $@ -lm
.PHONY: test-fm-math
test-fm-math: build/test-fm-math
	./build/test-fm-math
test: test-fm-math

build/test-sdrplay: tests/test_sdrplay.c src/source.c src/settings.h src/source_pluto.c src/source_pluto.h src/rx_queue.c src/rx_queue.h src/dsp.c src/pal.c src/pal.h src/atv.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DATV_NO_UHD tests/test_sdrplay.c src/source_pluto.c src/rx_queue.c src/dsp.c src/pal.c -o $@ $(LDLIBS)
.PHONY: test-sdrplay
test-sdrplay: build/test-sdrplay
	./build/test-sdrplay
test: test-sdrplay

build/atv-rx build/test-startup build/test-stack-worker build/bench-scatter: src/status_server.h

.PHONY: test-status-server
test-status-server: build/atv-rx
	python3 tests/test_status_server.py
test: test-status-server

build/atv-rx build/test-startup build/test-stack-worker build/bench-scatter: src/video_quality.h
build/test-video-quality: tests/test_video_quality.c src/video_quality.c src/video_quality.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_video_quality.c src/video_quality.c -o $@ -lm
.PHONY: test-video-quality
test-video-quality: build/test-video-quality build/test-video-quality-pipeline
	./build/test-video-quality
	./build/test-video-quality-pipeline
test: test-video-quality

build/test-video-quality-pipeline: tests/test_video_quality_pipeline.c src/video_quality.c src/video_quality.h src/alignment.c src/alignment.h src/test_signal.c src/filter.c src/dsp.c src/pal.c src/atv.h src/fm_math.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_video_quality_pipeline.c src/video_quality.c src/alignment.c src/test_signal.c src/filter.c src/dsp.c src/pal.c -o $@ $(LDLIBS)

build/atv-rx build/test-startup build/test-stack-worker build/bench-scatter: src/clipping.h
build/test-clipping: tests/test_clipping.c src/clipping.h src/atv.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_clipping.c -o $@ -lm
.PHONY: test-clipping
test-clipping: build/test-clipping
	./build/test-clipping
test: test-clipping
