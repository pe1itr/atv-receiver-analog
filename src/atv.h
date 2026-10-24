#ifndef ATV_H
#define ATV_H
#include <stdint.h>
#include <stddef.h>
#include "pal.h"
#define WIDTH 640
#define HEIGHT 288
#define FFT_SIZE 2048
#define BLOCK 32768
#define PI 3.14159265358979323846
/* Interleaved float32 I,Q, matching UHD fc32 and Soapy CF32. */
typedef struct { float i, q; } IQ;
typedef struct {
    const char *mode, *args, *images, *antenna, *ref, *file, *snapshot;
    double lo; /* Display frequency = SDR frequency + external converter LO. */
    double rate, frequency, bandwidth, if_bandwidth, gain, deviation, seconds, line_hz;
    size_t channel, rx_block;
    int invert, free_sync, loop, video_deemphasis, experimental_sync, fm_mode;
    const char *pluto_uri;
    const char *audio_output; /* SDL playback device name; empty = system default. */
    int status_enabled, status_port;
    const char *status_bind; /* Numeric IPv4 address for optional read-only HTTP API. */
    const char *record;
    double record_seconds;
    double alignment_bandwidth;
    double hackrf_vga; /* Fixed RX VGA in dB; UI gain controls LNA only. */
} Config;
typedef struct Source Source;
typedef struct {
    size_t queued_samples, peak_samples, capacity_samples;
    uint64_t discarded_samples, hardware_overflows;
} SourceStats;
Source *source_open(Config *c);
int source_read(Source *s, IQ *iq, size_t n); /* -3 file loop, -2 discontinuity, -1 error, 0 timeout/EOF */
int source_tune(Source *s, double *hz, double *gain);
void source_stop(Source *s);
/* Stop hardware and discard queued IQ; next read restarts reception. */
void source_pause(Source *s);
void source_get_stats(Source *s, SourceStats *stats);
void source_close(Source *s);
typedef struct IFFilter IFFilter;
IFFilter *if_filter_create(double rate, double bandwidth);
/* Linear-phase PAL F.405-1 inverse; group delay VIDEO_EQ_DELAY samples. */
#define VIDEO_EQ_DELAY 128
IFFilter *video_deemphasis_create(double rate);
/* Real video, in place; filter must come from video_deemphasis_create. */
void video_deemphasis_process(IFFilter *f, float *video, size_t count);
void if_filter_process(IFFilter *f, IQ *iq, size_t count);
void if_filter_reset(IFFilter *f);
void if_filter_destroy(IFFilter *f);
/* Independent FM state, with output held on the original IQ sample clock. */
typedef struct {
    float pi, pq, filtered, scale, alpha;
    unsigned stride;
    uint64_t tick, previous_tick;
    int valid, mode;
    IQ history[8];
    unsigned history_pos, history_count;
} FMDemod;
void fm_demod_init(FMDemod *d, double rate, double deviation, int invert, unsigned stride);
void fm_demod_process(FMDemod *d, const IQ *iq, float *video, size_t n);
typedef void (*FrameFn)(void *, const uint32_t *);
typedef struct Decoder {
    double rate, deviation, line_hz;
    int free_sync, vertical_shift, experimental_sync;
    int invert, previous_valid, low, have_field, have_line, row, column;
    uint64_t tick, pulse, line, field, fields, lines;
    unsigned complete_lines, stable_lines;
    int stack_valid, stack_phase;
    float pi, pq, filtered, sync_filtered, blank, porch_sum;
    unsigned porch_count;
    double free_origin, free_period;
    /* Raster event cache; invalidated by changes to recovered clock values. */
    double sample_time;
    double nominal_line_period, raster_period, raster_origin, raster_field_period;
    uint64_t raster_next_line, raster_field_ticks;
    uint64_t hsync_pulses[512];
    unsigned hsync_count, hsync_updates;
    uint64_t hsync_field;
    double hsync_sum[3][128];
    double fold_period, fold_phase;
    uint64_t fold_tick, vsync_bin_samples;
    unsigned hsync_samples[3][128];
    double vsync_bins[64], vsync_sum, vsync_best, vsync_period;
    double vsync_times[8];
    uint64_t vsync_bin_tick, vsync_peak, vsync_last, vsync_event, vsync_confirmed;
    unsigned vsync_samples, vsync_pos, vsync_filled, vsync_count;
    unsigned vsync_updates, vsync_epoch;
    /* Experimental full-field candidate selection, independent of raster phase. */
    uint64_t vsync_scan_tick, vsync_scan_peak;
    double vsync_scan_best;
    uint32_t pixels[WIDTH * HEIGHT];
    uint32_t guide_pixels[WIDTH * HEIGHT];
    float guide_pi, guide_pq, guide_filtered;
    unsigned guide_stride;
    uint64_t guide_tick;
    int guided;
    int color;
    int video_deemphasis;
    float sync_delay[VIDEO_EQ_DELAY];
    unsigned sync_delay_pos;
    PalDecoder pal;
    int no_video_raster, no_guide_raster; /* Sync remains active when unused images are omitted. */
    FrameFn frame;
    void *opaque;
    /* Optional independently initialized free-running raster at the same rate.
     * Receives this decoder's demodulated samples; never follows sync pulses. */
    struct Decoder *preview;
} Decoder;
void decoder_init(Decoder *d, double rate, double deviation, int invert, FrameFn fn, void *opaque);
void decoder_process(Decoder *d, const IQ *iq, size_t n);
/* Both inputs must have the same sample clock and filter delay. */
void decoder_process_guided(Decoder *d, const IQ *iq, const IQ *guide, size_t n);
/* Pre-demodulated inputs retain one value for every original IQ sample. */
void decoder_process_video(Decoder *d, const float *video, const float *guide, size_t n);
typedef struct {
    uint32_t sum[WIDTH * HEIGHT];
    uint16_t weight[WIDTH * HEIGHT];
    uint8_t reference_pixels[WIDTH*HEIGHT];
    float reference[(WIDTH/4)*(HEIGHT/4)];
    double dx, dy, shear, confidence;
    const char *status; /* Static diagnostic text for the last contribution. */
    unsigned count, target, next;
    uint64_t accepted;
    uint16_t *history; /* Aligned grey samples; 256 means no coverage. */
    int rolling;
    int phase, pending, weak;
    unsigned pending_misses;
    uint64_t seen, motion_time[24], relock_seen, relock_previous_seen;
    double motion_x[24], motion_y[24], period_x, period_y;
    double relock_x, relock_y, relock_previous_x, relock_previous_y;
    unsigned motion_count;
} FrameStack;
/* Zero-initialize before first use; destroy before releasing the owner. */
void stack_reset(FrameStack *s, unsigned target);
int stack_configure(FrameStack *s, unsigned target, int rolling);
void stack_destroy(FrameStack *s);
void stack_discontinuity(FrameStack *s);
int stack_add(FrameStack *s, const uint32_t *pixels, int valid, int phase, uint32_t *out);
int stack_add_guided(FrameStack *s, const uint32_t *pixels, const uint32_t *guide, int valid, int phase, uint32_t *out);
/* Caller guarantees common raster coordinates from confirmed H and V sync. */
int stack_add_synced(FrameStack *s, const uint32_t *pixels, const uint32_t *guide, int valid, int phase, uint32_t *out);
void demo_generate(IQ *iq, size_t n, double rate, double deviation, uint64_t *tick, double *phase);
#endif
