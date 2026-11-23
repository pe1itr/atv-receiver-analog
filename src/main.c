#include "alignment.h"
#include "pal_pipeline.h"
#include "afc.h"
#include "sound.h"
#include "nicam.h"
#include "atv.h"
#include "settings.h"
#include "version.h"
#include "ui_text.h"
#include "driver_status.h"
#include "status_server.h"
#include "video_quality.h"
#include "video_scope.h"
#include "iq_recorder.h"
#include "light_stack.h"
#include "clipping.h"
#include "iq_dc_block.h"
#include "iq_power.h"
#include "diagnostic_log.h"
#include <unistd.h>
#include <fcntl.h>
#include <SDL.h>
#include <fftw3.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <png.h>
#include "platform.h"

/* Only hardware reception must sustain realtime throughput. File/demo
 * sources can wait for processing, including stacking at higher rates. */
static int stacking_limited(const Config *c)
{
    return c->rate>10000000 &&
        (!strcmp(c->mode,"b210") || !strcmp(c->mode,"hackrf") || !strcmp(c->mode,"pluto") || !strcmp(c->mode,"sdrplay"));
}

static int color_enabled(double rate, double bandwidth)
{
    return rate>=12e6 && bandwidth>=10e6;
}

enum { UI_SIDE_WIDTH=140, UI_CONTROL_WIDTH=400,
       UI_WIDTH=UI_SIDE_WIDTH+960+UI_CONTROL_WIDTH, UI_HEIGHT=880 };

typedef struct {
    float power[FFT_SIZE];
    uint64_t samples;
    int valid;
} SpectrumAverage;

/* Average linear FFT power before conversion to dB, with a 180 ms time
 * constant measured in input samples. Each FFT contributes exactly once. */
static void spectrum_average_update(SpectrumAverage *average,float *bins,uint64_t samples,double rate)
{
    double weight=average->valid && samples>=average->samples?
        -expm1(-(double)(samples-average->samples)/(rate*.180)):1;
    for(int k=0;k<FFT_SIZE;++k) {
        float power=isfinite(bins[k])?fmaxf(0,bins[k]):0;
        if(!average->valid) average->power[k]=power;
        else average->power[k]+=(float)weight*(power-average->power[k]);
        bins[k]=10*log10f(average->power[k]+1e-16f);
    }
    average->valid=1;
    average->samples=samples;
}

typedef struct {
    int wide;
    int top_db, bottom_db, step_db;
} SpectrumScale;

/* Display scaling only: do not change the FFT levels or receiver gain.
 * A 5 dB hysteresis keeps noise near the threshold from flipping the axis. */
static void spectrum_scale_update(SpectrumScale *scale,const float *spectrum)
{
    float peak=-INFINITY;
    for(int k=0;k<FFT_SIZE;++k)
        if(isfinite(spectrum[k])) peak=fmaxf(peak,spectrum[k]);
    if(peak>=-40) scale->wide=1;
    else if(peak<-45) scale->wide=0;
    scale->top_db=scale->wide?0:-40;
    scale->bottom_db=scale->wide?-100:-80;
    scale->step_db=scale->wide?20:10;
}

static int spectrum_y(const SpectrumScale *scale,float db)
{
    double fraction=(scale->top_db-db)/(scale->top_db-scale->bottom_db);
    return 630+(int)lround(fmin(1,fmax(0,fraction))*140);
}

typedef struct {
    uint32_t pixels[WIDTH*HEIGHT];
    uint32_t guide[WIDTH*HEIGHT];
    int guided, synced;
    int lightweight; /* 0 aligned stack, 1 six-picture RGB, 2 variable live RGB. */
    int valid, phase, clear_pending;
    const char *reason;
    double period_x, period_y;
    uint64_t serial;
} StackFrame;

typedef struct {
    uint64_t blocks, wait_ticks, read_overhead_ticks, processing_ticks, empty_reads;
} BlockTiming;

typedef struct {
    Config c;
    Decoder *decoder; /* Worker-owned, used in its callback. */
    char my_call[17], my_locator[11]; /* UI-owned station identification. */
    FrameStack stack; /* UI statistics/settings only; worker owns the accumulator. */
    StackFrame queued;
    SDL_cond *stack_ready;
    int stack_pending, stack_busy;
    unsigned stack_epoch, stack_reset_id, stack_motion_id;
    uint64_t stack_skipped;
    int stacking, stack_paused, stack_hsync, sync_locked;
    int horizontal_locked, vertical_locked;
    /* Observed at decoder frame callbacks, retained between F10 snapshots. */
    uint64_t horizontal_losses, vertical_losses, sync_losses, frame_stalls;
    uint32_t max_frame_gap_ms;
    SyncEvidence sync_evidence;
    int sync_observed, have_hsync, have_vsync;
    uint64_t horizontal_age_fields;
    double vertical_age_ms;
    uint64_t consumed_samples;
    uint64_t diagnosis_previous_samples;
    uint32_t diagnosis_previous_tick;
    BlockTiming timing, diagnosis_previous_timing;
    int light_stacking;
    unsigned light_count;
    uint64_t light_input_serial;
    unsigned stack_vsync;
    int stack_sync_input, stack_synced;
    uint32_t last_stacked;
    Source *source;
    SDL_mutex *mutex;
    SDL_atomic_t stop, failed, done, audio_muted, audio_locked, audio_source, audio_swap, nicam_detect, nicam_lock;
    SDL_atomic_t mono_detect, stereo_detect, audio_metrics_tick;
    SDL_atomic_t audio_dsp_queued, audio_dsp_overflows;
    SDL_atomic_t nicam_good_frames, nicam_bad_frames, nicam_muted_frames, nicam_sync_bits, nicam_queue_overflows;
    SDL_atomic_t playback_refills, playback_resets, playback_max_gap_us;
    SDL_AudioDeviceID audio_device;
    double requested_hz, requested_gain, actual_hz, actual_gain;
    int command, invert, free_sync, vertical_shift, fm_mode, video_deemphasis, experimental_sync;
    int afc_enabled, afc_state; /* 0 seeking, 1 tracking, 2 outside capture range. */
    unsigned afc_epoch;
    double afc_anchor;
    int info_paused;
    double if_bandwidth, line_hz, raster_hz;
    uint32_t pixels[WIDTH * HEIGHT], live[WIDTH * HEIGHT], last_frame;
    float spectrum[FFT_SIZE], power;
    unsigned frames, overflows;
    Clipping clipping;
    int color_locked;
    SourceStats rx_stats;
    uint32_t metrics_tick, if_power_tick;
    double if_power_dbfs;
    VideoQualityReading quality;
    uint32_t quality_tick;
    SDL_atomic_t scope_enabled, scope_timebase, scope_epoch;
    VideoScopeReading scope;
    uint32_t scope_tick;
    IQRecorder *recorder;
} App;

/* Only scalar diagnostics are copied; no image or IQ data enters the API. */
static double json_number(double x) { return isfinite(x)?x:0; }
static const char *sync_indicator_reason(const App *a,uint32_t now)
{
    if(!a->last_frame) return "no_video";
    if(now-a->last_frame>=150) return "stale_video";
    if(!a->horizontal_locked && !a->vertical_locked) return "no_hv_sync";
    if(!a->horizontal_locked) return "no_h_sync";
    if(!a->vertical_locked) return "no_v_sync";
    return "locked";
}
static void sync_diagnostics_frame(App *a,uint32_t now,int horizontal,int vertical)
{
    if(a->sync_observed) {
        if(a->horizontal_locked && !horizontal) ++a->horizontal_losses;
        if(a->vertical_locked && !vertical) ++a->vertical_losses;
        if(a->sync_locked && !(horizontal && vertical)) ++a->sync_losses;
        uint32_t gap=now-a->last_frame;
        if(gap>=150) ++a->frame_stalls;
        if(gap>a->max_frame_gap_ms) a->max_frame_gap_ms=gap;
    }
    a->sync_observed=1;
    a->horizontal_locked=horizontal;
    a->vertical_locked=vertical;
    a->sync_locked=horizontal && vertical;
}
static int status_json(void *opaque,char *out,size_t capacity)
{
    App *a=opaque;
#define B(x) ((x)?"true":"false")
#define N(x) json_number(x)
    SDL_LockMutex(a->mutex);
    uint32_t now=SDL_GetTicks();
    char h_age[48]="null",v_age[48]="null";
    if(a->have_hsync) snprintf(h_age,sizeof(h_age),"%llu",(unsigned long long)a->horizontal_age_fields);
    if(a->have_vsync) snprintf(v_age,sizeof(v_age),"%.9g",a->vertical_age_ms);
    int light_mode=a->c.rate>10e6 && !a->stacking;
    char hackrf_gains[160]="";
    if(!strcmp(a->c.mode,"hackrf"))
        snprintf(hackrf_gains,sizeof(hackrf_gains),",\"gain_control\":\"lna\",\"rx_lna_db\":%.12g,\"rx_vga_db\":%.12g,\"rf_amp_db\":%d",a->actual_gain,a->c.hackrf_vga,a->c.hackrf_amp?14:0);
    char if_level[48];
    if(isfinite(a->if_power_dbfs)) snprintf(if_level,sizeof(if_level),"%.12g",a->if_power_dbfs);
    else strcpy(if_level,"null");
    int quality_valid=a->quality.valid && !a->info_paused && SDL_GetTicks()-a->quality_tick<1000;
    char snr[48]="null", noise[48]="null", depth[48]="null", uncertainty[48]="null";
    if(quality_valid) snprintf(snr,sizeof(snr),"%.9g",a->quality.video_snr_db);
    int quality_fresh=a->quality_tick && SDL_GetTicks()-a->quality_tick<1000 && !a->info_paused;
    if(quality_fresh) {
        if(isfinite(a->quality.noise_rms)) snprintf(noise,sizeof(noise),"%.9g",a->quality.noise_rms);
        if(isfinite(a->quality.sync_depth)) snprintf(depth,sizeof(depth),"%.9g",a->quality.sync_depth);
        if(isfinite(a->quality.sync_depth_error)) snprintf(uncertainty,sizeof(uncertainty),"%.9g",a->quality.sync_depth_error);
    }
    const char *quality_reason=a->info_paused?"paused":
        a->quality_tick && !quality_fresh?"stale":video_quality_status(&a->quality);
    int n=snprintf(out,capacity,
        "{\"schema_version\":1,\"version\":\"%s\",\"build_utc\":\"%s\",\"git_commit\":\"%s\","
        "\"timestamp_unix_s\":%lld,\"metrics_available\":%s,\"metrics_age_ms\":%u,"
        "\"source\":\"%s\",\"done\":%s,\"failed\":%s,\"reception_paused\":%s,"
        "\"receiver\":{\"frequency_hz\":%.12g,\"sdr_frequency_hz\":%.12g,\"requested_frequency_hz\":%.12g,"
        "\"converter_lo_hz\":%.12g,\"sample_rate_sps\":%.12g,\"rf_bandwidth_hz\":%.12g,"
        "\"if_bandwidth_hz\":%.12g,\"alignment_bandwidth_hz\":%.12g,\"gain_db\":%.12g,"
        "\"fm_mode\":%d,\"deviation_hz\":%.12g,\"invert\":%s,\"pal_deemphasis\":%s,"
        "\"rx_block_samples\":%zu,\"channel\":%zu,\"experimental_sync\":%s,\"iq_dc_block\":%s,\"requested_gain_db\":%.12g%s},"
        "\"video\":{\"signal_dbfs\":%.12g,\"if_signal_dbfs\":%s,\"if_signal_age_ms\":%u,\"fields\":%u,\"discontinuities\":%u,"
        "\"sync_locked\":%s,\"color_enabled\":%s,\"color_locked\":%s,\"free_sync\":%s,\"line_hz\":%.12g,\"vertical_shift_lines\":%d,\"horizontal_locked\":%s,\"vertical_locked\":%s,\"raster_hz\":%.12g,\"age_ms\":%u,\"consumed_samples\":%llu},"
        "\"sync_diagnostics\":{\"indicator_reason\":\"%s\",\"horizontal_losses\":%llu,\"vertical_losses\":%llu,\"combined_losses\":%llu,"
        "\"horizontal_age_fields\":%s,\"vertical_age_ms\":%s,\"completed_frame_stalls\":%llu,\"max_completed_frame_gap_ms\":%u,"
        "\"h_decision\":\"%s\",\"h_pulses\":%u,\"h_consensus\":%u,\"v_decision\":\"%s\",\"v_last_candidate_chain\":%u,"
        "\"v_scans\":%llu,\"v_candidates\":%llu,\"v_timing_resets\":%llu,\"v_confirmations\":%llu},"
        "\"clipping\":{\"active\":%s,\"repeated\":%s,\"clipped_samples\":%llu,\"inspected_samples\":%llu,\"last_clip_age_ms\":%u},"
        "\"video_quality\":{\"valid\":%s,\"age_ms\":%u,\"video_snr_db\":%s,\"noise_rms\":%s,"
        "\"sync_depth\":%s,\"sync_depth_error\":%s,\"reason\":\"%s\","
        "\"lines\":%u,\"method\":\"pal_back_porch_sync_scaled_unweighted\"},"
        "\"afc\":{\"enabled\":%s,\"state\":%d,\"anchor_sdr_hz\":%.12g},"
        "\"stack\":{\"enabled\":%s,\"paused\":%s,\"rolling\":%s,\"synced\":%s,\"lightweight\":%s,"
        "\"count\":%u,\"target\":%u,\"skipped\":%llu,\"confidence\":%.12g,\"dx\":%.12g,\"dy\":%.12g},"
        "\"audio\":{\"source\":%d,\"muted\":%s,\"locked\":%s,\"nicam_detected\":%s,\"nicam_locked\":%s,"
        "\"fm_702_detected\":%s,\"fm_722_detected\":%s,\"fm_738_detected\":%s,\"fm_756_detected\":%s,\"metrics_age_ms\":%u,"
        "\"swap_lr\":%s,\"output_open\":%s,\"dsp_queued_samples\":%d,\"dsp_queue_overflows\":%d,"
        "\"nicam_good_frames\":%u,\"nicam_bad_frames\":%u,\"nicam_muted_frames\":%u,\"nicam_sync_bits_discarded\":%u,\"nicam_queue_overflows\":%u,"
        "\"playback_refills\":%u,\"playback_resets\":%u,\"playback_max_supply_gap_ms\":%.3f},"
        "\"rx_queue\":{\"queued_samples\":%zu,\"peak_samples\":%zu,\"capacity_samples\":%zu,"
        "\"discarded_samples\":%llu,\"hardware_overflows\":%llu}}\n",
        ATV_VERSION,ATV_BUILD_UTC,ATV_GIT_COMMIT,(long long)time(NULL),B(a->metrics_tick!=0),
        a->metrics_tick?SDL_GetTicks()-a->metrics_tick:0,a->c.mode,
        B(SDL_AtomicGet(&a->done)),B(SDL_AtomicGet(&a->failed)),B(a->info_paused),
        N(a->actual_hz+a->c.lo),N(a->actual_hz),N(a->requested_hz+a->c.lo),N(a->c.lo),N(a->c.rate),N(a->c.bandwidth),
        N(a->if_bandwidth),N(a->c.alignment_bandwidth>0?a->c.alignment_bandwidth:a->if_bandwidth),N(a->actual_gain),
        a->fm_mode,N(a->c.deviation),B(a->invert),B(a->video_deemphasis),
        a->c.rx_block,a->c.channel,B(a->experimental_sync),B(iq_dc_block_enabled(&a->c)),N(a->requested_gain),hackrf_gains,
        N(a->power),if_level,isfinite(a->if_power_dbfs)?SDL_GetTicks()-a->if_power_tick:0,a->frames,a->overflows,B(a->sync_locked),B(color_enabled(a->c.rate,a->if_bandwidth)),
        B(a->color_locked),B(a->free_sync),N(a->line_hz),a->vertical_shift,
        B(a->horizontal_locked),B(a->vertical_locked),N(a->raster_hz),
        a->last_frame?SDL_GetTicks()-a->last_frame:0,(unsigned long long)a->consumed_samples,
        sync_indicator_reason(a,now),(unsigned long long)a->horizontal_losses,(unsigned long long)a->vertical_losses,
        (unsigned long long)a->sync_losses,h_age,v_age,(unsigned long long)a->frame_stalls,a->max_frame_gap_ms,
        a->sync_evidence.h_result?a->sync_evidence.h_result:"pending",a->sync_evidence.h_pulses,a->sync_evidence.h_consensus,
        a->sync_evidence.v_result?a->sync_evidence.v_result:"pending",a->sync_evidence.v_group_length,
        (unsigned long long)a->sync_evidence.v_scans,(unsigned long long)a->sync_evidence.v_candidates,
        (unsigned long long)a->sync_evidence.v_timing_resets,(unsigned long long)a->sync_evidence.v_confirmations,
        B(clipping_active(&a->clipping,SDL_GetTicks())),
        B(clipping_repeated(&a->clipping,SDL_GetTicks())),
        (unsigned long long)a->clipping.clipped_samples,(unsigned long long)a->clipping.samples,
        a->clipping.clipped_samples?SDL_GetTicks()-a->clipping.last_clip_tick:0,
        B(quality_valid),a->quality_tick?SDL_GetTicks()-a->quality_tick:0,snr,noise,depth,uncertainty,quality_reason,
        a->quality.lines,B(a->afc_enabled),a->afc_state,N(a->afc_anchor),
        B(a->stacking || a->light_stacking),B(!light_mode && a->stack_paused),
        B(light_mode || a->stack.rolling),B(!light_mode && a->stack_synced),B(light_mode),
        light_mode?a->light_count:a->stack.count,light_mode?LIGHT_STACK_FRAMES:a->stack.target,
        (unsigned long long)a->stack_skipped,light_mode?0:N(a->stack.confidence),light_mode?0:N(a->stack.dx),light_mode?0:N(a->stack.dy),
        SDL_AtomicGet(&a->audio_source),B(SDL_AtomicGet(&a->audio_muted)),B(SDL_AtomicGet(&a->audio_locked)),
        B(SDL_AtomicGet(&a->nicam_detect)),B(SDL_AtomicGet(&a->nicam_lock)),
        B(SDL_AtomicGet(&a->stereo_detect)&1),B(SDL_AtomicGet(&a->stereo_detect)&2),
        B(SDL_AtomicGet(&a->stereo_detect)&4),B(SDL_AtomicGet(&a->stereo_detect)&8),
        SDL_AtomicGet(&a->audio_metrics_tick)?SDL_GetTicks()-(uint32_t)SDL_AtomicGet(&a->audio_metrics_tick):0,
        B(SDL_AtomicGet(&a->audio_swap)),B(a->audio_device!=0),SDL_AtomicGet(&a->audio_dsp_queued),SDL_AtomicGet(&a->audio_dsp_overflows),
        (unsigned)SDL_AtomicGet(&a->nicam_good_frames),(unsigned)SDL_AtomicGet(&a->nicam_bad_frames),
        (unsigned)SDL_AtomicGet(&a->nicam_muted_frames),(unsigned)SDL_AtomicGet(&a->nicam_sync_bits),
        (unsigned)SDL_AtomicGet(&a->nicam_queue_overflows),(unsigned)SDL_AtomicGet(&a->playback_refills),
        (unsigned)SDL_AtomicGet(&a->playback_resets),SDL_AtomicGet(&a->playback_max_gap_us)/1000.0,
        a->rx_stats.queued_samples,a->rx_stats.peak_samples,a->rx_stats.capacity_samples,
        (unsigned long long)a->rx_stats.discarded_samples,(unsigned long long)a->rx_stats.hardware_overflows);
    SDL_UnlockMutex(a->mutex);
#undef B
#undef N
    return n;
}

/* Step on the displayed RF grid, including a converter LO. From between
 * grid points, the first detent reaches the next point in that direction. */
static double tuning_grid(double hz, double lo, double step, double steps)
{
    if(!steps) return hz;
    double grid=(hz+lo)/step;
    /* SDR synthesizer readback can be a few Hz off the requested grid.
     * Treat that as on-grid, otherwise every key repeat can select the
     * same frequency again (below the grid going up, above going down). */
    double nearest=round(grid);
    if(fabs((hz+lo)-nearest*step)<=10.0) grid=nearest;
    double next=(steps>0?floor(grid+1e-9):ceil(grid-1e-9))+steps;
    return fmin(6e9,fmax(1,next*step-lo));
}

static int afc_available(const App *a)
{
    return strcmp(a->c.mode,"demo") && strcmp(a->c.mode,"file");
}
/* Caller holds mutex. Manual tuning establishes a fresh capture window. */
static void afc_manual_tune(App *a)
{
    a->command&=~8;
    a->afc_anchor=a->requested_hz;
    a->afc_state=0; ++a->afc_epoch;
}
static void afc_toggle(App *a)
{
    if(!afc_available(a)) return;
    a->afc_enabled=!a->afc_enabled;
    if((a->command&8) && !(a->command&1)) a->requested_hz=a->actual_hz;
    afc_manual_tune(a);
}

/* This callback runs on the UI thread. Copy scalars under the same mutex as
 * the status API; formatting and file saving do not block the DSP thread. */
static int diagnostic_snapshot(void *opaque,char *out,size_t capacity)
{
    App *a=opaque;
    char pluto[512];
    source_pluto_diagnostics(a->source,pluto,sizeof(pluto));
    char raw[8192];
    int raw_size=status_json(a,raw,sizeof(raw));
    if(raw_size<=0 || (size_t)raw_size>=sizeof(raw)) return -1;
    SDL_LockMutex(a->mutex);
    Uint32 now=SDL_GetTicks(),elapsed=now-a->diagnosis_previous_tick;
    int have_rate=a->diagnosis_previous_tick && elapsed && a->consumed_samples>=a->diagnosis_previous_samples;
    double throughput=have_rate?(a->consumed_samples-a->diagnosis_previous_samples)*1000.0/elapsed/1e6:0;
    char block_timing[384]="Block timing: awaiting completed blocks in a sampling interval\n";
    BlockTiming t=a->timing, prev=a->diagnosis_previous_timing;
    if(a->diagnosis_previous_tick && t.blocks>prev.blocks) {
        uint64_t blocks=t.blocks-prev.blocks;
        double scale=1000.0/SDL_GetPerformanceFrequency()/blocks;
        snprintf(block_timing,sizeof(block_timing),
            "Receiver thread: %llu completed blocks; mean wall ms/block: empty queue wait=%.3f; "
            "other input read (including copy)=%.3f; block processing (including DSP waits)=%.3f; "
            "empty reads=%llu in interval. Pluto RX runs concurrently.\n",
            (unsigned long long)blocks,(t.wait_ticks-prev.wait_ticks)*scale,
            (t.read_overhead_ticks-prev.read_overhead_ticks)*scale,(t.processing_ticks-prev.processing_ticks)*scale,
            (unsigned long long)(t.empty_reads-prev.empty_reads));
    }
    a->diagnosis_previous_timing=t;
    a->diagnosis_previous_tick=now; a->diagnosis_previous_samples=a->consumed_samples;
    int fresh=a->last_frame && now-a->last_frame<150;
    int quality_fresh=a->quality_tick && now-a->quality_tick<1000 && !a->info_paused;
    char snr[96],throughput_text[96],if_text[64];
    if(have_rate) snprintf(throughput_text,sizeof(throughput_text),"%.6f MS/s",throughput);
    else snprintf(throughput_text,sizeof(throughput_text),"unavailable (first snapshot or counter reset)");
    if(isfinite(a->if_power_dbfs)) snprintf(if_text,sizeof(if_text),"%.3f dBFS",a->if_power_dbfs);
    else snprintf(if_text,sizeof(if_text),"unavailable");
    if(quality_fresh && a->quality.valid) snprintf(snr,sizeof(snr),"%.3f dB",a->quality.video_snr_db);
    else snprintf(snr,sizeof(snr),"unavailable (%s)",a->info_paused?"paused":!quality_fresh?"stale or no snapshot":video_quality_status(&a->quality));
    int n=snprintf(out,capacity,
        "UTC Unix time: %lld\n"
        "Receiver: %s; paused=%d done=%d failed=%d; metrics age=%u ms\n"
        "%s%sTuning RF: requested=%.6f MHz actual=%.6f MHz; SDR=%.6f MHz; converter LO=%.6f MHz\n"
        "AFC: %s; anchor SDR=%.6f MHz; applied offset=%.3f kHz (not residual error)\n"
        "Rate=%.6f MS/s; RF BW=%.6f MHz; video IF=%.6f MHz; sync IF=%.6f MHz; gain=%.2f dB\n"
        "FM%d/D%d; inversion=%d; PAL de-emphasis=%d; experimental sync=%d; free sync=%d\n"
        "Sync: H=%d V=%d combined=%d; video fresh=%d age=%u ms; raster=%.6f Hz (0=unavailable)\n"
        "Sync indicator: %s; cumulative observed losses H=%llu V=%llu combined=%llu; completed frame gaps >=150 ms=%llu max=%u ms\n"
        "Latest H decision: %s (pulses in consensus %u of %u); latest V scan: %s (last candidate chain %u)\n"
        "V totals since decoder init: scans=%llu candidates=%llu timing resets=%llu confirmed=%llu\n"
        "Color: enabled=%d locked=%d; IF level=%s age=%u ms; Video-SNR=%s\n"
        "Consumed throughput: %s; total samples=%llu; queue=%.3f ms (peak=%.3f ms)\n"
        "Cumulative: discontinuities=%u discarded=%llu hardware overflows=%llu; clipping samples=%llu\n"
        "Raw status (full precision; compare counters and settings between snapshots):\n%s\n",
        (long long)time(NULL),a->c.mode,a->info_paused,SDL_AtomicGet(&a->done),SDL_AtomicGet(&a->failed),
        a->metrics_tick?now-a->metrics_tick:0,pluto,block_timing,
        (a->requested_hz+a->c.lo)/1e6,(a->actual_hz+a->c.lo)/1e6,a->actual_hz/1e6,a->c.lo/1e6,
        !a->afc_enabled?"OFF":a->afc_state==1?"TRACK":a->afc_state==2?"LIMIT":"SEEK (no reliable estimate)",
        a->afc_anchor/1e6,(a->actual_hz-a->afc_anchor)/1000,
        a->c.rate/1e6,a->c.bandwidth/1e6,a->if_bandwidth/1e6,
        (a->c.alignment_bandwidth>0?a->c.alignment_bandwidth:a->if_bandwidth)/1e6,a->actual_gain,
        a->fm_mode,a->fm_mode,a->invert,a->video_deemphasis,a->experimental_sync,a->free_sync,
        a->horizontal_locked,a->vertical_locked,a->sync_locked,fresh,a->last_frame?now-a->last_frame:0,a->raster_hz,
        sync_indicator_reason(a,now),(unsigned long long)a->horizontal_losses,(unsigned long long)a->vertical_losses,
        (unsigned long long)a->sync_losses,(unsigned long long)a->frame_stalls,a->max_frame_gap_ms,
        a->sync_evidence.h_result?a->sync_evidence.h_result:"pending",a->sync_evidence.h_consensus,a->sync_evidence.h_pulses,
        a->sync_evidence.v_result?a->sync_evidence.v_result:"pending",a->sync_evidence.v_group_length,
        (unsigned long long)a->sync_evidence.v_scans,(unsigned long long)a->sync_evidence.v_candidates,
        (unsigned long long)a->sync_evidence.v_timing_resets,(unsigned long long)a->sync_evidence.v_confirmations,
        color_enabled(a->c.rate,a->if_bandwidth),a->color_locked,if_text,
        isfinite(a->if_power_dbfs)?now-a->if_power_tick:0,snr,
        throughput_text,(unsigned long long)a->consumed_samples,
        1000.0*a->rx_stats.queued_samples/a->c.rate,1000.0*a->rx_stats.peak_samples/a->c.rate,
        a->overflows,(unsigned long long)a->rx_stats.discarded_samples,
        (unsigned long long)a->rx_stats.hardware_overflows,(unsigned long long)a->clipping.clipped_samples,raw);
    SDL_UnlockMutex(a->mutex);
    return n;
}

typedef struct {
    int active;
    char value[32];
    const char *error;
} FrequencyInput;

typedef struct {
    int active, field;
    char my_call[17], my_locator[11];
    const char *error;
} StationInput;

static int valid_locator(const char *s)
{
    size_t n=strlen(s);
    if(!n) return 1; /* Identification is optional. */
    if(n<4 || n>10 || n%2) return 0;
    for(size_t i=0;i<n;++i) {
        char low=(i/2)%2?'0':'A';
        char high=(i/2)%2?'9':i<2?'R':'X';
        if(s[i]<low || s[i]>high) return 0;
    }
    return 1;
}

static int station_input_event(StationInput *input,App *a,const SDL_Event *e)
{
    if(e->type==SDL_QUIT) return 0;
    if(!input->active) {
        if(e->type!=SDL_KEYDOWN || e->key.keysym.sym!=SDLK_F7 || e->key.repeat) return 0;
        *input=(StationInput){.active=1};
        strcpy(input->my_call,a->my_call); strcpy(input->my_locator,a->my_locator);
        SDL_StartTextInput(); return 1;
    }
    char *value=input->field?input->my_locator:input->my_call;
    size_t capacity=input->field?sizeof(input->my_locator):sizeof(input->my_call);
    if(e->type==SDL_TEXTINPUT) {
        size_t n=strlen(value), extra=strlen(e->text.text);
        if(n+extra>=capacity) { input->error="VALUE TOO LONG"; return 1; }
        for(size_t i=0;i<extra;++i) {
            char ch=e->text.text[i];
            if(ch>='a' && ch<='z') ch-='a'-'A';
            if((ch<'A' || ch>'Z') && (ch<'0' || ch>'9') && (input->field || (ch!='/' && ch!='-')))
                return 1;
        }
        for(size_t i=0;i<extra;++i) {
            char ch=e->text.text[i];
            value[n+i]=ch>='a' && ch<='z'?ch-('a'-'A'):ch;
        }
        value[n+extra]=0; input->error=NULL;
    } else if(e->type==SDL_MOUSEBUTTONDOWN && e->button.button==SDL_BUTTON_LEFT) {
        if(e->button.x>=60 && e->button.x<900) {
            if(e->button.y>=305 && e->button.y<345) input->field=0;
            if(e->button.y>=375 && e->button.y<415) input->field=1;
        }
    } else if(e->type==SDL_KEYDOWN) {
        SDL_Keycode key=e->key.keysym.sym;
        if(key==SDLK_ESCAPE) { input->active=0; SDL_StopTextInput(); }
        else if(key==SDLK_TAB || key==SDLK_UP || key==SDLK_DOWN) input->field=!input->field;
        else if(key==SDLK_BACKSPACE) {
            size_t n=strlen(value); if(n) value[n-1]=0;
            input->error=NULL;
        } else if(key==SDLK_a && (e->key.keysym.mod & KMOD_CTRL)) {
            value[0]=0; input->error=NULL;
        } else if(key==SDLK_RETURN || key==SDLK_KP_ENTER) {
            if(!input->field) input->field=1;
            else if(!valid_locator(input->my_locator))
                input->error="MAIDENHEAD: USE 4, 6, 8 OR 10 CHARACTERS (E.G. JO22AA)";
            else {
                strcpy(a->my_call,input->my_call); strcpy(a->my_locator,input->my_locator);
                input->active=0; SDL_StopTextInput();
            }
        }
    }
    return 1;
}

/* Input uses displayed MHz; reception always uses Hz after the converter. */
static int parse_frequency(const char *value, double lo, double *hz)
{
    char normalized[32]; size_t n=strlen(value);
    if(!n || n>=sizeof(normalized)) return 0;
    for(size_t i=0;i<=n;++i) {
        char ch=value[i];
        if(ch && (ch<'0' || ch>'9') && ch!='.' && ch!=',') return 0;
        normalized[i]=ch==','?'.':ch;
    }
    char *end; errno=0;
    double displayed=strtod(normalized,&end)*1e6;
    double sdr=displayed-lo;
    if(errno || end==normalized || *end || !isfinite(displayed) || displayed<=0 || displayed>1e12 ||
       !isfinite(sdr) || sdr<=0 || sdr>6e9) return 0;
    *hz=sdr; return 1;
}

/* Modal entry consumes tuning/shortcut events until confirmed or cancelled. */
static int frequency_input_event(FrequencyInput *input, App *a, const SDL_Event *e)
{
    if(e->type==SDL_QUIT) return 0;
    if(!input->active) {
        if(e->type!=SDL_KEYDOWN || e->key.keysym.sym!=SDLK_t || e->key.repeat) return 0;
        *input=(FrequencyInput){.active=1};
        SDL_StartTextInput();
        return 1;
    }
    if(e->type==SDL_TEXTINPUT) {
        size_t n=strlen(input->value), extra=strlen(e->text.text);
        if(n+extra>=sizeof(input->value)) { input->error="TOO MANY DIGITS"; return 1; }
        for(size_t i=0;i<extra;++i) {
            char ch=e->text.text[i];
            if((ch<'0' || ch>'9') && ch!='.' && ch!=',') return 1;
        }
        memcpy(input->value+n,e->text.text,extra+1); input->error=NULL;
    } else if(e->type==SDL_KEYDOWN) {
        SDL_Keycode key=e->key.keysym.sym;
        if(key==SDLK_ESCAPE) { input->active=0; SDL_StopTextInput(); }
        else if(key==SDLK_BACKSPACE) {
            size_t n=strlen(input->value); if(n) input->value[n-1]=0;
            input->error=NULL;
        } else if(key==SDLK_a && (e->key.keysym.mod & KMOD_CTRL)) {
            input->value[0]=0; input->error=NULL;
        } else if(key==SDLK_RETURN || key==SDLK_KP_ENTER) {
            double hz;
            if(!parse_frequency(input->value,a->c.lo,&hz))
                input->error="INVALID MHZ: CHECK VALUE AND CONVERTER LO";
            else if(a->c.mode && !strcmp(a->c.mode,"sdrplay") && (hz<1000 || hz>2e9))
                input->error="RSP1B: SDR FREQUENCY MUST BE 0.001..2000 MHZ; CHECK LO";
            else {
                SDL_LockMutex(a->mutex);
                a->requested_hz=hz; afc_manual_tune(a); a->command|=1;
                SDL_UnlockMutex(a->mutex);
                input->active=0; SDL_StopTextInput();
            }
        }
    }
    return 1;
}

typedef struct {
    Config c;
    VideoQualityReading quality;
    uint32_t quality_tick;
    VideoScopeReading scope;
    uint32_t scope_tick;
    int scope_enabled, scope_timebase;
    int light_stacking;
    IQRecordStatus recording;
    uint32_t pixels[WIDTH*HEIGHT], live[WIDTH*HEIGHT], last_frame, last_stacked;
    float spectrum[FFT_SIZE], power;
    double actual_hz, actual_gain, if_bandwidth, line_hz, raster_hz, if_power_dbfs;
    unsigned frames, overflows;
    Clipping clipping;
    int color_locked;
    uint64_t stack_skipped;
    int mono_detect, stereo_detect, nicam_detect, audio_muted, audio_source, audio_swap;
    uint32_t audio_metrics_tick;
    int stacking, stack_paused, stack_synced, sync_locked, free_sync, done, fm_mode;
    int afc_enabled, afc_state;
    double afc_anchor;
    struct {
        unsigned count, target;
        int rolling;
        double confidence, dx, dy, shear;
        const char *status;
    } stack;
} View;

/* Copy a coherent display snapshot, then release reception before calling
 * SDL. Texture uploads and drawing can block unpredictably in the driver. */
static void capture_view(App *a, View *v)
{
    SDL_LockMutex(a->mutex);
    v->c=a->c;
    v->c.invert=a->invert;
    v->c.video_deemphasis=a->video_deemphasis;
    v->c.experimental_sync=a->experimental_sync;
    int show_stack=(a->stacking && a->stack.count) || a->light_stacking;
    memcpy(v->pixels,show_stack?a->pixels:a->live,sizeof(v->pixels));
    if(a->stacking) memcpy(v->live,a->live,sizeof(v->live));
    memcpy(v->spectrum,a->spectrum,sizeof(v->spectrum));
#define COPY_VIEW(name) v->name=a->name
    COPY_VIEW(quality); COPY_VIEW(quality_tick); COPY_VIEW(clipping);
    COPY_VIEW(scope); COPY_VIEW(scope_tick);
    v->scope_enabled=SDL_AtomicGet(&a->scope_enabled);
    v->scope_timebase=SDL_AtomicGet(&a->scope_timebase);
    v->mono_detect=SDL_AtomicGet(&a->mono_detect);
    v->stereo_detect=SDL_AtomicGet(&a->stereo_detect);
    v->nicam_detect=SDL_AtomicGet(&a->nicam_detect);
    v->audio_metrics_tick=(uint32_t)SDL_AtomicGet(&a->audio_metrics_tick);
    v->audio_muted=SDL_AtomicGet(&a->audio_muted);
    v->audio_source=SDL_AtomicGet(&a->audio_source);
    v->audio_swap=SDL_AtomicGet(&a->audio_swap);
    iq_recorder_status(a->recorder,&v->recording);
    COPY_VIEW(last_frame); COPY_VIEW(last_stacked); COPY_VIEW(power); COPY_VIEW(if_power_dbfs);
    COPY_VIEW(actual_hz); COPY_VIEW(actual_gain); COPY_VIEW(if_bandwidth); COPY_VIEW(line_hz);
    COPY_VIEW(raster_hz);
    COPY_VIEW(frames); COPY_VIEW(overflows); COPY_VIEW(stack_skipped);
    COPY_VIEW(stacking); COPY_VIEW(stack_paused); COPY_VIEW(stack_synced);
    COPY_VIEW(light_stacking);
    COPY_VIEW(afc_enabled); COPY_VIEW(afc_state); COPY_VIEW(afc_anchor);
    COPY_VIEW(fm_mode); COPY_VIEW(color_locked); COPY_VIEW(sync_locked); COPY_VIEW(free_sync);
    COPY_VIEW(stack.count); COPY_VIEW(stack.target); COPY_VIEW(stack.rolling);
    COPY_VIEW(stack.confidence); COPY_VIEW(stack.dx); COPY_VIEW(stack.dy);
    COPY_VIEW(stack.shear); COPY_VIEW(stack.status);
    if(a->c.rate>10e6 && !a->stacking) {
        v->stack.count=a->light_count; v->stack.target=LIGHT_STACK_FRAMES; v->stack.rolling=1;
        v->stack_paused=0; v->stack_synced=0;
    }
#undef COPY_VIEW
    v->done=SDL_AtomicGet(&a->done);
    SDL_UnlockMutex(a->mutex);
}

/* Called with the UI mutex held, except before threads have been started. */
static void reset_stack(App *a, unsigned target, int rolling)
{
    memset(&a->stack,0,sizeof(a->stack));
    a->stack.target=target; a->stack.rolling=rolling;
    a->stack_pending=0; a->last_stacked=0;
    a->light_count=0;
    a->stack_synced=0;
    ++a->stack_epoch; ++a->stack_reset_id;
    SDL_CondBroadcast(a->stack_ready);
}
static void interrupt_stack(App *a)
{
    ++a->stack_epoch; ++a->stack_motion_id;
    a->stack_pending=0;
    SDL_CondBroadcast(a->stack_ready);
}
/* Called with the UI mutex held. S selects lightweight smoothing at high
 * rates; explicitly requested offline contest stacks keep their controls. */
static void stack_action(App *a,int action)
{
    if(!action) return;
    if(a->c.rate>10e6 && !a->stacking) {
        if(action!=1) return;
        a->light_stacking=!a->light_stacking; a->stack_paused=0;
        reset_stack(a,a->stack.target,a->stack.rolling);
        memcpy(a->pixels,a->live,sizeof(a->pixels));
        return;
    }
    if(stacking_limited(&a->c)) return;
    unsigned target=a->stack.target;
    if(action==1) { a->stacking=!a->stacking; a->stack_paused=0; }
    if(action==5 && a->stacking) { a->stack_paused=!a->stack_paused; interrupt_stack(a); }
    if(action==2 && target>1) --target;
    if(action==3 && target<512) ++target;
    if(action==1 || action==4 || action==6 || target<a->stack.count ||
       (a->stack.rolling && target!=a->stack.target))
        reset_stack(a,target,action==6?!a->stack.rolling:a->stack.rolling);
    else if(a->stack.target!=target) { a->stack.target=target; ++a->stack_epoch; }
    if(!a->stacking) memcpy(a->pixels,a->live,sizeof(a->pixels));
}
static void queue_light_frame(App *a,const uint32_t *pixels)
{
    int small=a->stacking && a->stack.rolling && a->c.rate<=10e6;
    if(a->command || (small && a->stack_paused) || SDL_AtomicGet(&a->failed)) return;
    if(small ? !a->stack.count : !a->light_count) memcpy(a->pixels,pixels,sizeof(a->pixels));
    if(a->stack_pending) ++a->stack_skipped;
    memcpy(a->queued.pixels,pixels,sizeof(a->queued.pixels));
    a->queued.lightweight=small?2:1;
    a->queued.serial=++a->light_input_serial;
    a->stack_pending=1;
    SDL_CondSignal(a->stack_ready);
}
static int stack_worker(void *opaque)
{
    App *a=opaque;
    FrameStack *s=calloc(1,sizeof(*s));
    StackFrame *input=malloc(sizeof(*input));
    uint32_t *output=calloc(WIDTH*HEIGHT,sizeof(*output));
    LightStack *light=NULL;
    LiveStack *live=NULL;
    unsigned live_reset=0,live_motion=0;
    uint64_t light_serial=0;
    unsigned reset=0,motion=0;
    if(!s || !input || !output) { SDL_AtomicSet(&a->failed,1); goto cleanup; }
    for(;;) {
        SDL_LockMutex(a->mutex);
        while(!a->stack_pending && !SDL_AtomicGet(&a->stop) && !SDL_AtomicGet(&a->done))
            SDL_CondWait(a->stack_ready,a->mutex);
        if(!a->stack_pending) { SDL_UnlockMutex(a->mutex); break; }
        if(a->queued.lightweight) {
            memcpy(input->pixels,a->queued.pixels,sizeof(input->pixels));
            input->lightweight=a->queued.lightweight; input->serial=a->queued.serial;
        } else *input=a->queued;
        a->stack_pending=0; a->stack_busy=1;
        unsigned epoch=a->stack_epoch, next_reset=a->stack_reset_id, next_motion=a->stack_motion_id;
        unsigned target=a->stack.target;
        int rolling=a->stack.rolling;
        SDL_UnlockMutex(a->mutex);

        /* Neither reception nor rendering holds this accumulator's lock. */
        int configured=1;
        int added=0;
        if(input->lightweight==2) {
            if(!live) live=calloc(1,sizeof(*live));
            configured=live!=NULL;
            if(live && (live_reset!=next_reset || live_motion!=next_motion || live->target!=target)) {
                configured=live_stack_configure(live,target);
                live_reset=next_reset; live_motion=next_motion;
            }
            if(configured) { live_stack_add(live,input->pixels,output); added=1; }
        } else if(input->lightweight) {
            if(!light) {
                light=malloc(sizeof(*light));
                if(light) light_stack_reset(light);
            }
            configured=light!=NULL;
            if(light) {
                if(reset!=next_reset || motion!=next_motion ||
                   (light_serial && input->serial!=light_serial+1)) light_stack_reset(light);
                reset=next_reset; motion=next_motion; light_serial=input->serial;
                light_stack_add(light,input->pixels,output); added=1;
            }
        } else {
            if(reset!=next_reset || !s->target) {
                configured=stack_configure(s,target,rolling);
                reset=next_reset;
            } else s->target=target;
            if(motion!=next_motion) { stack_discontinuity(s); motion=next_motion; }
            s->period_x=input->period_x; s->period_y=input->period_y;
            /* Keep motion fitting on the input field clock, even when pending
             * stack frames were replaced while this worker was busy. */
            s->seen=input->serial-1;
            if(input->clear_pending) s->pending=0;
            added=configured && (input->synced ?
                stack_add_synced(s,input->pixels,input->guided?input->guide:input->pixels,input->valid,input->phase,output) : input->guided ?
                stack_add_guided(s,input->pixels,input->guide,input->valid,input->phase,output) :
                stack_add(s,input->pixels,input->valid,input->phase,output));
        }

        SDL_LockMutex(a->mutex);
        if(epoch==a->stack_epoch && a->stacking && a->stack.rolling && input->lightweight==2 && !a->stack_paused && !a->command) {
            a->stack.count=live?live->count:0; a->stack.accepted=live?live->accepted:0;
            a->stack.status="LIVE ROLLING"; a->stack_synced=0;
            a->stack.confidence=0; a->stack.dx=a->stack.dy=a->stack.shear=0;
            if(added) { memcpy(a->pixels,output,sizeof(a->pixels)); a->last_stacked=SDL_GetTicks(); }
        } else if(epoch==a->stack_epoch && a->light_stacking && input->lightweight==1 && !a->command) {
            a->light_count=light?light->count:0;
            if(added) { memcpy(a->pixels,output,sizeof(a->pixels)); a->last_stacked=SDL_GetTicks(); }
        } else if(epoch==a->stack_epoch && a->stacking && !input->lightweight && !a->stack_paused && !a->command) {
            a->stack.count=s->count; a->stack.accepted=s->accepted;
            a->stack.confidence=s->confidence;
            a->stack.status=input->reason ? input->reason : s->status;
            a->stack_synced=input->synced;
            a->stack.dx=s->dx; a->stack.dy=s->dy; a->stack.shear=s->shear;
            if(added) { memcpy(a->pixels,output,sizeof(a->pixels)); a->last_stacked=SDL_GetTicks(); }
        }
        a->stack_busy=0;
        if(!configured) { fprintf(stderr,"Cannot allocate stack buffer\n"); SDL_AtomicSet(&a->failed,1); }
        SDL_CondBroadcast(a->stack_ready);
        SDL_UnlockMutex(a->mutex);
        if(!configured) break;
    }
cleanup:
    if(s) stack_destroy(s);
    if(live) live_stack_destroy(live);
    free(live);
    free(s); free(input); free(output); free(light);
    SDL_LockMutex(a->mutex);
    a->stack_busy=0; a->stack_pending=0;
    SDL_CondBroadcast(a->stack_ready);
    SDL_UnlockMutex(a->mutex);
    return 0;
}
static void live_frame(void *opaque, const uint32_t *pixels)
{
    App *a=opaque;
    SDL_LockMutex(a->mutex);
    memcpy(a->live,pixels,sizeof(a->live));
    if(a->light_stacking || (a->stacking && a->stack.rolling && a->c.rate<=10e6)) queue_light_frame(a,pixels);
    SDL_UnlockMutex(a->mutex);
}
static void frame(void *opaque, const uint32_t *pixels)
{
    App *a = opaque;
    SDL_LockMutex(a->mutex);
    /* Files/demo may wait to preserve deterministic offline stacking. A live
     * SDR must always keep draining its input, regardless of alignment cost. */
    if(!strcmp(a->c.mode,"file") || !strcmp(a->c.mode,"demo"))
        while((a->stack_pending || a->stack_busy) && !SDL_AtomicGet(&a->stop) && !SDL_AtomicGet(&a->failed))
            SDL_CondWait(a->stack_ready,a->mutex);
    if(!a->decoder->preview) memcpy(a->live, pixels, sizeof(a->live));
    a->color_locked=a->decoder->pal.last_lock && a->decoder->tick-a->decoder->pal.last_lock<a->c.rate*.002;
    a->sync_evidence=a->decoder->sync_evidence;
    a->have_hsync=a->decoder->hsync_updates!=0;
    a->have_vsync=a->decoder->vsync_updates!=0;
    a->horizontal_age_fields=a->decoder->fields-a->decoder->hsync_field;
    a->vertical_age_ms=1000.0*(a->decoder->tick-a->decoder->vsync_confirmed)/a->decoder->rate;
    uint32_t frame_tick=SDL_GetTicks();
    sync_diagnostics_frame(a,frame_tick,
        a->have_hsync && a->horizontal_age_fields<=5,
        a->have_vsync && a->decoder->tick-a->decoder->vsync_confirmed<a->decoder->rate*.1);
    a->raster_hz=a->sync_locked && a->decoder->vsync_period>0?
        a->decoder->rate/a->decoder->vsync_period:0;
    int synced=a->decoder->hsync_updates && a->decoder->vsync_updates &&
        a->decoder->fields-a->decoder->hsync_field<=2 &&
        a->decoder->tick-a->decoder->vsync_confirmed<a->decoder->rate*.060;
    int live_rolling=a->stacking && a->stack.rolling && a->c.rate<=10e6;
    if(!live_rolling && !a->stack_paused && a->decoder->free_sync &&
       ((a->decoder->hsync_updates && !a->stack_hsync) ||
        (a->decoder->vsync_epoch && a->decoder->vsync_epoch!=a->stack_vsync) ||
        (synced && !a->stack_sync_input))) {
        /* An arbitrary free-running reference would retain its sync stripe.
         * Start a new reference in the recovered active-video coordinates. */
        reset_stack(a,a->stack.target,a->stack.rolling);
        a->stack_hsync=a->decoder->hsync_updates!=0;
        a->stack_vsync=a->decoder->vsync_epoch;
    }
    if(!a->stack_paused) a->stack_sync_input=synced;
    double line_hz=a->decoder->free_period ? a->decoder->rate/a->decoder->free_period : a->decoder->line_hz;
    a->stack.period_x=WIDTH/(52e-6*line_hz);
    a->stack.period_y=line_hz*(a->decoder->vsync_period ? a->decoder->vsync_period/a->decoder->rate : .020);
    int valid=a->decoder->stack_valid, clear_pending=0;
    const char *reason=valid ? NULL : "INCOMPLETE FRAME";
    /* Sync improves raster coordinates, but is not an admission requirement.
     * During fades the clock coasts and image registration supplies alignment.
     * Requiring fresh sync here prevented a weak reference from ever being
     * confirmed once horizontal sync had been acquired even once. Keep the
     * completeness guard (also rejects acquisition transients) and let the
     * stack's structure/match checks reject noise. */
    if(!valid && !a->stack.count) clear_pending=1;
    if(a->light_stacking || live_rolling) {
        if(!a->decoder->preview) queue_light_frame(a,a->live);
    }
    else if(a->stacking && !a->command && !a->stack_paused && !SDL_AtomicGet(&a->failed)) {
        if(a->stack_pending) ++a->stack_skipped;
        memcpy(a->queued.pixels,pixels,sizeof(a->queued.pixels));
        a->queued.lightweight=0;
        /* Contest stacking remains luminance-only; the live image keeps RGB. */
        if(a->decoder->color) for(int k=0;k<WIDTH*HEIGHT;++k) {
            unsigned p=pixels[k];
            unsigned y=(77*((p>>16)&255)+150*((p>>8)&255)+29*(p&255)+128)>>8;
            a->queued.pixels[k]=0xff000000u|y*0x010101u;
        }
        a->queued.guided=a->decoder->guided;
        a->queued.synced=synced;
        if(a->queued.guided) memcpy(a->queued.guide,a->decoder->guide_pixels,sizeof(a->queued.guide));
        a->queued.valid=valid; a->queued.phase=a->decoder->stack_phase;
        a->queued.clear_pending=clear_pending;
        a->queued.reason=reason;
        a->queued.period_x=a->stack.period_x; a->queued.period_y=a->stack.period_y;
        a->queued.serial=(uint64_t)a->frames+1;
        a->stack_pending=1;
        SDL_CondSignal(a->stack_ready);
    }
    a->last_frame = frame_tick; ++a->frames;
    SDL_UnlockMutex(a->mutex);
}
static void init_decoders(App *a, Decoder *d, Decoder *preview,
                          int invert, int free_sync, double line_hz, int vertical_shift, double if_bw, int deemphasis, int experimental_sync, int fm_mode)
{
    decoder_init(d,a->c.rate,a->c.deviation,invert,frame,a);
    /* Both display modes use the adaptive sync clock, which keeps producing
     * rasters while searching/coasting. F only selects the independent live
     * preview versus the sync-following raster, not the legacy pulse gate. */
    d->experimental_sync=experimental_sync;
    d->multi_candidate_sync=fm_mode==2;
    d->free_sync=1; d->line_hz=line_hz; d->vertical_shift=vertical_shift;
    if(a->c.alignment_bandwidth>0)
        d->guide_stride=(unsigned)fmax(1,floor(a->c.rate/fmax(4e6,2*a->c.alignment_bandwidth)));
    decoder_init(preview,a->c.rate,a->c.deviation,invert,live_frame,a);
    preview->free_sync=1; preview->line_hz=line_hz; preview->vertical_shift=vertical_shift;
    if(free_sync) d->preview=preview;
    /* For live SDR above the contest rate no stack consumes the guide image. In free
     * run only the independent live raster is visible; keep H/V recovery. */
    d->color=preview->color=color_enabled(a->c.rate,if_bw);
    pal_init(&d->pal,a->c.rate);
    d->video_deemphasis=deemphasis;
    d->no_guide_raster=stacking_limited(&a->c);
    d->no_video_raster=stacking_limited(&a->c) && free_sync;
}
typedef struct {
    App *app;
    Sound *sound;
    StereoSound *stereo;
    int source, swap;
    Nicam *nicam;
    unsigned queue_resets, queue_refills;
    unsigned reserve_frames;
    Uint64 last_supply, supply_ticks, supply_frames, max_supply_gap;
    int started, failed;
    SDL_Thread *thread;
    SDL_mutex *mutex;
    SDL_cond *ready;
    float *queue, *scratch;
    size_t *lengths, block, slots, head, count, queued_samples;
    int busy, stop, gap;
    double bandwidth;
    uint64_t discarded;
    unsigned input_overflows;
} Playback;
static void play_stereo(void *opaque, const float *samples, size_t n)
{
    Playback *p=opaque;
    SDL_AudioDeviceID device=p->app->audio_device;
    if(!device || p->failed || !n) return;
    Uint64 now=SDL_GetPerformanceCounter();
    if(p->last_supply) {
        Uint64 gap=now-p->last_supply;
        p->supply_ticks+=gap; p->supply_frames+=n;
        if(gap>p->max_supply_gap) {
            p->max_supply_gap=gap;
            SDL_AtomicSet(&p->app->playback_max_gap_us,(int)fmin(2147483647.0,1e6*gap/SDL_GetPerformanceFrequency()));
        }
    }
    p->last_supply=now;
    if(!p->reserve_frames) p->reserve_frames=3840; /* Initially 80 ms. */
    /* An empty SDL queue signals starvation risk, not necessarily a physical
     * underrun: the device may still hold its last block. Increase the reserve
     * by 40 ms on each refill, capped at 240 ms, to tolerate recurring bursts. */
    Uint32 queued=SDL_GetQueuedAudioSize(device);
    if(p->started && !queued) {
        SDL_PauseAudioDevice(device,1); p->started=0; ++p->queue_refills;
        SDL_AtomicSet(&p->app->playback_refills,(int)p->queue_refills);
        if(p->reserve_frames<11520) p->reserve_frames+=1920;
    }
    /* Bound latency even if the device stops consuming or its clock drifts.
     * After a flush, refill before resuming playback as at initial startup. */
    if(queued>(p->reserve_frames+5760)*2*sizeof(float)) {
        SDL_PauseAudioDevice(device,1); p->started=0;
        SDL_ClearQueuedAudio(device); ++p->queue_resets;
        SDL_AtomicSet(&p->app->playback_resets,(int)p->queue_resets);
    }
    float silence[512]={0};
    if(SDL_QueueAudio(device,SDL_AtomicGet(&p->app->audio_muted)?silence:samples,n*2*sizeof(float))) {
        fprintf(stderr,"Audio playback: %s\n",SDL_GetError()); p->failed=1; return;
    }
    if(!p->started && SDL_GetQueuedAudioSize(device)>=p->reserve_frames*2*sizeof(float)) {
        SDL_PauseAudioDevice(device,0); p->started=1;
    }
}
static void play_audio(void *opaque, const float *samples, size_t n)
{
    Playback *p=opaque;
    if(p->source!=0) return;
    float stereo[512];
    for(size_t k=0;k<n;++k) stereo[2*k]=stereo[2*k+1]=samples[k];
    play_stereo(p,stereo,n);
}
static void process_audio(void *opaque, const float *samples, size_t n)
{
    Playback *p=opaque;
    /* Keep carrier indicators current independently of the playback source.
     * play_audio already forwards mono samples only when source == 0. */
    sound_process(p->sound,samples,n);
    SDL_AtomicSet(&p->app->mono_detect,sound_locked(p->sound));
    int stereo_enabled=stereo_sound_enabled(p->app->c.rate,p->bandwidth);
    if(stereo_enabled) stereo_sound_monitor(p->stereo,samples,n,p->source>=2);
    SDL_AtomicSet(&p->app->stereo_detect,stereo_enabled?(int)stereo_sound_detected(p->stereo):0);
    SDL_AtomicSet(&p->app->audio_locked,p->source>=2?(stereo_enabled && stereo_sound_locked(p->stereo)):p->source==0 && sound_locked(p->sound));
    if(p->nicam) {
        nicam_process(p->nicam,samples,n,nicam_active(p->nicam)?0:sound_power(p->sound));
        SDL_AtomicSet(&p->app->nicam_detect,nicam_detected(p->nicam));
        SDL_AtomicSet(&p->app->nicam_lock,nicam_locked(p->nicam));
        NicamStats stats; nicam_stats(p->nicam,&stats);
        SDL_AtomicSet(&p->app->nicam_good_frames,(int)stats.good_frames);
        SDL_AtomicSet(&p->app->nicam_bad_frames,(int)stats.bad_frames);
        SDL_AtomicSet(&p->app->nicam_muted_frames,(int)stats.muted_frames);
        SDL_AtomicSet(&p->app->nicam_sync_bits,(int)stats.sync_bits_discarded);
        SDL_AtomicSet(&p->app->nicam_queue_overflows,(int)nicam_overflows(p->nicam));
    }
    SDL_AtomicSet(&p->app->audio_metrics_tick,(int)SDL_GetTicks());
}
/* Called only with the audio worker idle, or by that worker after a gap. */
static void reset_audio_state(Playback *p)
{
    stereo_sound_reset(p->stereo,p->source==3,p->swap);
    if(p->nicam) { nicam_select(p->nicam,0); nicam_reset(p->nicam); }
    SDL_AtomicSet(&p->app->nicam_detect,0); SDL_AtomicSet(&p->app->nicam_lock,0);
    SDL_AtomicSet(&p->app->mono_detect,0); SDL_AtomicSet(&p->app->audio_metrics_tick,0);
    SDL_AtomicSet(&p->app->stereo_detect,0);
    if(p->sound) sound_reset(p->sound);
    if(p->app->audio_device) {
        SDL_PauseAudioDevice(p->app->audio_device,1);
        SDL_ClearQueuedAudio(p->app->audio_device);
    }
    p->started=0;
    /* Deliberate tuning/source/pause resets are not audio supply stalls.
     * Retain the learned reserve; measure only continuous supply intervals. */
    p->last_supply=0;
    if(p->nicam && sound_enabled(p->app->c.rate,p->bandwidth) && p->source==1)
        if(nicam_select(p->nicam,1)) fprintf(stderr,"Cannot start NICAM decoder: %s\n",SDL_GetError());
    SDL_AtomicSet(&p->app->audio_locked,0);
}
static int audio_worker(void *opaque)
{
    Playback *p=opaque;
    SDL_LockMutex(p->mutex);
    for(;;) {
        while(!p->count && !p->stop) SDL_CondWait(p->ready,p->mutex);
        if(!p->count && p->stop) break;
        size_t n=p->lengths[p->head];
        memcpy(p->scratch,p->queue+p->head*p->block,n*sizeof(float));
        p->head=(p->head+1)%p->slots; --p->count;
        p->queued_samples-=n; SDL_AtomicSet(&p->app->audio_dsp_queued,(int)p->queued_samples);
        int gap=p->gap; p->gap=0; p->busy=1;
        SDL_UnlockMutex(p->mutex);
        if(gap) reset_audio_state(p);
        process_audio(p,p->scratch,n);
        SDL_LockMutex(p->mutex);
        p->busy=0; SDL_CondBroadcast(p->ready);
    }
    SDL_UnlockMutex(p->mutex);
    return 0;
}
/* Producer only copies raw FM, before video de-emphasis. Never wait for DSP. */
static void enqueue_audio(void *opaque,const float *samples,size_t n)
{
    Playback *p=opaque;
    SDL_LockMutex(p->mutex);
    if(p->count==p->slots) {
        for(size_t k=0;k<p->count;++k) p->discarded+=p->lengths[(p->head+k)%p->slots];
        p->head=p->count=p->queued_samples=0; p->gap=1; ++p->input_overflows;
        SDL_AtomicSet(&p->app->audio_dsp_overflows,(int)p->input_overflows);
    }
    size_t tail=(p->head+p->count)%p->slots;
    memcpy(p->queue+tail*p->block,samples,n*sizeof(float));
    p->lengths[tail]=n; ++p->count; p->queued_samples+=n;
    SDL_AtomicSet(&p->app->audio_dsp_queued,(int)p->queued_samples);
    SDL_CondSignal(p->ready); SDL_UnlockMutex(p->mutex);
}
/* Producer must be idle. Discard on control changes, drain at EOF. */
static void audio_idle(Playback *p,int discard)
{
    if(!p->thread) return;
    SDL_LockMutex(p->mutex);
    if(discard) {
        p->head=p->count=p->queued_samples=0; p->gap=0;
        SDL_AtomicSet(&p->app->audio_dsp_queued,0);
    }
    while(p->count || p->busy) SDL_CondWait(p->ready,p->mutex);
    SDL_UnlockMutex(p->mutex);
}
static void stop_audio_worker(Playback *p)
{
    if(p->thread) {
        SDL_LockMutex(p->mutex); p->stop=1;
        SDL_CondBroadcast(p->ready); SDL_UnlockMutex(p->mutex);
        SDL_WaitThread(p->thread,NULL); p->thread=NULL;
    }
    if(p->ready) SDL_DestroyCond(p->ready);
    if(p->mutex) SDL_DestroyMutex(p->mutex);
    free(p->queue); free(p->scratch); free(p->lengths);
    p->ready=NULL; p->mutex=NULL; p->queue=p->scratch=NULL; p->lengths=NULL;
}
static int start_audio_worker(Playback *p,size_t block)
{
    p->block=block;
    p->slots=(size_t)ceil(p->app->c.rate*.1/block);
    if(!p->slots) p->slots=1;
    p->queue=calloc(p->slots,block*sizeof(float));
    p->scratch=malloc(block*sizeof(float)); p->lengths=calloc(p->slots,sizeof(size_t));
    p->mutex=SDL_CreateMutex(); p->ready=SDL_CreateCond();
    if(!p->queue || !p->scratch || !p->lengths || !p->mutex || !p->ready) goto fail;
    p->thread=SDL_CreateThread(audio_worker,"Audio-DSP",p);
    if(!p->thread) goto fail;
    return 0;
fail:
    stop_audio_worker(p); return -1;
}
static void reset_audio(Playback *p, Alignment *video, double bandwidth)
{
    alignment_wait(video);
    audio_idle(p,1);
    p->bandwidth=bandwidth;
    p->source=SDL_AtomicGet(&p->app->audio_source);
    p->swap=SDL_AtomicGet(&p->app->audio_swap);
    reset_audio_state(p);
    alignment_set_consumer(video,p->thread && p->sound && sound_enabled(p->app->c.rate,bandwidth)?enqueue_audio:NULL,p);
}

/* Drain a pending block before a discontinuity, control change or EOF. */
static void finish_video(PalPipeline *pal, Decoder *d, Alignment *video, Alignment *guide, size_t *pending)
{
    if(!*pending) return;
    const float *v=alignment_wait(video);
    const float *g=guide?alignment_wait(guide):NULL;
    pal_pipeline_decode(pal,d,v,g,*pending);
    *pending=0;
}
static void publish_video_quality(App *a,const VideoQuality *q)
{
    SDL_LockMutex(a->mutex);
    if(a->quality.sequence!=q->reading.sequence) a->quality_tick=SDL_GetTicks();
    a->quality=q->reading;
    SDL_UnlockMutex(a->mutex);
}
static int quality_sync_valid(const Decoder *d)
{
    return d->hsync_updates>=2 && d->vsync_updates &&
        d->fields-d->hsync_field<=2 && d->tick-d->vsync_confirmed<d->rate*.06;
}

static int worker(void *opaque)
{
    App *a = opaque;
    Playback audio={.app=a};
    VideoQuality quality; video_quality_init(&quality,a->c.rate);
    VideoScope scope; video_scope_init(&scope,a->c.rate,a->c.line_hz);
    int scope_epoch=-1;
    IQ *iq = malloc(a->c.rx_block * sizeof(*iq));
    Decoder *d = malloc(sizeof(*d));
    Decoder *preview = malloc(sizeof(*preview));
    fftwf_complex *in = fftwf_alloc_complex(FFT_SIZE), *out = fftwf_alloc_complex(FFT_SIZE);
    fftwf_plan plan = NULL;
    Alignment *video_path=NULL;
    size_t pending=0;
    Alignment *alignment=NULL;
    PalPipeline *pal=NULL;
    FILE *record_file=NULL;
    IQ *recorded=NULL;
    size_t record_count=0,record_capacity=0;
    if(a->c.record) {
        record_file=fopen(a->c.record,"wbx");
        if(!record_file) { perror("IQ recording"); goto fail; }
        record_capacity=(size_t)llround(a->c.rate*a->c.record_seconds);
        recorded=malloc(record_capacity*sizeof(*recorded));
        if(!recorded) { fprintf(stderr,"Cannot allocate IQ recording buffer\n"); goto fail; }
        /* Touch pages before receiving, avoiding page faults during capture. */
        memset(recorded,0,record_capacity*sizeof(*recorded));
    }
    if (!iq || !d || !preview || !in || !out) goto fail;
    plan = fftwf_plan_dft_1d(FFT_SIZE, in, out, FFTW_FORWARD, FFTW_ESTIMATE);
    if (!plan) goto fail;
    SDL_LockMutex(a->mutex);
    int initial_vertical_shift=a->vertical_shift;
    SDL_UnlockMutex(a->mutex);
    init_decoders(a,d,preview,a->c.invert,a->c.free_sync,a->c.line_hz,initial_vertical_shift,a->c.if_bandwidth,a->c.video_deemphasis,a->c.experimental_sync,a->c.fm_mode);
    a->decoder=d;
    if(d->color && a->c.rx_block>PAL_CHUNK && !(pal=pal_pipeline_create())) goto fail;
    video_path=alignment_create_video(a->c.rate,a->c.if_bandwidth,a->c.deviation,a->c.invert,a->c.rx_block,d->color);
    if(video_path && a->c.video_deemphasis && alignment_set_deemphasis(video_path)) {
        alignment_destroy(video_path); video_path=NULL;
    }
    if(!video_path) goto fail;
    alignment_set_dc_block(video_path,iq_dc_block_enabled(&a->c));
    if(a->c.rate>=14e6) {
        audio.sound=sound_create(a->c.rate,play_audio,&audio);
        if(a->c.rate>=16e6) audio.stereo=stereo_sound_create(a->c.rate,play_stereo,&audio);
        audio.nicam=nicam_create(a->c.rate,play_stereo,&audio);
        if(!audio.sound || !audio.nicam || (a->c.rate>=16e6 && !audio.stereo)) goto fail;
        if(start_audio_worker(&audio,a->c.rx_block)) goto fail;
        reset_audio(&audio,video_path,a->c.if_bandwidth);
        fprintf(stderr,"Audio: 6 MHz mono / NICAM (rate and IF >= 14 MHz), 7.02/7.22 or 7.38/7.56 MHz stereo (>= 16 MHz); A mute, N source, Shift+N swap L/R\n");
    }
    if(a->c.experimental_sync) fprintf(stderr,"Experimental sync: echo-resistant line timing enabled\n");
    if(a->c.video_deemphasis) fprintf(stderr,"Video de-emphasis: PAL ITU-R F.405-1, linear phase; raw FM/NICAM audio preserved\n");
    if(iq_dc_block_enabled(&a->c)) fprintf(stderr,"IQ DC rejection: 10 Hz, video and sync paths; raw IQ preserved; level measurements exclude DC\n");
    fprintf(stderr,"Video IF/FM: pipelined thread; PAL color automatic for IF >= 10 MHz at >= 12 MS/s\n");
    if(a->c.alignment_bandwidth>0) {
        alignment=alignment_create(a->c.rate,a->c.alignment_bandwidth,a->c.deviation,a->c.invert,a->c.rx_block);
        if(!alignment) goto fail;
        alignment_set_dc_block(alignment,iq_dc_block_enabled(&a->c));
        fprintf(stderr,"Alignment IF/FM: dedicated thread, sample-aligned blocks\n");
    }
    alignment_set_fm_mode(video_path,a->c.fm_mode,a->c.invert);
    if(alignment) alignment_set_fm_mode(alignment,a->c.fm_mode,a->c.invert);
    fprintf(stderr,"FM%d detector: video and alignment; D cycles FM0/FM1/FM2\n",a->c.fm_mode);
    AFC afc; afc_reset(&afc,a->c.rate,a->c.line_hz);
    unsigned afc_epoch=0;
    double applied_if = a->c.if_bandwidth;
    int applied_deemphasis=a->c.video_deemphasis;
    uint64_t count = 0, next_fft = 0;
    SpectrumAverage spectrum_average={0};
    uint32_t start = SDL_GetTicks();
    BlockTiming pending_timing={0};
    Clipping clipping; clipping_init(&clipping,a->c.mode);
    while (!SDL_AtomicGet(&a->stop)) {
        SDL_LockMutex(a->mutex);
        if(a->info_paused) {
            pending_timing=(BlockTiming){0};
            iq_recorder_finish(a->recorder,"paused");
            uint32_t pause_start=SDL_GetTicks();
            double pause_if=a->if_bandwidth;
            SDL_UnlockMutex(a->mutex);
            finish_video(pal,d,video_path,alignment,&pending);
            source_pause(a->source);
            reset_audio(&audio,video_path,pause_if);
            SDL_LockMutex(a->mutex);
            while(a->info_paused && !SDL_AtomicGet(&a->stop))
                SDL_CondWait(a->stack_ready,a->mutex);
            start+=SDL_GetTicks()-pause_start;
            spectrum_average.valid=0;
            if(strcmp(a->c.mode,"demo") && strcmp(a->c.mode,"file")) {
                /* A live source resumes at the current signal, after a gap. */
                a->command|=2;
                reset_stack(a,a->stack.target,a->stack.rolling);
                if(recorded && record_count<record_capacity) record_count=0;
            }
        }
        if(SDL_AtomicGet(&a->stop)) { SDL_UnlockMutex(a->mutex); break; }
        int command = a->command, invert = a->invert, free_sync=a->free_sync, vertical_shift=a->vertical_shift;
        int afc_enabled=a->afc_enabled, fm_mode=a->fm_mode, deemphasis=a->video_deemphasis, experimental_sync=a->experimental_sync;
        unsigned epoch=a->afc_epoch;
        double if_bw=a->if_bandwidth, line_hz=a->line_hz;
        double hz = a->requested_hz, gain = a->requested_gain;
        a->command = 0;
        SDL_UnlockMutex(a->mutex);
        if(epoch!=afc_epoch) { afc_reset(&afc,a->c.rate,line_hz); afc_epoch=epoch; }
        if(command) pending_timing=(BlockTiming){0};
        if(command & 4) {
            finish_video(pal,d,video_path,alignment,&pending);
            reset_audio(&audio,video_path,if_bw);
            command&=~4;
        }
        if (command) {
            if(command & 9) spectrum_average.valid=0;
            iq_recorder_finish(a->recorder,"settings_changed");
            finish_video(pal,d,video_path,alignment,&pending);
            afc_reset(&afc,a->c.rate,line_hz);
            if(!strcmp(a->c.mode,"sdrplay")) hz=fmin(2e9,fmax(1000,hz));
            if ((command & 9) && source_tune(a->source, &hz, &gain)) goto fail;
            if((command & 9) && recorded && record_count<record_capacity) {
                record_count=0;
                fprintf(stderr,"IQ recording: restarting after tuning/gain change\n");
            }
            SDL_LockMutex(a->mutex);
            a->actual_hz = hz; a->actual_gain = gain;
            if(!(a->command&9)) { a->requested_hz=hz; a->requested_gain=gain; }
            a->last_frame = 0;
            reset_stack(a, a->stack.target, a->stack.rolling);
            a->last_stacked = 0;
            a->stack_hsync = 0;
            a->stack_vsync = 0;
            /* Keep the last image visible until a new contribution arrives. */
            SDL_UnlockMutex(a->mutex);
            video_quality_reset(&quality);
            publish_video_quality(a,&quality);
            video_scope_reset(&scope,line_hz);
            SDL_LockMutex(a->mutex); memset(&a->scope,0,sizeof(a->scope)); a->scope_tick=0; SDL_UnlockMutex(a->mutex);
            init_decoders(a,d,preview,invert,free_sync,line_hz,vertical_shift,if_bw,deemphasis,experimental_sync,fm_mode);
            if(d->color && a->c.rx_block>PAL_CHUNK && !pal && !(pal=pal_pipeline_create())) goto fail;
            if(alignment) alignment_reset(alignment,invert);
            if (if_bw != applied_if || deemphasis != applied_deemphasis) {
                alignment_destroy(video_path);
                video_path=alignment_create_video(a->c.rate,if_bw,a->c.deviation,invert,a->c.rx_block,d->color);
                if(video_path && deemphasis && alignment_set_deemphasis(video_path)) {
                    alignment_destroy(video_path); video_path=NULL;
                }
                if(!video_path) goto fail;
                alignment_set_dc_block(video_path,iq_dc_block_enabled(&a->c));
                applied_if=if_bw;
                applied_deemphasis=deemphasis;
            } else alignment_reset(video_path,invert);
            alignment_set_fm_mode(video_path,fm_mode,invert);
            if(alignment) alignment_set_fm_mode(alignment,fm_mode,invert);
            reset_audio(&audio,video_path,if_bw);
        }
        uint64_t wait_ticks,read_start=SDL_GetPerformanceCounter();
        int n=source_read_timed(a->source,iq,a->c.rx_block,&wait_ticks);
        uint64_t processing_start=SDL_GetPerformanceCounter();
        pending_timing.wait_ticks+=wait_ticks;
        pending_timing.read_overhead_ticks+=processing_start-read_start-wait_ticks;
        if(n<=0) finish_video(pal,d,video_path,alignment,&pending);
        if (n == -2 || n == -3) {
            pending_timing=(BlockTiming){0};
            spectrum_average.valid=0;
            iq_recorder_finish(a->recorder,"input_discontinuity");
            video_quality_reset(&quality); publish_video_quality(a,&quality);
            video_scope_reset(&scope,line_hz);
            SDL_LockMutex(a->mutex); memset(&a->scope,0,sizeof(a->scope)); a->scope_tick=0; SDL_UnlockMutex(a->mutex);
            if(recorded && record_count<record_capacity) {
                record_count=0;
                fprintf(stderr,"IQ recording: restarting after input discontinuity\n");
            }
            SDL_LockMutex(a->mutex);
            if(n==-3)
                while((a->stack_pending || a->stack_busy) && !SDL_AtomicGet(&a->stop) && !SDL_AtomicGet(&a->failed))
                    SDL_CondWait(a->stack_ready,a->mutex);
            if(n==-2) ++a->overflows;
            a->afc_state=0;
            a->last_frame = 0; interrupt_stack(a);
            SDL_UnlockMutex(a->mutex);
            init_decoders(a,d,preview,invert,free_sync,line_hz,vertical_shift,if_bw,deemphasis,experimental_sync,fm_mode);
            afc_reset(&afc,a->c.rate,line_hz);
            alignment_reset(video_path,invert);
            reset_audio(&audio,video_path,if_bw);
            if(alignment) alignment_reset(alignment,invert);
            if(n==-3) { count=0; next_fft=0; start=SDL_GetTicks(); }
            continue;
        }
        if (n < 0) goto fail;
        if (!n) {
            ++pending_timing.empty_reads;
            pending_timing.processing_ticks+=SDL_GetPerformanceCounter()-processing_start;
            if (!strcmp(a->c.mode, "file")) { audio_idle(&audio,0); nicam_finish(audio.nicam); SDL_AtomicSet(&a->nicam_lock,nicam_locked(audio.nicam)); break; }
            continue;
        }
        clipping_process(&clipping,iq,(size_t)n,SDL_GetTicks());
        iq_recorder_push(a->recorder,iq,(size_t)n);
        count += n;
        if(recorded && record_count<record_capacity) {
            size_t take=record_capacity-record_count;
            if(take>(size_t)n) take=(size_t)n;
            memcpy(recorded+record_count,iq,take*sizeof(*iq));
            record_count+=take;
            if(record_count==record_capacity)
                fprintf(stderr,"IQ recording captured %.3f seconds; saving on exit\n",record_count/a->c.rate);
        }
        if (count >= next_fft && n >= FFT_SIZE) {
            double power=iq_ac_power_dbfs(iq,(size_t)n);
            for (int j = 0; j < FFT_SIZE; ++j) {
                float w = .5f - .5f*cosf(2*PI*j/(FFT_SIZE-1));
                in[j][0] = iq[j].i*w; in[j][1] = iq[j].q*w;
            }
            fftwf_execute(plan);
            SourceStats metrics;
            source_get_stats(a->source,&metrics);
            SDL_LockMutex(a->mutex);
            a->rx_stats=metrics; a->metrics_tick=SDL_GetTicks(); a->consumed_samples=count;
            a->clipping=clipping;
            for (int j = 0; j < FFT_SIZE; ++j) {
                int k = (j + FFT_SIZE/2) % FFT_SIZE;
                float p = (out[k][0]*out[k][0]+out[k][1]*out[k][1]) / (FFT_SIZE*FFT_SIZE*.25f);
                a->spectrum[j] = p;
            }
            spectrum_average_update(&spectrum_average,a->spectrum,count,a->c.rate);
            a->power = power;
            SDL_UnlockMutex(a->mutex);
            next_fft = count + (uint64_t)(a->c.rate / 25);
        }
        /* Both branches can start N+1 as soon as their own N completes.
         * The preceding outputs remain owned by the reader until next submit. */
        if(alignment_queue(video_path,iq,(size_t)n)) goto fail;
        if(alignment && alignment_queue(alignment,iq,(size_t)n)) goto fail;
        size_t ready=pending;
        pending=(size_t)n;
        const float *video=NULL,*guide=NULL;
        if(ready) {
            uint32_t measured_at;
            double if_power;
            video=alignment_previous(video_path,&if_power,&measured_at);
            if(alignment) guide=alignment_previous(alignment,NULL,NULL);
            SDL_LockMutex(a->mutex);
            a->if_power_dbfs=if_power; a->if_power_tick=measured_at;
            SDL_UnlockMutex(a->mutex);
        }
        if(ready) {
            uint64_t first_tick=d->tick;
            if(SDL_AtomicGet(&a->scope_enabled)) {
                int epoch_now=SDL_AtomicGet(&a->scope_epoch);
                if(scope_epoch!=epoch_now) {
                    scope.timebase=SDL_AtomicGet(&a->scope_timebase);
                    video_scope_reset(&scope,line_hz);
                    scope_epoch=epoch_now;
                }
                if(video_scope_process(&scope,video,ready)) {
                    SDL_LockMutex(a->mutex);
                    if(SDL_AtomicGet(&a->scope_enabled) && SDL_AtomicGet(&a->scope_epoch)==epoch_now) {
                        a->scope=scope.reading; a->scope_tick=SDL_GetTicks();
                    }
                    SDL_UnlockMutex(a->mutex);
                }
            }
            pal_pipeline_decode(pal,d,video,guide,ready);
            video_quality_process(&quality,video,ready,first_tick,quality_sync_valid(d),
                d->free_origin,d->free_period>0?d->free_period:d->rate/d->line_hz,
                (double)d->field,d->vsync_period>0?d->vsync_period:d->rate/50);
            publish_video_quality(a,&quality);
        }
        if(ready && afc_enabled) {
            double error=0;
            /* PAL de-emphasis amplifies DC (and tuning error) by 11 dB. */
            double hz_per_unit=a->c.deviation/(applied_deemphasis?pow(10,11.0/20):1);
            int measured=afc_measure(&afc,video,ready,hz_per_unit,invert,&error);
            if(measured) {
                SDL_LockMutex(a->mutex);
                if(a->afc_enabled && a->afc_epoch==epoch && !a->command) {
                    int limited=0;
                    double target=a->actual_hz;
                    if(measured>0) target=afc_target(a->afc_anchor,a->actual_hz,error,&limited);
                    a->afc_state=measured<0?0:limited?2:1;
                    if(target!=a->actual_hz) { a->requested_hz=target; a->command|=8; }
                }
                SDL_UnlockMutex(a->mutex);
            }
        }
        /* Publish matched times only on completed blocks; a 100 ms queue
         * timeout and its pipeline flush belong to the next delivery. */
        pending_timing.processing_ticks+=SDL_GetPerformanceCounter()-processing_start;
        SDL_LockMutex(a->mutex);
        ++a->timing.blocks;
        a->timing.wait_ticks+=pending_timing.wait_ticks;
        a->timing.read_overhead_ticks+=pending_timing.read_overhead_ticks;
        a->timing.processing_ticks+=pending_timing.processing_ticks;
        a->timing.empty_reads+=pending_timing.empty_reads;
        SDL_UnlockMutex(a->mutex);
        pending_timing=(BlockTiming){0};
        if (!strcmp(a->c.mode, "demo") || !strcmp(a->c.mode, "file")) {
            double ahead = 1000*count/a->c.rate - (SDL_GetTicks()-start);
            if (ahead > 1) SDL_Delay((uint32_t)fmin(ahead, 20));
        }
    }
    SDL_LockMutex(a->mutex); a->clipping=clipping; SDL_UnlockMutex(a->mutex);
    goto cleanup;
fail:
    SDL_AtomicSet(&a->failed, 1);
cleanup:
    iq_recorder_finish(a->recorder,"end_of_input");
    source_stop(a->source);
    SourceStats rx_stats;
    source_get_stats(a->source,&rx_stats);
    if(rx_stats.capacity_samples)
        fprintf(stderr,"SDR receive: hardware overflows %llu; IQ buffer peak %.1f / %.1f ms; discarded samples %llu\n",
            (unsigned long long)rx_stats.hardware_overflows,
            1000.0*rx_stats.peak_samples/a->c.rate,1000.0*rx_stats.capacity_samples/a->c.rate,
            (unsigned long long)rx_stats.discarded_samples);
    if(record_file) {
        int ok=!record_count || fwrite(recorded,sizeof(*recorded),record_count,record_file)==record_count;
        if(fclose(record_file)) ok=0;
        if(!ok) { perror("Save IQ recording"); SDL_AtomicSet(&a->failed,1); }
        else fprintf(stderr,"IQ recording: %s, %zu samples, %.0f samples/s, native cf32, %.3f seconds%s\n",
            a->c.record,record_count,a->c.rate,record_count/a->c.rate,
            record_count<record_capacity?" (partial)":"");
    }
    if(d && a->decoder==d && d->color) fprintf(stderr,"PAL burst updates: %u; color lock: %s\n",d->pal.burst_updates,d->pal.locked?"yes":"no");
    free(recorded);
    alignment_destroy(alignment); alignment_destroy(video_path); pal_pipeline_destroy(pal);
    stop_audio_worker(&audio);
    nicam_finish(audio.nicam);
    if(audio.sound) fprintf(stderr,"Audio DSP: input queue overflows %u; discarded samples %llu\n",audio.input_overflows,(unsigned long long)audio.discarded);
    if(audio.sound) fprintf(stderr,"Audio 6 MHz: carrier %s; playback queue resets %u; playback buffer refills %u\n",
        sound_locked(audio.sound)?"locked":"not detected",audio.queue_resets,audio.queue_refills);
    if(audio.supply_ticks) fprintf(stderr,"Audio playback: reserve %.0f ms; largest supply gap %.1f ms; average supply %.0f frames/s (target 48000)\n",
        audio.reserve_frames/48.0,1000.0*audio.max_supply_gap/SDL_GetPerformanceFrequency(),
        (double)audio.supply_frames*SDL_GetPerformanceFrequency()/audio.supply_ticks);
    if(audio.nicam) fprintf(stderr,"NICAM: selected %s; lock %s; queue overflows %u\n",
        nicam_active(audio.nicam)?"yes":"no",nicam_locked(audio.nicam)?"yes":"no",nicam_overflows(audio.nicam));
    nicam_destroy(audio.nicam); sound_destroy(audio.sound); stereo_sound_destroy(audio.stereo);
    if (plan) fftwf_destroy_plan(plan);
    fftwf_free(in); fftwf_free(out); free(iq); free(d); free(preview);
    SDL_LockMutex(a->mutex);
    SDL_AtomicSet(&a->done, 1);
    SDL_CondBroadcast(a->stack_ready);
    SDL_UnlockMutex(a->mutex);
    return 0;
}
static void text(SDL_Renderer *r, int x, int y, const char *str, int scale)
{
    ui_text_draw(r,x,y,str,scale);
}

static void detection_bar(SDL_Renderer *r,int y,const char *label,int enabled,int detected)
{
    SDL_Rect bar={10,y,UI_SIDE_WIDTH-20,44};
    if(!enabled) SDL_SetRenderDrawColor(r,65,72,82,255);
    else if(detected) SDL_SetRenderDrawColor(r,22,130,72,255);
    else SDL_SetRenderDrawColor(r,24,83,170,255);
    SDL_RenderFillRect(r,&bar);
    SDL_SetRenderDrawColor(r,enabled?255:165,enabled?255:172,enabled?255:182,255);
    text(r,bar.x+8,y+7,label,2);
    text(r,bar.x+8,y+25,!enabled?"UNAVAILABLE":detected?"DETECTED":"SEEKING",1);
}

enum {
    CONTROL_IF, CONTROL_AFC, CONTROL_FM,
    CONTROL_FREE_SYNC, CONTROL_EXP_SYNC, CONTROL_INVERSE, CONTROL_DEEMPH, CONTROL_SCOPE, CONTROL_TIMEBASE,
    CONTROL_STACK, CONTROL_STACK_MODE, CONTROL_STACK_FRAMES, CONTROL_STACK_RESET, CONTROL_STACK_PAUSE,
    CONTROL_SCREENSHOT, CONTROL_IQ, CONTROL_DIAG,
    CONTROL_AUDIO, CONTROL_MUTE, CONTROL_SWAP,
    CONTROL_SAVE, CONTROL_SETUP, CONTROL_STATION, CONTROL_INFO, CONTROL_COUNT
};
static const struct { const char *title; unsigned count; } control_groups[]={
    {"TUNING",3}, {"VIDEO",6}, {"STACKING",5},
    {"RECORDING",3}, {"AUDIO",3}, {"SETTINGS",4}
};
static const struct { SDL_Keycode key; SDL_Keymod modifiers; const char *label; } controls[]={
    {SDLK_RIGHTBRACKET,KMOD_NONE,"[ ] VIDEO IF"}, {SDLK_c,KMOD_NONE,"C AFC"}, {SDLK_d,KMOD_NONE,"D FM MODE"},
    {SDLK_f,KMOD_NONE,"F FREE SYNC"}, {SDLK_g,KMOD_NONE,"G EXP SYNC"}, {SDLK_i,KMOD_NONE,"I INVERSE"},
    {SDLK_e,KMOD_NONE,"E DE-EMPH"}, {SDLK_o,KMOD_NONE,"O SCOPE"}, {SDLK_PAGEUP,KMOD_NONE,"TIME/DIV"},
    {SDLK_s,KMOD_NONE,"S STACK"}, {SDLK_m,KMOD_NONE,"M MODE"}, {SDLK_PLUS,KMOD_NONE,"FRAMES"}, {SDLK_r,KMOD_NONE,"R RESET"}, {SDLK_p,KMOD_SHIFT,"SHIFT+P"},
    {SDLK_p,KMOD_NONE,"P SCREENSHOT"}, {SDLK_F9,KMOD_NONE,"F9 IQ"}, {SDLK_F10,KMOD_NONE,"F10 DIAG"},
    {SDLK_n,KMOD_NONE,"N SOURCE"}, {SDLK_a,KMOD_NONE,"A MUTE"}, {SDLK_n,KMOD_SHIFT,"SHIFT+N L/R"},
    {SDLK_w,KMOD_NONE,"W SAVE"}, {SDLK_x,KMOD_NONE,"X SETUP"}, {SDLK_F7,KMOD_NONE,"F7 MY ID"},
    {SDLK_EXCLAIM,KMOD_NONE,"SHIFT+1 INFO"}
};
_Static_assert(sizeof(controls)/sizeof(*controls)==CONTROL_COUNT,"Every control needs a label and event");

static SDL_Rect control_group_rect(unsigned group)
{
    int y=70;
    for(unsigned i=0;i<group;++i) y+=24+50*((control_groups[i].count+2)/3)+18;
    return (SDL_Rect){UI_WIDTH-UI_CONTROL_WIDTH+10,y,UI_CONTROL_WIDTH-20,
        24+50*(int)((control_groups[group].count+2)/3)};
}
static SDL_Rect side_control_rect(int index)
{
    if(index<0 || index>=CONTROL_COUNT) return (SDL_Rect){0,0,0,0};
    unsigned offset=(unsigned)index,group=0;
    while(offset>=control_groups[group].count) offset-=control_groups[group++].count;
    SDL_Rect frame=control_group_rect(group);
    int width=(frame.w-20-12)/3;
    return (SDL_Rect){frame.x+10+(int)(offset%3)*(width+6),frame.y+16+(int)(offset/3)*50,width,44};
}
static void draw_control_groups(SDL_Renderer *r)
{
    SDL_Rect panel={UI_WIDTH-UI_CONTROL_WIDTH,65,UI_CONTROL_WIDTH,UI_HEIGHT-65};
    SDL_SetRenderDrawColor(r,19,27,39,255); SDL_RenderFillRect(r,&panel);
    for(unsigned i=0;i<sizeof(control_groups)/sizeof(*control_groups);++i) {
        SDL_Rect frame=control_group_rect(i);
        int label_x=frame.x+14, label_end=label_x+(int)strlen(control_groups[i].title)*8;
        SDL_SetRenderDrawColor(r,245,245,245,255);
        SDL_RenderDrawLine(r,frame.x,frame.y,label_x-6,frame.y);
        SDL_RenderDrawLine(r,label_end+6,frame.y,frame.x+frame.w-1,frame.y);
        SDL_RenderDrawLine(r,frame.x,frame.y,frame.x,frame.y+frame.h-1);
        SDL_RenderDrawLine(r,frame.x+frame.w-1,frame.y,frame.x+frame.w-1,frame.y+frame.h-1);
        SDL_RenderDrawLine(r,frame.x,frame.y+frame.h-1,frame.x+frame.w-1,frame.y+frame.h-1);
        text(r,label_x,frame.y-7,control_groups[i].title,2);
    }
}
static void side_control(SDL_Renderer *r,int index,const char *value,int state)
{
    SDL_Rect bar=side_control_rect(index);
    if(state==3) SDL_SetRenderDrawColor(r,65,72,82,255);
    else if(state==2) SDL_SetRenderDrawColor(r,190,35,40,255);
    else if(state==1) SDL_SetRenderDrawColor(r,22,130,72,255);
    else SDL_SetRenderDrawColor(r,24,83,170,255);
    SDL_RenderFillRect(r,&bar);
    SDL_SetRenderDrawColor(r,state==3?165:255,state==3?172:255,state==3?182:255,255);
    text(r,bar.x+6,bar.y+7,controls[index].label,2);
    text(r,bar.x+6,bar.y+25,value,2);
}

/* Events have already been translated into centre-panel coordinates. */
static int side_control_event(SDL_Event *e)
{
    if(e->type!=SDL_MOUSEBUTTONDOWN || e->button.button!=SDL_BUTTON_LEFT) return 0;
    SDL_Point point={e->button.x+UI_SIDE_WIDTH,e->button.y};
    for(int i=0;i<CONTROL_COUNT;++i) {
        SDL_Rect rect=side_control_rect(i);
        if(!SDL_PointInRect(&point,&rect)) continue;
        SDL_Keycode key=controls[i].key;
        if(i==CONTROL_TIMEBASE && point.x<rect.x+rect.w/2) key=SDLK_PAGEDOWN;
        if(i==CONTROL_IF && point.x<rect.x+rect.w/2) key=SDLK_LEFTBRACKET;
        if(i==CONTROL_STACK_FRAMES && point.x<rect.x+rect.w/2) key=SDLK_MINUS;
        SDL_zero(*e); e->type=SDL_KEYDOWN;
        e->key.keysym.sym=key; e->key.keysym.mod=controls[i].modifiers;
        return 1;
    }
    return 0;
}

static void draw_side_panels(SDL_Renderer *r,const View *v,uint32_t save_notice,int save_result,uint32_t screenshot_notice,uint32_t if_notice)
{
    SDL_SetRenderDrawColor(r,160,205,235,255);
    text(r,UI_WIDTH-150,8,"V" ATV_VERSION,2);
    text(r,UI_WIDTH-150,28,"COMMIT",2);
    text(r,UI_WIDTH-150,46,ATV_GIT_COMMIT,2);
    uint32_t now=SDL_GetTicks();
    int fresh_video=v->last_frame && now-v->last_frame<150;
    int fresh_audio=v->audio_metrics_tick && now-v->audio_metrics_tick<1000;
    SDL_Rect panel={0,65,UI_SIDE_WIDTH,740};
    SDL_SetRenderDrawColor(r,19,27,39,255); SDL_RenderFillRect(r,&panel);
    draw_control_groups(r);
    detection_bar(r,65,"SYNC",1,fresh_video && v->sync_locked);
    detection_bar(r,117,"COLOR BURST",color_enabled(v->c.rate,v->if_bandwidth),fresh_video && v->color_locked);
    int audio_enabled=sound_enabled(v->c.rate,v->if_bandwidth);
    detection_bar(r,169,"AUDIO 6 MHZ",audio_enabled,fresh_audio && v->mono_detect);
    detection_bar(r,221,"NICAM",audio_enabled,fresh_audio && v->nicam_detect);
    int stereo_enabled=stereo_sound_enabled(v->c.rate,v->if_bandwidth);
    const char *carriers[]={"7.02 MHZ","7.22 MHZ","7.38 MHZ","7.56 MHZ"};
    for(int k=0;k<4;++k)
        detection_bar(r,273+52*k,carriers[k],stereo_enabled,fresh_audio && (v->stereo_detect & (1<<k)));

    SDL_Rect frames={10,481,UI_SIDE_WIDTH-20,52};
    int stack_progress=(v->stacking || v->light_stacking) &&
        v->stack.target && v->stack.count;
    if(stack_progress && v->stack.count>=v->stack.target)
        SDL_SetRenderDrawColor(r,22,130,72,255);
    else if(stack_progress) SDL_SetRenderDrawColor(r,190,35,40,255);
    else SDL_SetRenderDrawColor(r,24,83,170,255);
    SDL_RenderFillRect(r,&frames);
    SDL_SetRenderDrawColor(r,255,255,255,255);
    text(r,frames.x+8,frames.y+6,"FRAMES",2);
    char frame_count[32];
    snprintf(frame_count,sizeof(frame_count),"%u/%u",v->stack.count,v->stack.target);
    text(r,frames.x+8,frames.y+22,frame_count,2);
    const char *stack_status=!v->stacking && !v->light_stacking?"OFF":v->stack_paused?"PAUSED":
        !v->stack.rolling && v->stack.count>=v->stack.target?"DONE":
        v->last_stacked && now-v->last_stacked<200?"ADDING":"HOLD";
    text(r,frames.x+8,frames.y+39,stack_status,1);

    SDL_Rect snr={10,541,UI_SIDE_WIDTH-20,44};
    SDL_SetRenderDrawColor(r,24,83,170,255); SDL_RenderFillRect(r,&snr);
    SDL_SetRenderDrawColor(r,255,255,255,255);
    text(r,snr.x+8,snr.y+7,"VIDEO SNR~",2);
    int quality_fresh=v->quality_tick && now-v->quality_tick<1000;
    int quality_valid=quality_fresh && v->quality.valid && isfinite(v->quality.video_snr_db);
    if(quality_valid) {
        char snr_value[32];
        snprintf(snr_value,sizeof(snr_value),"%.1f DB",v->quality.video_snr_db);
        text(r,snr.x+8,snr.y+25,snr_value,2);
    }

    SDL_Rect timing={10,593,UI_SIDE_WIDTH-20,76};
    SDL_SetRenderDrawColor(r,24,83,170,255); SDL_RenderFillRect(r,&timing);
    SDL_SetRenderDrawColor(r,255,255,255,255);
    text(r,timing.x+8,timing.y+7,"LINE",2);
    char frequency[32];
    snprintf(frequency,sizeof(frequency),"%.1f HZ",v->line_hz);
    text(r,timing.x+8,timing.y+25,frequency,2);
    text(r,timing.x+8,timing.y+41,"RASTER",2);
    if(fresh_video && v->sync_locked && v->raster_hz>0 && isfinite(v->raster_hz)) {
        snprintf(frequency,sizeof(frequency),"%.2f HZ",v->raster_hz);
    } else snprintf(frequency,sizeof(frequency),"NO LOCK");
    text(r,timing.x+8,timing.y+59,frequency,2);

    SDL_Rect if_level={10,677,UI_SIDE_WIDTH-20,44};
    SDL_SetRenderDrawColor(r,24,83,170,255); SDL_RenderFillRect(r,&if_level);
    SDL_SetRenderDrawColor(r,255,255,255,255);
    char status[32];
    const char *source=!strcmp(v->c.mode,"demo")?"DEMO":
        !strcmp(v->c.mode,"file")?(v->done?"FILE END":"FILE"):"LIVE";
    snprintf(status,sizeof(status),"%s IF",source);
    text(r,if_level.x+8,if_level.y+7,status,2);
    snprintf(status,sizeof(status),"%.1f DBFS",v->if_power_dbfs);
    text(r,if_level.x+8,if_level.y+25,status,2);

    SDL_Rect counters={10,729,UI_SIDE_WIDTH-20,76};
    SDL_SetRenderDrawColor(r,24,83,170,255); SDL_RenderFillRect(r,&counters);
    SDL_SetRenderDrawColor(r,255,255,255,255);
    text(r,counters.x+8,counters.y+7,"FRAMES",2);
    snprintf(status,sizeof(status),"%u",v->frames);
    text(r,counters.x+8,counters.y+25,status,2);
    text(r,counters.x+8,counters.y+41,"DROPS",2);
    snprintf(status,sizeof(status),"%u",v->overflows);
    text(r,counters.x+8,counters.y+59,status,2);

    char label[32];
    snprintf(label,sizeof(label),"- %.2fM +",v->if_bandwidth/1e6);
    side_control(r,CONTROL_IF,label,if_notice && now-if_notice<500?1:0);
    int can_afc=strcmp(v->c.mode,"file") && strcmp(v->c.mode,"demo");
    const char *afc=!can_afc?"N/A":!v->afc_enabled?"OFF":
        v->afc_state==1?"TRACK":v->afc_state==2?"LIMIT":"SEEK";
    if(can_afc && v->afc_enabled) {
        snprintf(label,sizeof(label),"%s %+.0fK",afc,(v->actual_hz-v->afc_anchor)/1e3);
        afc=label;
    }
    side_control(r,CONTROL_AFC,afc,!can_afc?3:v->afc_enabled);
    side_control(r,CONTROL_FM,v->fm_mode==2?"FM2":v->fm_mode?"FM1":"FM0",0);
    side_control(r,CONTROL_FREE_SYNC,v->free_sync?"ON":"OFF",v->free_sync);
    side_control(r,CONTROL_EXP_SYNC,v->c.experimental_sync?"ON":"OFF",v->c.experimental_sync);
    side_control(r,CONTROL_INVERSE,v->c.invert?"ON":"OFF",v->c.invert);
    side_control(r,CONTROL_DEEMPH,v->c.video_deemphasis?"ON":"OFF",v->c.video_deemphasis);
    side_control(r,CONTROL_SCOPE,v->scope_enabled?"SCOPE":"TV",v->scope_enabled);
    snprintf(label,sizeof(label),"- %u US +",video_scope_us_per_div[v->scope_timebase]);
    side_control(r,CONTROL_TIMEBASE,label,v->scope_enabled?0:3);
    int stacking=v->stacking || v->light_stacking;
    int light_controls=v->c.rate>10e6 && !v->stacking;
    side_control(r,CONTROL_STACK,v->light_stacking?"6 ON":stacking?"ON":"OFF",stacking);
    side_control(r,CONTROL_STACK_MODE,light_controls?"ROLLING 6":v->stack.rolling?"ROLLING":"FIXED",light_controls?3:v->stack.rolling);
    snprintf(label,sizeof(label),"- %u +",v->stack.target);
    side_control(r,CONTROL_STACK_FRAMES,light_controls?"- 6 +":label,light_controls?3:0);
    side_control(r,CONTROL_STACK_RESET,light_controls?"N/A":"CLEAR",light_controls?3:0);
    side_control(r,CONTROL_STACK_PAUSE,light_controls?"N/A":v->stack_paused?"RESUME":"PAUSE",light_controls?3:v->stack_paused?2:0);
    side_control(r,CONTROL_SCREENSHOT,"SAVE PNG",screenshot_notice && now-screenshot_notice<500?1:0);
    IQRecordState recording=v->recording.state;
    const char *record_label=recording==IQ_REC_PREPARING?"PREPARING":
        recording==IQ_REC_CAPTURING?"RECORDING":recording==IQ_REC_SAVING?"SAVING":
        recording==IQ_REC_SAVED?"SAVED":recording==IQ_REC_ERROR?"ERROR":"RECORD";
    side_control(r,CONTROL_IQ,record_label,recording==IQ_REC_ERROR?2:recording==IQ_REC_PREPARING || recording==IQ_REC_CAPTURING?1:0);
    side_control(r,CONTROL_DIAG,"30 SEC + PNG",0);
    const char *sources[]={"FM 6 MHZ","NICAM","7.02/7.22","7.38/7.56"};
    side_control(r,CONTROL_AUDIO,sources[v->audio_source & 3],audio_enabled?0:3);
    side_control(r,CONTROL_MUTE,v->audio_muted?"ON":"OFF",v->audio_muted?2:0);
    side_control(r,CONTROL_SWAP,v->audio_swap?"R / L":"L / R",stereo_enabled && v->audio_source>=2?v->audio_swap:3);
    int saved=save_notice && now-save_notice<1500;
    side_control(r,CONTROL_SAVE,saved?(save_result?"SAVED":"FAILED"):"SETTINGS",saved?(save_result?1:2):0);
    side_control(r,CONTROL_SETUP,"SDR SETTINGS",0);
    side_control(r,CONTROL_STATION,"CALL / LOC",0);
    side_control(r,CONTROL_INFO,"CONTROLS",0);
}
typedef struct {
    SDL_Renderer *renderer;
    int drivers_checked, drivers[4];
} WizardWindow;

/* Wrap longer paths and help text within the fixed-width layout. */
static void wizard_lines(SDL_Renderer *r,int x,int y,const char *str,int max_lines)
{
    for(int row=0;*str && row<max_lines;++row) {
        char line[108]; size_t n=0;
        while(str[n] && str[n]!='\n' && n<sizeof(line)-1) ++n;
        memcpy(line,str,n); line[n]=0; text(r,x,y+row*22,line,2);
        str+=n; if(*str=='\n') ++str;
    }
}
static void draw_wizard(SDL_Renderer *r,const SettingsPrompt *q,const char *input,const char *error,const int *drivers)
{
    SDL_SetRenderDrawColor(r,14,19,27,255); SDL_RenderClear(r);
    SDL_SetRenderDrawColor(r,90,210,180,255);
    text(r,40,32,"FM ATV - STARTUP CONFIGURATION",4);
    SDL_SetRenderDrawColor(r,140,155,170,255);
    text(r,40,70,"VERSION " ATV_VERSION "   COMMIT " ATV_GIT_COMMIT,2);
    SDL_SetRenderDrawColor(r,235,240,245,255);
    text(r,40,120,q->title,3);
    if(q->kind==SETTINGS_HARDWARE && drivers) {
        static const char *names[]={"1. B210", "2. HackRF", "3. PlutoSDR", "4. RSP1B"};
        static const char *backends[]={"UHD", "SoapyHackRF", "libiio", "SoapySDRPlay3"};
        for(int i=0;i<4;++i) {
            int y=190+i*46;
            SDL_SetRenderDrawColor(r,235,240,245,255); text(r,40,y,names[i],2);
            text(r,245,y,settings_hardware_rate_hint((unsigned)i),2);
            SDL_SetRenderDrawColor(r,drivers[i]?90:255,drivers[i]?210:100,drivers[i]?145:100,255);
            text(r,710,y,drivers[i]?"Installed":"Not installed",2);
            SDL_SetRenderDrawColor(r,140,155,170,255); text(r,40,y+20,backends[i],2);
        }
        SDL_SetRenderDrawColor(r,140,155,170,255);
        text(r,40,366,"Driver status for this application. Connected SDR hardware is not checked.",2);
        text(r,40,394,"Type 1, 2, 3 or 4 and press Enter.",2);
        text(r,40,422,"Continuous RX guidance; actual rate depends on PC and connection.",2);
        text(r,40,450,"FM 6 MHz / NICAM needs >=14 MS/s AND >=14 MHz video IF.",2);
    } else wizard_lines(r,40,178,q->details,q->kind==SETTINGS_MENU?18:14);
    SDL_SetRenderDrawColor(r,160,205,235,255);
    if(q->kind!=SETTINGS_MENU) {
        text(r,40,500,"CURRENT VALUE:",2);
        wizard_lines(r,40,525,*q->current?q->current:"-",2);
    }
    int input_y=q->kind==SETTINGS_MENU?600:580;
    SDL_Rect box={40,input_y,880,44};
    SDL_SetRenderDrawColor(r,35,49,65,255); SDL_RenderFillRect(r,&box);
    SDL_SetRenderDrawColor(r,235,240,245,255);
    char visible[108]; size_t len=strlen(input);
    snprintf(visible,sizeof(visible),"%s_",input+(len>104?len-104:0));
    text(r,50,input_y+15,visible,2);
    SDL_SetRenderDrawColor(r,255,170,110,255);
    wizard_lines(r,40,654,*error?error:q->error,2);
    SDL_Rect next={40,730,320,46}, cancel={380,730,320,46};
    SDL_SetRenderDrawColor(r,45,65,80,255); SDL_RenderFillRect(r,&next); SDL_RenderFillRect(r,&cancel);
    SDL_SetRenderDrawColor(r,235,240,245,255);
    text(r,55,745,(q->kind==SETTINGS_WELCOME)?"SET UP":q->kind==SETTINGS_MENU?"START - ENTER":"CONTINUE - ENTER",2);
    text(r,395,745,(q->kind==SETTINGS_WELCOME)?"SKIP - ENTER":"EXIT - ESC",2);
    if(q->kind==SETTINGS_MENU) {
        SDL_Rect save={720,730,200,46};
        SDL_SetRenderDrawColor(r,45,65,80,255); SDL_RenderFillRect(r,&save);
        SDL_SetRenderDrawColor(r,235,240,245,255); text(r,735,745,"SAVE - W",2);
    }
    SDL_SetRenderDrawColor(r,140,155,170,255);
    text(r,40,805,"BACKSPACE: DELETE CHARACTER   CTRL+A: CLEAR FIELD   CTRL+V: PASTE",2);
    text(r,40,837,"W: SAVE SETTINGS AND THIS SDR PROFILE   ALT+ENTER: FULLSCREEN",2);
}
static int fullscreen_shortcut(const SDL_Event *e)
{
    if(e->type!=SDL_KEYDOWN || !(e->key.keysym.mod & KMOD_ALT) ||
       (e->key.keysym.sym!=SDLK_RETURN && e->key.keysym.sym!=SDLK_KP_ENTER)) return 0;
    SDL_Window *window=SDL_GetWindowFromID(e->key.windowID);
    if(window && !e->key.repeat) {
        Uint32 mode=(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN)?0:SDL_WINDOW_FULLSCREEN_DESKTOP;
        if(SDL_SetWindowFullscreen(window,mode)) fprintf(stderr,"Fullscreen: %s\n",SDL_GetError());
    }
    return 1;
}
static int graphical_prompt(void *opaque,const SettingsPrompt *q,char *line,size_t n)
{
    WizardWindow *w=opaque; line[0]=0;
    if(q->kind==SETTINGS_HARDWARE && !w->drivers_checked) {
        driver_status(w->drivers); w->drivers_checked=1;
    }
    const char *error="";
    SDL_StartTextInput();
    for(;;) {
        SDL_Event e;
        while(SDL_PollEvent(&e)) {
            if(fullscreen_shortcut(&e)) continue;
            if(e.type==SDL_QUIT) { SDL_StopTextInput(); return 0; }
            if(e.type==SDL_KEYDOWN) {
                SDL_Keycode key=e.key.keysym.sym;
                if(key==SDLK_RETURN || key==SDLK_KP_ENTER) { SDL_StopTextInput(); return 1; }
                if(key==SDLK_ESCAPE) { line[0]=0; SDL_StopTextInput(); return (q->kind==SETTINGS_WELCOME)?1:0; }
                if(key==SDLK_BACKSPACE) {
                    size_t len=strlen(line); if(len) line[len-1]=0; error="";
                }
                if(key==SDLK_a && (e.key.keysym.mod & KMOD_CTRL)) { line[0]=0; error=""; }
                if(key==SDLK_v && (e.key.keysym.mod & KMOD_CTRL)) {
                    char *clip=SDL_GetClipboardText();
                    if(clip) {
                        if(strlen(line)+strlen(clip)<n && !strchr(clip,'\n') && !strchr(clip,'\r')) strcat(line,clip);
                        else error="INPUT TOO LONG OR CONTAINS MULTIPLE LINES.";
                        SDL_free(clip);
                    }
                }
            }
            if(e.type==SDL_TEXTINPUT) {
                if(q->kind==SETTINGS_MENU && (!strcmp(e.text.text,"w") || !strcmp(e.text.text,"W"))) {
                    snprintf(line,n,"w"); SDL_StopTextInput(); return 1;
                }
                if(strlen(line)+strlen(e.text.text)<n) { strcat(line,e.text.text); error=""; }
                else error="INPUT TOO LONG. USE BACKSPACE OR CTRL+A.";
            }
            if(e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT && e.button.x>=40 && e.button.x<920) {
                if(q->kind==SETTINGS_HARDWARE && e.button.y>=190 && e.button.y<374) {
                    snprintf(line,n,"%d",1+(e.button.y-190)/46);
                    SDL_StopTextInput(); return 1;
                }
                if(q->kind==SETTINGS_MENU && e.button.y>=178 && e.button.y<574) {
                    int row=(e.button.y-178)/22; const char *entry=q->details;
                    while(row-- && *entry) { const char *next=strchr(entry,'\n'); entry=next?next+1:entry+strlen(entry); }
                    int choice;
                    if(sscanf(entry,"%d.",&choice)==1) {
                        snprintf(line,n,"%d",choice); SDL_StopTextInput(); return 1;
                    }
                }
            }
            if(e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT && e.button.y>=730 && e.button.y<776) {
                if(e.button.x>=40 && e.button.x<360) {
                    if((q->kind==SETTINGS_WELCOME)) snprintf(line,n,"w");
                    SDL_StopTextInput(); return 1;
                }
                if(q->kind==SETTINGS_MENU && e.button.x>=720 && e.button.x<920) {
                    snprintf(line,n,"w"); SDL_StopTextInput(); return 1;
                }
                if(e.button.x>=380 && e.button.x<700) {
                    line[0]=0; SDL_StopTextInput(); return (q->kind==SETTINGS_WELCOME)?1:0;
                }
            }
        }
        draw_wizard(w->renderer,q,line,error,w->drivers_checked?w->drivers:NULL);
        SDL_RenderPresent(w->renderer); SDL_Delay(16);
    }
}
/* Keep the video backend alive between the wizard and receiver. Reopening
 * it can initialize GTK twice through SDL's desktop integration. */
static int init_ui(void)
{
    Uint32 flags=SDL_INIT_VIDEO | SDL_INIT_TIMER;
    return SDL_InitSubSystem(flags & ~SDL_WasInit(flags));
}

static SDL_Window *create_ui_window(const char *title,int width,int height)
{
    SDL_Rect area;
    int x=SDL_WINDOWPOS_CENTERED,y=SDL_WINDOWPOS_CENTERED;
    if(SDL_GetDisplayUsableBounds(0,&area)==0 || SDL_GetDisplayBounds(0,&area)==0) {
        /* Usable bounds exclude desktop panels. Leave room for window decorations. */
        int available_w=SDL_max(1,area.w-32),available_h=SDL_max(1,area.h-64);
        double scale=fmin(1.0,fmin((double)available_w/width,(double)available_h/height));
        width=SDL_max(1,(int)(width*scale));
        height=SDL_max(1,(int)(height*scale));
        x=area.x+(area.w-width)/2;
        y=area.y+(area.h-height)/2;
    }
    return SDL_CreateWindow(title,x,y,width,height,SDL_WINDOW_RESIZABLE);
}

static int graphical_wizard(Settings *settings)
{
    if(init_ui()) { fprintf(stderr,"SDL: %s\n",SDL_GetError()); return -1; }
    SDL_Window *window=create_ui_window("FM ATV - Startup Configuration",960,880);
    SDL_Renderer *renderer=window?SDL_CreateRenderer(window,-1,0):NULL;
    int result=-1;
    if(renderer && ui_text_init(renderer)) {
        SDL_RenderSetLogicalSize(renderer,960,880);
        WizardWindow wizard={.renderer=renderer};
#ifdef _WIN32
        int count=0;
        char **devices=NULL;
        if((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) || !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            int found=SDL_GetNumAudioDevices(0);
            if(found>0) devices=calloc((size_t)found,sizeof(*devices));
            if(devices) for(int i=0;i<found;++i) {
                const char *name=SDL_GetAudioDeviceName(i,0);
                if(name && (devices[count]=SDL_strdup(name))) ++count;
            }
        } else fprintf(stderr,"Cannot enumerate audio outputs: %s\n",SDL_GetError());
        result=settings_run_wizard_audio(settings,graphical_prompt,&wizard,(const char *const *)devices,count);
        for(int i=0;i<count;++i) SDL_free(devices[i]);
        free(devices);
#else
        result=settings_run_wizard(settings,graphical_prompt,&wizard);
#endif
    } else fprintf(stderr,"Wizard: %s\n",SDL_GetError());
    SDL_StopTextInput();
    if(renderer) { ui_text_destroy(renderer); SDL_DestroyRenderer(renderer); }
    if(window) SDL_DestroyWindow(window);
    return result;
}

typedef struct {
    int active;
    SDL_Keycode dismissed_with;
} InfoScreen;

static int info_screen_event(InfoScreen *info, const SDL_Event *e)
{
    if(e->type==SDL_QUIT) return 0;
    /* A held dismissal key must not leak repeats into tuning or other controls. */
    if(info->dismissed_with && (e->type==SDL_KEYDOWN || e->type==SDL_KEYUP) &&
       e->key.keysym.sym==info->dismissed_with) {
        if(e->type==SDL_KEYUP) info->dismissed_with=0;
        return 1;
    }
    if(info->active) {
        if(e->type==SDL_MOUSEBUTTONDOWN && e->button.button==SDL_BUTTON_LEFT)
            info->active=0;
        if(e->type==SDL_KEYDOWN && !e->key.repeat) {
            info->active=0; info->dismissed_with=e->key.keysym.sym;
        }
        return 1;
    }
    if(e->type==SDL_KEYDOWN && !e->key.repeat &&
       (e->key.keysym.sym==SDLK_EXCLAIM ||
        (e->key.keysym.sym==SDLK_1 && (e->key.keysym.mod & KMOD_SHIFT)))) {
        info->active=1; return 1;
    }
    return 0;
}

static SDL_AudioDeviceID open_audio_output(const char *name)
{
    if(!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr,"Audio unavailable (video continues): %s\n",SDL_GetError());
        return 0;
    }
    SDL_AudioSpec wanted={0};
    wanted.freq=48000; wanted.format=AUDIO_F32SYS; wanted.channels=2; wanted.samples=1024;
    if(name && !*name) name=NULL;
    SDL_AudioDeviceID device=SDL_OpenAudioDevice(name,0,&wanted,NULL,0);
    if(!device && name) {
        fprintf(stderr,"Audio output '%s' unavailable: %s; trying system default\n",name,SDL_GetError());
        name=NULL;
        device=SDL_OpenAudioDevice(NULL,0,&wanted,NULL,0);
    }
    if(device) fprintf(stderr,"Audio output: %s (%s), 48000 Hz stereo\n",
        name?name:"system default",SDL_GetCurrentAudioDriver());
    else fprintf(stderr,"Audio unavailable (video continues): %s\n",SDL_GetError());
    return device;
}
static void draw_video_scope(SDL_Renderer *r,const View *v)
{
    const VideoScopeReading *s=&v->scope;
    const int left=190,top=165,width=620,height=340;
    char label[160];
    SDL_SetRenderDrawColor(r,220,230,240,255);
    unsigned us_div=video_scope_us_per_div[v->scope_timebase];
    double span_us=s->sequence?s->duration_us:8.0*us_div;
    int milliseconds=span_us>=999;
    snprintf(label,sizeof(label),"VIDEO OSCILLOSCOPE | %u US/DIV",us_div);
    text(r,140,85,label,2);
    text(r,140,110,v->c.video_deemphasis?
        "SIGNAL: VIDEO IF -> FM -> PAL DE-EMPHASIS":
        "SIGNAL: VIDEO IF -> FM (DE-EMPHASIS OFF)",2);
    text(r,140,137,"AMPLITUDE: FM UNITS / AUTO SCALE",1);
    double range=fmax(.1,(double)s->maximum-s->minimum);
    double minimum=s->minimum-range*.08,maximum=s->maximum+range*.08;
    if(!s->sequence) { minimum=-1; maximum=1; }
    double scale=height/(maximum-minimum);
    for(int j=0;j<=4;++j) {
        int y=top+j*height/4;
        SDL_SetRenderDrawColor(r,45,55,66,255); SDL_RenderDrawLine(r,left,y,left+width,y);
        SDL_SetRenderDrawColor(r,140,155,170,255);
        snprintf(label,sizeof(label),"%+.2f",maximum-(maximum-minimum)*j/4);
        text(r,125,y-5,label,1);
    }
    for(int j=0;j<=8;++j) {
        int x=left+j*width/8;
        SDL_SetRenderDrawColor(r,45,55,66,255); SDL_RenderDrawLine(r,x,top,x,top+height);
        SDL_SetRenderDrawColor(r,140,155,170,255);
        snprintf(label,sizeof(label),milliseconds?"%.2f":"%.1f",(s->start_us+span_us*j/8)/(milliseconds?1000:1));
        text(r,x-12,top+height+12,label,1);
    }
    text(r,760,top+height+30,milliseconds?"TIME (MS)":"TIME (US)",1);
    if(s->sequence) {
        if(s->triggered) {
            int x=left+(int)(-s->start_us/s->duration_us*width);
            SDL_SetRenderDrawColor(r,220,165,65,255); SDL_RenderDrawLine(r,x,top,x,top+height);
        }
        int stale=SDL_GetTicks()-v->scope_tick>500;
        SDL_SetRenderDrawColor(r,stale?110:75,stale?125:220,stale?130:170,255);
        int previous_x=0,previous_y=0;
        for(int j=0;j<VIDEO_SCOPE_POINTS;++j) {
            int x=left+j*width/(VIDEO_SCOPE_POINTS-1);
            int upper=top+(int)((maximum-s->high[j])*scale);
            int lower=top+(int)((maximum-s->low[j])*scale);
            int middle=(upper+lower)/2;
            SDL_RenderDrawLine(r,x,upper,x,lower);
            if(j) SDL_RenderDrawLine(r,previous_x,previous_y,x,middle);
            previous_x=x; previous_y=middle;
        }
        SDL_SetRenderDrawColor(r,220,230,240,255);
        snprintf(label,sizeof(label),"%s%s",s->triggered?(us_div>=128?"BROAD PULSE TRIGGER":"LINE PULSE TRIGGER"):"FREE RUN - NO TRIGGER",stale?" | STALE":"");
        text(r,140,550,label,2);
    } else {
        SDL_SetRenderDrawColor(r,220,230,240,255);
        text(r,240,315,"WAITING FOR VIDEO SAMPLES",2);
    }
    SDL_SetRenderDrawColor(r,140,155,170,255);
    text(r,140,580,"PGDN / PGUP TIMEBASE | P SAVE SCOPE | O TV",1);
}

static void draw_iq_recording(SDL_Renderer *r,const IQRecordStatus *s,const char *warning)
{
    if(!warning && (s->state==IQ_REC_IDLE ||
       ((s->state==IQ_REC_SAVED || s->state==IQ_REC_ERROR) && SDL_GetTicks()-s->updated>8000))) return;
    SDL_Rect panel={120,571,720,34};
    SDL_SetRenderDrawColor(r,24,34,46,255); SDL_RenderFillRect(r,&panel);
    SDL_SetRenderDrawColor(r,235,180,100,255);
    char label[160];
    if(warning || s->state==IQ_REC_ERROR) snprintf(label,sizeof(label),"IQ RECORDING UNAVAILABLE");
    else if(s->state==IQ_REC_PREPARING) snprintf(label,sizeof(label),"IQ PREPARING %.0f MB | F9 CANCEL",s->capacity*sizeof(IQ)/1e6);
    else if(s->state==IQ_REC_CAPTURING) snprintf(label,sizeof(label),"IQ REC %.2f / %.2f S | F9 STOP",s->count/s->rate,s->capacity/s->rate);
    else if(s->state==IQ_REC_SAVING) snprintf(label,sizeof(label),"IQ SAVING %.0f MB",s->count*sizeof(IQ)/1e6);
    else snprintf(label,sizeof(label),"IQ SAVED %.2f S | %s",s->count/s->rate,s->reason);
    text(r,130,576,label,2);
    SDL_SetRenderDrawColor(r,220,230,240,255);
    text(r,130,595,warning?warning:s->state==IQ_REC_ERROR?s->error:s->path,1);
}

static void draw_diagnosis(SDL_Renderer *r,DiagnosticLog *log)
{
    int state=diagnostic_log_state(log);
    if(state==DIAG_IDLE) return;
    char button_value[32];
    if(state==DIAG_RECORDING) snprintf(button_value,sizeof(button_value),"%u SEC LEFT",diagnostic_log_remaining(log));
    else snprintf(button_value,sizeof(button_value),"%s",state==DIAG_SAVING?"SAVING":state==DIAG_SAVED?"SAVED":"ERROR");
    side_control(r,CONTROL_DIAG,button_value,state==DIAG_ERROR?2:1);
    char label[512];
    if(state==DIAG_RECORDING) snprintf(label,sizeof(label),"F10 DIAGNOSIS: %u SEC LEFT - F10 STOPS",diagnostic_log_remaining(log));
    else if(state==DIAG_SAVING) snprintf(label,sizeof(label),"DIAGNOSIS: SAVING TEXT FILE...");
    else if(state==DIAG_SAVED) {
        const char *path=diagnostic_log_path(log), *name=strrchr(path,'/');
        snprintf(label,sizeof(label),"SAVED %s + INITIAL PNG | SHIFT+F10: COPY TXT PATH",name?name+1:path);
    } else snprintf(label,sizeof(label),"DIAGNOSIS: %s",diagnostic_log_error(log));
    SDL_Rect bar={UI_SIDE_WIDTH+10,810,940,20};
    SDL_SetRenderDrawColor(r,state==DIAG_ERROR?130:20,35,50,255); SDL_RenderFillRect(r,&bar);
    SDL_SetRenderDrawColor(r,235,245,255,255); text(r,bar.x+5,bar.y+5,label,1);
}

static void draw_info_screen(SDL_Renderer *r, const Config *c)
{
    static const struct { const char *key, *description; } commands[]={
        {"LEFT / RIGHT", "TUNE IN 100 KHZ STEPS"},
        {"SHIFT / CTRL", "WHEN TUNING: 1 MHZ / 10 KHZ PER STEP"},
        {"UP / DOWN", "INCREASE / DECREASE RECEIVE GAIN BY 1 DB"},
        {"T", "ENTER FREQUENCY IN MHZ, INCLUDING CONVERTER LO"},
        {"O", "TV / SCOPE; PGDN / PGUP ADJUST TIMEBASE"},
        {"F9 / F10", "IQ RECORDING / DIAGNOSIS TXT (30 SEC); PRESS AGAIN TO STOP"},
        {"SHIFT+F10", "COPY SAVED DIAGNOSIS FILE PATH"},
        {"F7", "EDIT MY CALLSIGN AND LOCATOR"},
        {"ENTER / ESC", "DURING FREQUENCY ENTRY: TUNE / CANCEL"},
        {"BACKSPACE / CTRL+A", "DURING FREQUENCY ENTRY: DELETE CHARACTER / CLEAR FIELD"},
        {"W / X", "SAVE SETTINGS AND SDR PROFILE / RETURN TO SETUP"},
        {"C", "AFC ON / OFF: TRACK SIGNAL WITHIN +/-500 KHZ (LIVE SDR)"},
        {"I", "INVERT FM VIDEO POLARITY"},
        {"D / E", "FM0 / FM1 / FM2; PAL DE-EMPHASIS ON / OFF"},
        {"F / G", "FREE SYNC / EXPERIMENTAL ECHO-RESISTANT SYNC ON / OFF"},
        {"[ / ]", "DECREASE / INCREASE VIDEO IF BANDWIDTH BY 1 MHZ"},
        {", / .", "DECREASE / INCREASE LINE FREQUENCY BY 0.1 HZ"},
        {"B / V", "SHIFT VERTICAL IMAGE POSITION BY ONE STEP"},
        {"S", "STACK ON / OFF; ABOVE 10 MS/S: ROLLING 6, FULL PICTURE"},
        {"- / +", "DECREASE / INCREASE NUMBER OF FRAMES TO STACK"},
        {"R", "RESTART STACK"},
        {"M", "SWITCH FIXED / ROLLING STACKING"},
        {"SHIFT+P", "PAUSE / RESUME STACKING"},
        {"P / CTRL+P", "PNG: 120 MS AVERAGE / SINGLE IMAGE"},
        {"A", "MUTE / UNMUTE AUDIO"},
        {"N", "AUDIO SOURCE: FM 6 / NICAM / 7.02+7.22 / 7.38+7.56 MHZ"},
        {"SHIFT+N", "SWAP LEFT AND RIGHT AUDIO CHANNELS"},
        {"SHIFT+1 / !", "OPEN THIS INFO SCREEN"},
        {"Q / ESC", "QUIT APPLICATION FROM THE MAIN SCREEN"},
        {"MOUSE", "WHEEL: TUNE. CLICK SPECTRUM: SELECT FREQUENCY"}
    };
    SDL_SetRenderDrawColor(r,14,19,27,255); SDL_RenderClear(r);
    SDL_SetRenderDrawColor(r,90,210,180,255);
    text(r,40,28,"FM ATV - INFO AND CONTROLS",4);
    SDL_SetRenderDrawColor(r,225,235,245,255);
    text(r,40,60,"VERSION " ATV_VERSION "   COMMIT " ATV_GIT_COMMIT,2);
    text(r,40,78,"PURPOSE: RECEIVE ANALOG FM AMATEUR TELEVISION WITH AN SDR.",2);
    SDL_SetRenderDrawColor(r,160,205,235,255);
    text(r,40,104,"STACK: CONTEST AT <=10 MS/S; ABOVE 10 MS/S: LIGHT ROLLING 6, RGB.",2);
    text(r,40,126,"PAL COLOR: SAMPLE RATE >= 12 MS/S AND VIDEO IF >= 10 MHZ.",2);
    text(r,40,148,"NICAM DIGITAL AUDIO: SAMPLE RATE >= 14 MS/S AND VIDEO IF >= 14 MHZ.",2);
    SDL_SetRenderDrawColor(r,225,235,245,255);
    text(r,40,180,"AUTHOR: ROB HARDENBERG, PE1ITR",3);
    text(r,40,206,"ROB@PE1ITR.COM",2);
    SDL_SetRenderDrawColor(r,90,210,180,255);
    text(r,40,238,"KEY",2); text(r,250,238,"DESCRIPTION",2);
    for(size_t i=0;i<sizeof(commands)/sizeof(*commands);++i) {
        int y=264+(int)i*18;
        SDL_SetRenderDrawColor(r,160,205,235,255); text(r,40,y,commands[i].key,2);
        SDL_SetRenderDrawColor(r,225,235,245,255); text(r,250,y,i==2 && !strcmp(c->mode,"hackrf")?"RX LNA 0..40 DB, STEP 8 DB (VGA FIXED)":commands[i].description,2);
    }
    SDL_SetRenderDrawColor(r,140,155,170,255);
    if(!strcmp(c->mode,"hackrf")) {
        char gain_info[128];
        snprintf(gain_info,sizeof(gain_info),"HACKRF: RX VGA %.0f DB FIXED; RF AMP %s. UP/DOWN: RX LNA.",c->hackrf_vga,c->hackrf_amp?"ON (14 DB)":"OFF");
        text(r,40,784,gain_info,2);
    }
    text(r,40,804,"FILE / DEMO: FRAME STACKING IS ALSO AVAILABLE ABOVE 10 MS/S.",2);
    SDL_SetRenderDrawColor(r,90,210,180,255);
    text(r,40,842,"PRESS ANY KEY OR CLICK TO RETURN",3);
}

/* Short exposure of the displayed live images, without registration or a sync
 * requirement. Only distinct received fields count, not repeated UI redraws.
 * This runs in the UI thread and never holds the reception mutex. */
typedef struct {
    uint32_t sum[WIDTH*HEIGHT][3], pixels[WIDTH*HEIGHT];
    unsigned count, last_field;
    uint32_t started;
    double frequency, bandwidth;
    int stacking, fm_mode;
} ScreenshotExposure;

static int screenshot_expose(ScreenshotExposure *s,const View *v,uint32_t now)
{
    if(s->count && (s->frequency!=v->actual_hz || s->bandwidth!=v->if_bandwidth ||
       s->stacking!=v->stacking || s->fm_mode!=v->fm_mode))
        memset(s,0,sizeof(*s));
    if(!s->count) {
        s->started=now; s->frequency=v->actual_hz; s->bandwidth=v->if_bandwidth;
        s->stacking=v->stacking; s->fm_mode=v->fm_mode;
    }
    if(!s->count || s->last_field!=v->frames) {
        const uint32_t *pixels=v->stacking?v->live:v->pixels;
        for(size_t k=0;k<WIDTH*HEIGHT;++k) {
            s->sum[k][0]+=(pixels[k]>>16)&255;
            s->sum[k][1]+=(pixels[k]>>8)&255;
            s->sum[k][2]+=pixels[k]&255;
        }
        s->last_field=v->frames; ++s->count;
    }
    if(now-s->started<120 && s->count<64) return 0;
    for(size_t k=0;k<WIDTH*HEIGHT;++k) {
        unsigned red=(s->sum[k][0]+s->count/2)/s->count;
        unsigned green=(s->sum[k][1]+s->count/2)/s->count;
        unsigned blue=(s->sum[k][2]+s->count/2)/s->count;
        s->pixels[k]=0xff000000u|(red<<16)|(green<<8)|blue;
    }
    return 1;
}

typedef struct {
    char path[4352],error[512],capture_issue[256];
} ScreenshotResult;

/* Read the completed backbuffer before presentation. F10 can still save the
 * receiver's live video when the graphics backend cannot read its window. */
static SDL_Surface *capture_window(SDL_Renderer *renderer,char *issue,size_t capacity)
{
    SDL_RendererInfo info={0};
    SDL_GetRendererInfo(renderer,&info);
    const char *backend=info.name?info.name:"unknown renderer";
    int w,h;
    if(SDL_GetRendererOutputSize(renderer,&w,&h)) {
        snprintf(issue,capacity,"output size (%s): %s",backend,SDL_GetError());
        return NULL;
    }
    SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormat(0,w,h,32,SDL_PIXELFORMAT_RGBA32);
    if(!surface || SDL_RenderReadPixels(renderer,NULL,SDL_PIXELFORMAT_RGBA32,surface->pixels,surface->pitch)) {
        snprintf(issue,capacity,"capture %dx%d (%s): %s",w,h,backend,SDL_GetError());
        SDL_FreeSurface(surface);
        return NULL;
    }
    return surface;
}

static int save_window_png(SDL_Renderer *renderer,double frequency,const char *directory,
                           const uint32_t *live,ScreenshotResult *result)
{
    struct timespec now;
    struct tm utc;
    char stamp[32];
    char *path=result->path, *error=result->error;
    size_t capacity=sizeof(result->path),error_size=sizeof(result->error);
    SDL_Surface *surface=NULL;
    if(clock_gettime(CLOCK_REALTIME,&now) || !gmtime_r(&now.tv_sec,&utc) ||
       !strftime(stamp,sizeof(stamp),"%Y%m%dT%H%M%S",&utc)) {
        snprintf(error,error_size,"UTC filename: %s",strerror(errno));
        goto fail;
    }
    surface=capture_window(renderer,result->capture_issue,sizeof(result->capture_issue));
    if(!surface) {
        if(!live) goto fail;
        surface=SDL_CreateRGBSurfaceWithFormat(0,WIDTH,HEIGHT,32,SDL_PIXELFORMAT_RGBA32);
        if(!surface || SDL_ConvertPixels(WIDTH,HEIGHT,SDL_PIXELFORMAT_ARGB8888,live,WIDTH*4,
                                        SDL_PIXELFORMAT_RGBA32,surface->pixels,surface->pitch)) {
            snprintf(error,error_size,"live video fallback: %s",SDL_GetError());
            goto fail;
        }
    }
    int fd=-1;
    for(unsigned suffix=0;suffix<1000;++suffix) {
        int length=snprintf(path,capacity,"%sfm-atv-%.0fHz-%s-%03ldZ-%03u.png",
            directory,frequency,stamp,now.tv_nsec/1000000,suffix);
        if(length<0 || (size_t)length>=capacity) {
            snprintf(error,error_size,"screenshot path too long");
            goto fail;
        }
#ifdef _WIN32
        fd=_open(path,_O_WRONLY|_O_CREAT|_O_EXCL|_O_BINARY,_S_IREAD|_S_IWRITE);
#else
        fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);
#endif
        if(fd>=0 || errno!=EEXIST) break;
    }
    if(fd<0) {
        snprintf(error,error_size,"create file: %s",strerror(errno));
        goto fail;
    }
    FILE *file=fdopen(fd,"wb");
    if(!file) {
        snprintf(error,error_size,"open binary stream: %s",strerror(errno));
        close(fd); remove(path); goto fail;
    }
    png_image png={0}; png.version=PNG_IMAGE_VERSION;
    png.width=surface->w; png.height=surface->h; png.format=PNG_FORMAT_RGBA;
    if(!png_image_write_to_stdio(&png,file,0,surface->pixels,surface->pitch,NULL))
        snprintf(error,error_size,"encode PNG: %s",png.message);
    png_image_free(&png);
    if(fclose(file) && !*error) snprintf(error,error_size,"close file: %s",strerror(errno));
    if(*error) { remove(path); goto fail; }
    SDL_FreeSurface(surface);
    fprintf(stderr,"Screenshot saved: %s\n",path);
    return 0;
fail:
    SDL_FreeSurface(surface);
    fprintf(stderr,"Screenshot failed (%s): %s %s\n",*path?path:directory,result->capture_issue,error);
    return -1;
}

static void help(void)
{
    puts("FM-ATV PAL receiver (C)\n"
         "  --demo                 synthetic FM IQ (default; no hardware)\n"
         "  --b210                 OpenSourceSDRLab Kintex-7 via UHD C API\n"
         "  --sdrplay              SDRplay RSP1B via SoapySDRPlay3 (8 <= rate < 10 MS/s)\n"
         "  --hackrf               HackRF via SoapySDR\n"
         "  --pluto                Pluto / AD9361 via libiio (RX1)\n"
         "  --pluto-uri URI        USB auto-select by default; usb:... or ip:192.168.2.1\n"
         "  --file PATH            native float32 I,Q recording\n"
         "  --loop                 repeat IQ file until stopped\n"
         "  --args ARGS            UHD args / Soapy device selection\n"
         "  --uhd-images-dir DIR   clone firmware + FPGA directory\n"
         "  --antenna NAME         UHD antenna (default TX/RX = TRX1)\n"
         "  --channel N            RX channel (default 0)\n"
         "  --ref SOURCE           UHD reference (default internal)\n"
         "  --frequency HZ         received frequency before converter; default 1252000000\n"
         "  --lo HZ                converter LO; SDR tunes to frequency minus LO (default 0)\n"
         "  --version              show application version and UTC build timestamp\n"
         "  --config PATH          startup file (default ~/.atv-rx.conf)\n"
         "  --no-config            skip loading saved settings\n"
         "  --no-wizard            skip startup wizard\n"
         "  --wizard               show startup wizard in the application window\n"
         "  --terminal-wizard      use the optional terminal wizard\n"
         "  W                     save current settings to startup file\n"
         "  --rate SPS             default 20000000; minimum 5000000 for Pluto, 8000000 otherwise\n"
         "  --rx-block N           samples per receive call, 2048..1048576 (B210: 16384; Pluto: 131072; others: 32768)\n"
         "  --bandwidth HZ         default 18000000\n"
         "  --gain DB              default 30; HackRF RX LNA 0..40 in 8 dB steps\n"
         "  --hackrf-amp 0|1       RF preamp off/on (default on)\n"
         "  --hackrf-vga DB        fixed RX VGA, default 12 (0..62, step 2)\n"
         "  --iq-dc-block MODE     auto (HackRF only, default), on or off; IQ DC rejection before FM\n"
         "  --deviation HZ         Test/file video scale in Hz (default 4000000)\n"
         "  --video-deemphasis MODE PAL F.405-1 linear-phase correction: pal or off (default pal)\n"
         "  --if-bandwidth HZ      digital IF width (default 18000000); auto PAL color >= 10 MHz at >= 12 MS/s\n"
         "  --alignment-bandwidth HZ separate IF for sync/alignment (default 3200000; 0 = video IF)\n"
         "  --stack-frames N       aligned stack 1..512 fields (live >10 MS/s: light rolling 6)\n"
         "  --stack-rolling        continuously replace oldest accepted field\n"
         "  --free-sync            free-running live view; stack still follows sync\n"
         "  --experimental-sync    experimental echo-resistant line sync (default off)\n"
         "  --line-hz HZ           free-running line clock (default 15625)\n"
         "  --status-server        enable read-only HTTP JSON debug status (default off)\n"
         "  --no-status-server     disable status API, including a saved setting\n"
         "  --status-bind IPV4     local bind address (default 127.0.0.1; LAN: 0.0.0.0)\n"
         "  --status-port PORT     TCP port (default 8765); GET /status\n"
         "  --fm-mode 0|1|2        FM detector (2: FM1 with multi-candidate PAL sync); D cycles\n"
         "  --invert               reverse FM video polarity\n"
         "  --snapshot PATH        save UI screenshot as BMP on exit\n"
         "  --record PATH          capture raw IQ in RAM, save native cf32 on exit (no overwrite)\n"
         "  --record-seconds N     CLI / F9 capture duration, default 3; maximum 15 s / 2 GiB\n"
         "  --seconds N            stop after N seconds (0 = unlimited)\n"
         "  --nicam                start with NICAM 6.552 MHz audio selected (N cycles audio sources)\n"
    );
    puts("Audio: A mute; N select FM 6 MHz / NICAM / FM 7.02+7.22 / FM 7.38+7.56 MHz; Shift+N swap L/R.\n"
         "Shift+1 (!) = application information and all controls; any key returns.\n"
         "Controls: left/right or wheel = 100 kHz, Shift = 1 MHz, Ctrl = 10 kHz;\n"
         "C = AFC on/off (+/-500 kHz, live SDR); T = type frequency in MHz, Enter = tune, Esc = cancel; up/down = gain 1 dB; I = invert; click spectrum = tune; Q / Esc = quit.\n"
         "[ / ] = IF narrower/wider by 1 MHz; O = TV / scope; PgDn/PgUp = shorter/longer time per division; F = free sync; , / . = line clock -/+ 0.1 Hz; B/V = vertical position.\n"
         "E = PAL video de-emphasis on/off (default on); G = experimental sync on/off (default off).\n"
         "X = return to SDR setup (restarts reception after configuration).\n"
         "F7 = edit callsign/locator; F10 = start/stop 30-second diagnosis TXT; Shift+F10 = copy saved path.\n"
         "F9 = start/stop raw IQ recording with settings; auto-saves after --record-seconds (default 3).\n"
         "S = stack on/off (above 10 MS/s: light RGB rolling 6, full picture); -/+ = contest stack size; R = reset; M = fixed/rolling; P = averaged PNG; Ctrl+P = single image; Shift+P = contest pause/resume.");
}
static double number(const char *s)
{
    char *end; errno = 0; double v = strtod(s, &end);
    if (errno || end == s || *end || !isfinite(v)) { fprintf(stderr,"Invalid number: %s\n",s); exit(2); }
    return v;
}
/* Called under the UI mutex: keep requested changes even if RX is still applying them. */
static void current_settings(App *a, Settings *s)
{
    s->c=a->c;
    s->c.frequency=a->requested_hz+a->c.lo; s->c.gain=a->requested_gain;
    s->c.fm_mode=a->fm_mode;
    s->c.video_deemphasis=a->video_deemphasis;
    s->c.experimental_sync=a->experimental_sync;
    s->c.invert=a->invert; s->c.free_sync=a->free_sync;
    s->c.if_bandwidth=a->if_bandwidth; s->c.line_hz=a->line_hz;
    s->stacking=a->stacking; s->stack_frames=(int)a->stack.target;
    s->light_stacking=a->light_stacking;
    s->rolling=a->stack.rolling; s->paused=a->stack_paused;
    s->vertical_shift=a->vertical_shift;
    s->audio_source=SDL_AtomicGet(&a->audio_source);
    s->audio_muted=SDL_AtomicGet(&a->audio_muted);
    s->audio_swap=SDL_AtomicGet(&a->audio_swap);
    strcpy(s->my_call,a->my_call); strcpy(s->my_locator,a->my_locator);
}
static void apply_settings(App *a,const Settings *s)
{
    a->c=s->c;
    strcpy(a->my_call,s->my_call); strcpy(a->my_locator,s->my_locator);
    a->stacking=s->stacking; a->stack_paused=s->paused;
    a->light_stacking=s->light_stacking;
    a->vertical_shift=s->vertical_shift;
    SDL_AtomicSet(&a->audio_source,s->audio_source);
    SDL_AtomicSet(&a->audio_muted,s->audio_muted);
    SDL_AtomicSet(&a->audio_swap,s->audio_swap);
    stack_reset(&a->stack, (unsigned)s->stack_frames);
}
static int atv_main(int argc, char **argv)
{
    App *a = calloc(1, sizeof(*a));
    if (!a) return 1;
    a->if_power_dbfs=NAN;
    a->c = (Config){.mode="demo", .args="", .video_deemphasis=1, .images=ATV_DEFAULT_IMAGES,
        .status_bind="127.0.0.1", .status_port=8765,
        .antenna="TX/RX", .ref="internal", .rate=20e6, .frequency=1252e6, .bandwidth=18e6, .gain=30, .hackrf_vga=12, .hackrf_amp=1, .deviation=4e6, .if_bandwidth=18e6, .line_hz=15625, .rx_block=0, .record_seconds=3, .alignment_bandwidth=3200000};
    char config_path[4096];
#ifdef _WIN32
    char *pref=SDL_GetPrefPath("PE1ITR","ATV Receiver");
    if(!pref) { free(a); return 2; }
    int path_length=snprintf(config_path,sizeof(config_path),"%satv-rx.conf",pref);
    SDL_free(pref);
    if(path_length<0 || path_length>=(int)sizeof(config_path)) { free(a); return 2; }
#else
    const char *home=getenv("HOME");
    if(snprintf(config_path,sizeof(config_path),"%s/.atv-rx.conf",home&&*home?home:".")>=(int)sizeof(config_path)) { free(a); return 2; }
#endif
    int load_config=1, required_config=0, wizard=1;
    /* Read configuration before CLI options, so explicit options take precedence. */
    for(int j=1;j<argc;++j) {
        if(!strcmp(argv[j],"--version")) { printf("FM ATV %s (commit %s, built %s UTC)\n",ATV_VERSION,ATV_GIT_COMMIT,ATV_BUILD_UTC); free(a); return 0; }
        if(!strcmp(argv[j],"--help")) { help(); free(a); return 0; }
        if(!strcmp(argv[j],"--no-config")) load_config=0;
        if(!strcmp(argv[j],"--no-wizard")) wizard=0;
        if(!strcmp(argv[j],"--wizard")) wizard=1;
        if(!strcmp(argv[j],"--terminal-wizard")) wizard=2;
        if(!strcmp(argv[j],"--config")) {
            if(++j>=argc || strlen(argv[j])>=sizeof(config_path)) { fprintf(stderr,"Invalid --config path\n"); free(a); return 2; }
            strcpy(config_path,argv[j]); required_config=1;
        }
    }
    Settings settings={.c=a->c,.stack_frames=25};
    if(load_config && !settings_load(&settings,config_path,required_config)) { free(a); return 2; }
    snprintf(settings.profile_base,sizeof(settings.profile_base),"%s",config_path);
    settings.load_profiles=load_config;
    apply_settings(a,&settings);
    int rolling=settings.rolling, explicit_nicam=0, explicit_invert=0;
    for (int j = 1; j < argc; ++j) {
        const char *o = argv[j];
        if (!strcmp(o,"--no-config") || !strcmp(o,"--no-wizard") || !strcmp(o,"--wizard") || !strcmp(o,"--terminal-wizard")) continue;
        if (!strcmp(o,"--config")) { ++j; continue; }
        if (!strcmp(o,"--help")) { help(); free(a); return 0; }
        if (!strcmp(o,"--status-server")) { a->c.status_enabled=1; continue; }
        if (!strcmp(o,"--no-status-server")) { a->c.status_enabled=0; continue; }
        if (!strcmp(o,"--demo")) { a->c.mode="demo"; continue; }
        if (!strcmp(o,"--b210")) { a->c.mode="b210"; continue; }
        if (!strcmp(o,"--pluto")) { a->c.mode="pluto"; continue; }
        if (!strcmp(o,"--sdrplay")) { a->c.mode="sdrplay"; continue; }
        if (!strcmp(o,"--hackrf")) { a->c.mode="hackrf"; continue; }
        if (!strcmp(o,"--stack-rolling")) { rolling=1; a->stacking=1; a->light_stacking=0; continue; }
        if (!strcmp(o,"--loop")) { a->c.loop=1; continue; }
        if (!strcmp(o,"--experimental-sync") || !strcmp(o,"--experimental-vsync") ||
            !strcmp(o,"--experimental-echo-sync")) { a->c.experimental_sync=1; continue; }
        if (!strcmp(o,"--free-sync")) { a->c.free_sync=1; continue; }
        if (!strcmp(o,"--nicam")) { SDL_AtomicSet(&a->audio_source,1); explicit_nicam=1; continue; }
        if (!strcmp(o,"--invert")) { a->c.invert=1; explicit_invert=1; continue; }
        if (++j >= argc) { fprintf(stderr,"Missing value for %s\n",o); free(a); return 2; }
        const char *v = argv[j];
        if (!strcmp(o,"--iq-dc-block")) {
            if(!strcmp(v,"auto")) a->c.iq_dc_block=0;
            else if(!strcmp(v,"on")) a->c.iq_dc_block=1;
            else if(!strcmp(v,"off")) a->c.iq_dc_block=2;
            else { fprintf(stderr,"--iq-dc-block requires auto, on or off\n"); free(a); return 2; }
        }
        else if (!strcmp(o,"--args")) a->c.args=v;
        else if (!strcmp(o,"--status-bind")) a->c.status_bind=v;
        else if (!strcmp(o,"--status-port")) {
            double port=number(v);
            if(port<1 || port>65535 || floor(port)!=port) { fprintf(stderr,"--status-port requires 1..65535\n"); free(a); return 2; }
            a->c.status_port=(int)port;
        }
        else if (!strcmp(o,"--pluto-uri")) a->c.pluto_uri=v;
        else if (!strcmp(o,"--uhd-images-dir")) a->c.images=v;
        else if (!strcmp(o,"--antenna")) a->c.antenna=v;
        else if (!strcmp(o,"--snapshot")) a->c.snapshot=v;
        else if (!strcmp(o,"--record")) a->c.record=v;
        else if (!strcmp(o,"--record-seconds")) a->c.record_seconds=number(v);
        else if (!strcmp(o,"--ref")) a->c.ref=v;
        else if (!strcmp(o,"--file")) { a->c.file=v; a->c.mode="file"; }
        else if (!strcmp(o,"--frequency")) a->c.frequency=number(v);
        else if (!strcmp(o,"--lo")) a->c.lo=number(v);
        else if (!strcmp(o,"--rate")) a->c.rate=number(v);
        else if (!strcmp(o,"--rx-block")) {
            double n=number(v);
            if(n<2048 || n>1048576 || floor(n)!=n) {
                fprintf(stderr,"--rx-block requires 2048..1048576 whole samples\n"); free(a); return 2;
            }
            a->c.rx_block=(size_t)n;
        }
        else if (!strcmp(o,"--bandwidth")) a->c.bandwidth=number(v);
        else if (!strcmp(o,"--if-bandwidth")) a->c.if_bandwidth=number(v);
        else if (!strcmp(o,"--alignment-bandwidth")) a->c.alignment_bandwidth=number(v);
        else if (!strcmp(o,"--line-hz")) a->c.line_hz=number(v);
        else if (!strcmp(o,"--fm-mode")) {
            if(strcmp(v,"0") && strcmp(v,"1") && strcmp(v,"2")) { fprintf(stderr,"--fm-mode requires 0, 1 or 2\n"); free(a); return 2; }
            a->c.fm_mode=atoi(v);
        }
        else if (!strcmp(o,"--hackrf-amp")) {
            if(strcmp(v,"0") && strcmp(v,"1")) { fprintf(stderr,"--hackrf-amp requires 0 or 1\n"); free(a); return 2; }
            a->c.hackrf_amp=atoi(v);
        }
        else if (!strcmp(o,"--hackrf-vga")) a->c.hackrf_vga=number(v);
        else if (!strcmp(o,"--gain")) a->c.gain=number(v);
        else if (!strcmp(o,"--video-deemphasis")) {
            if(!strcmp(v,"pal")) a->c.video_deemphasis=1;
            else if(!strcmp(v,"off")) a->c.video_deemphasis=0;
            else { fprintf(stderr,"--video-deemphasis requires pal or off\n"); free(a); return 2; }
        }
        else if (!strcmp(o,"--deviation")) a->c.deviation=number(v);
        else if (!strcmp(o,"--seconds")) a->c.seconds=number(v);
        else if (!strcmp(o,"--stack-frames")) {
            double n = number(v);
            if (n < 1 || n > 512 || floor(n) != n) { free(a); return 2; }
            stack_reset(&a->stack, (unsigned)n); a->stacking = 1; a->light_stacking=0;
        }
        else if (!strcmp(o,"--channel")) {
            double ch = number(v); if (ch < 0 || ch > 1 || floor(ch) != ch) { free(a); return 2; }
            a->c.channel=(size_t)ch;
        } else { fprintf(stderr,"Unknown option: %s\n",o); free(a); return 2; }
    }
    if(!a->c.rx_block) a->c.rx_block=settings_default_rx_block(a->c.mode);
configure_receiver:
    settings.c=a->c;
    settings.stacking=a->stacking; settings.stack_frames=(int)a->stack.target;
    settings.light_stacking=a->light_stacking; settings.paused=a->stack_paused;
    settings.rolling=rolling; settings.vertical_shift=a->vertical_shift;
    settings.audio_source=SDL_AtomicGet(&a->audio_source);
    settings.audio_muted=SDL_AtomicGet(&a->audio_muted);
    settings.audio_swap=SDL_AtomicGet(&a->audio_swap);
    if(wizard) {
        int setup=wizard==2?settings_wizard(&settings,stdin,stdout):graphical_wizard(&settings);
        if(setup<=0) { free(a); return setup<0?1:0; }
    }
    apply_settings(a,&settings);
    /* Polarity always starts normal unless explicitly requested on the CLI,
     * including when a saved configuration or SDR profile has invert enabled. */
    a->c.invert=explicit_invert;
    rolling=settings.rolling;
    if(!strcmp(a->c.mode,"file") && (!a->c.file || !*a->c.file)) { fprintf(stderr,"File source requires a path\n"); free(a); return 2; }
    /* CLI uses the frequency ahead of the converter. Keep all internal
     * tuning state in SDR Hz; the UI adds the LO back after hardware tuning. */
    double sdr_frequency=a->c.frequency-a->c.lo;
    if(a->c.frequency<=0 || !isfinite(sdr_frequency) || sdr_frequency<=0 || sdr_frequency>6e9) {
        fprintf(stderr,"Invalid frequency: --frequency minus --lo must be above 0 and at most 6000000000 Hz\n");
        free(a); return 2;
    }
    a->c.frequency=sdr_frequency;
    if(!strcmp(a->c.mode,"sdrplay") &&
       (a->c.rate<8e6 || a->c.rate>=10e6 || a->c.bandwidth>8e6 ||
        sdr_frequency<1000 || sdr_frequency>2e9 || a->c.channel!=0)) {
        fprintf(stderr,"SDRplay: rate must be 8 <= MS/s < 10, analog BW <= 8 MHz, RF 1 kHz..2 GHz, channel 0. Use --lo for a converter.\n");
        free(a); return 2;
    }
    if (a->c.rate < settings_min_rate(a->c.mode) || a->c.rate > 64e6 || a->c.bandwidth <= 0 || a->c.bandwidth > a->c.rate ||
        a->c.frequency <= 0 || a->c.frequency > 6e9 || a->c.deviation <= 0 || a->c.deviation > 32e6 ||
        a->c.if_bandwidth < 250000 || a->c.if_bandwidth > a->c.rate || a->c.line_hz < 15000 || a->c.line_hz > 16250 || a->c.seconds < 0 || a->c.gain < 0 || a->c.gain > 100) {
        fprintf(stderr,"Invalid range: check --help (rate %.0f..64 MS/s, BW <= rate, deviation 1..32000000, gain 0..100)\n",settings_min_rate(a->c.mode)/1e6); free(a); return 2;
    }
    if(!isfinite(a->c.hackrf_vga) || a->c.hackrf_vga<0 || a->c.hackrf_vga>62 || fmod(a->c.hackrf_vga,2)!=0) {
        fprintf(stderr,"HackRF VGA must be 0..62 dB in 2 dB steps\n"); free(a); return 2;
    }
    if(a->c.alignment_bandwidth!=0 && (a->c.alignment_bandwidth<250000 || a->c.alignment_bandwidth>a->c.rate)) {
        fprintf(stderr,"Alignment bandwidth must be 0 or 250000..sample rate\n"); free(a); return 2;
    }
    if(a->c.record_seconds<.001 || a->c.record_seconds>15 ||
       (a->c.record && a->c.rate*a->c.record_seconds*sizeof(IQ)>2147483648.0)) {
        fprintf(stderr,"Recording requires 0.001..15 seconds and at most 2 GiB\n"); free(a); return 2;
    }
    if(SDL_AtomicGet(&a->audio_source) && !sound_enabled(a->c.rate,a->c.if_bandwidth)) {
        if(explicit_nicam) { fprintf(stderr,"--nicam requires rate and video IF >= 14 MHz\n"); free(a); return 2; }
        fprintf(stderr,"Selected audio unavailable: rate and video IF must be >= 14 MHz\n");
    }
    a->source = source_open(&a->c);
    if(a->source && a->c.lo) fprintf(stderr,"Converter LO: %+.6f MHz; SDR frequency: %.6f MHz; displayed frequency: %.6f MHz\n",
        a->c.lo/1e6,a->c.frequency/1e6,(a->c.frequency+a->c.lo)/1e6);
    if(a->source) fprintf(stderr,"RX block: %zu samples (%.3f ms at %.3f MS/s)\n",
        a->c.rx_block,1000*a->c.rx_block/a->c.rate,a->c.rate/1e6);
    if(a->source && a->c.alignment_bandwidth>0)
        fprintf(stderr,"Separate sync/alignment IF: %.3f MHz; video IF: %.3f MHz\n",a->c.alignment_bandwidth/1e6,a->c.if_bandwidth/1e6);
    if (!a->source) { free(a); return 1; }
    if(stacking_limited(&a->c)) {
        if(a->stacking) a->light_stacking=1;
        a->stacking=0;
        fprintf(stderr,"Live SDR %.3f MS/s: S toggles lightweight RGB rolling stack of 6 fields\n",a->c.rate/1e6);
    }
    if(a->c.rate<=10e6) a->light_stacking=0;
    if(a->light_stacking) { a->stacking=0; a->stack_paused=0; }
    if (init_ui()) { fprintf(stderr,"SDL: %s\n",SDL_GetError()); source_close(a->source); free(a); return 1; }
    if(a->c.rate>=14e6) {
        a->audio_device=open_audio_output(a->c.audio_output);
    }
    a->mutex = SDL_CreateMutex();
    SDL_Window *window = create_ui_window("FM ATV - PAL",UI_WIDTH,UI_HEIGHT);
    SDL_Renderer *r = window ? SDL_CreateRenderer(window, -1, 0) : NULL;
    SDL_Texture *texture = r ? SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, WIDTH, HEIGHT) : NULL;
    SDL_Thread *thread = NULL, *stack_thread = NULL;
    View *view=malloc(sizeof(*view));
    ScreenshotExposure *exposure=NULL;
    a->stack_ready=SDL_CreateCond();
    StatusServer *status_server=NULL;
    DiagnosticLog *diagnosis=NULL;
    int result = 1, reopen_setup=0;
    if (!a->mutex || !a->stack_ready || !window || !r || !texture || !view || !ui_text_init(r)) goto cleanup;
    diagnosis=diagnostic_log_create(diagnostic_snapshot,a);
    SDL_RenderSetLogicalSize(r,UI_WIDTH,UI_HEIGHT);
    a->afc_anchor=a->c.frequency;
    a->requested_hz=a->actual_hz=a->c.frequency; a->requested_gain=a->actual_gain=a->c.gain; a->invert=a->c.invert;
    a->fm_mode=a->c.fm_mode;
    a->video_deemphasis=a->c.video_deemphasis;
    a->experimental_sync=a->c.experimental_sync;
    a->if_bandwidth=a->c.if_bandwidth; a->line_hz=a->c.line_hz; a->free_sync=a->c.free_sync;
    for (int k=0;k<FFT_SIZE;++k) a->spectrum[k]=-120;
    a->recorder=iq_recorder_create();
    if(!a->recorder) fprintf(stderr,"F9 IQ recording unavailable: %s\n",SDL_GetError());
    reset_stack(a,a->stack.target,rolling);
    if(a->c.status_enabled) {
        status_server=status_server_open(a->c.status_bind,a->c.status_port);
        if(!status_server) goto cleanup;
    }
    stack_thread=SDL_CreateThread(stack_worker,"Frame-stack",a);
    if(!stack_thread) goto cleanup;
    thread = SDL_CreateThread(worker,"RX-DSP",a);
    if (!thread) goto cleanup;
    uint32_t start = SDL_GetTicks();
    int screenshot_requested=0, save_result=0, diagnostic_screenshot_requested=0;
    uint32_t save_notice=0, screenshot_notice=0, if_notice=0;
    uint32_t record_notice=0;
    const char *record_warning=NULL;
    FrequencyInput frequency_input={0};
    StationInput station_input={0};
    InfoScreen info_screen={0};
    SpectrumScale spectrum_scale={0};
    uint32_t info_opened=0;
    SDL_StopTextInput();
    /* Keep a completed file visible for inspection; errors still exit. */
    while (!SDL_AtomicGet(&a->stop) && !SDL_AtomicGet(&a->failed) &&
           (!SDL_AtomicGet(&a->done) || !strcmp(a->c.mode,"file"))) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if(fullscreen_shortcut(&e)) continue;
            /* SDL maps events into logical window coordinates; the existing
             * controls are drawn in the centre viewport. */
            if(e.type==SDL_MOUSEBUTTONDOWN || e.type==SDL_MOUSEBUTTONUP)
                e.button.x-=UI_SIDE_WIDTH;
            if(e.type==SDL_MOUSEMOTION) e.motion.x-=UI_SIDE_WIDTH;
            if(!frequency_input.active && !station_input.active && !info_screen.active) {
                side_control_event(&e);
            }
            int info_was_active=info_screen.active;
            if(!frequency_input.active && !station_input.active && info_screen_event(&info_screen,&e)) {
                if(info_screen.active!=info_was_active) {
                    if(info_screen.active) info_opened=SDL_GetTicks();
                    else start+=SDL_GetTicks()-info_opened;
                    SDL_LockMutex(a->mutex);
                    a->info_paused=info_screen.active;
                    SDL_CondBroadcast(a->stack_ready);
                    SDL_UnlockMutex(a->mutex);
                }
                continue;
            }
            if(!frequency_input.active && station_input_event(&station_input,a,&e)) continue;
            if(frequency_input_event(&frequency_input,a,&e)) continue;
            if(e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_x && !e.key.repeat) {
                reopen_setup=1; SDL_AtomicSet(&a->stop,1); break;
            }
            if(e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_F10 && !e.key.repeat) {
                if(e.key.keysym.mod & KMOD_SHIFT) {
                    if(diagnostic_log_state(diagnosis)==DIAG_SAVED) SDL_SetClipboardText(diagnostic_log_path(diagnosis));
                } else if(!diagnostic_screenshot_requested) {
                    int was_recording=diagnostic_log_state(diagnosis)==DIAG_RECORDING;
                    if(!was_recording) a->diagnosis_previous_tick=0;
                    diagnostic_log_toggle(diagnosis);
                    if(!was_recording && diagnostic_log_state(diagnosis)==DIAG_RECORDING)
                        diagnostic_screenshot_requested=1;
                }
                continue;
            }
            double delta=0, gain_delta=0;
            double step = SDL_GetModState() & KMOD_CTRL ? 1e4 : SDL_GetModState() & KMOD_SHIFT ? 1e6 : 1e5;
            if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN &&
                (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_q))) SDL_AtomicSet(&a->stop,1);
            if (e.type == SDL_MOUSEWHEEL) delta=e.wheel.y*step;
            if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym==SDLK_LEFT) delta=-step;
                if (e.key.keysym.sym==SDLK_RIGHT) delta=step;
                if (e.key.keysym.sym==SDLK_UP) gain_delta=1;
                if (e.key.keysym.sym==SDLK_DOWN) gain_delta=-1;
            }
            if (e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT && e.button.y>=630 && e.button.y<=770 && e.button.x>=60 && e.button.x<=920)
                delta=((e.button.x-60)/860.0-.5)*a->c.rate;
            SDL_LockMutex(a->mutex);
            if (delta || gain_delta) {
                if(delta && (e.type==SDL_KEYDOWN || e.type==SDL_MOUSEWHEEL))
                    a->requested_hz=tuning_grid(a->requested_hz,a->c.lo,step,delta/step);
                else a->requested_hz=fmin(6e9,fmax(1,a->requested_hz+delta));
                if(delta) afc_manual_tune(a);
                a->requested_gain=fmin(!strcmp(a->c.mode,"hackrf")?40:!strcmp(a->c.mode,"sdrplay")?39:100,
                    fmax(0,a->requested_gain+gain_delta*(!strcmp(a->c.mode,"hackrf")?8:1))); a->command|=1;
            }
            if(e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_c && !e.key.repeat) afc_toggle(a);
            if (e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_i) { a->invert=!a->invert; a->command|=2; }
            if(e.type==SDL_KEYDOWN) {
                SDL_Keycode key=e.key.keysym.sym;
                if(key==SDLK_n && !e.key.repeat && sound_enabled(a->c.rate,a->if_bandwidth)) {
                    if(e.key.keysym.mod & KMOD_SHIFT) {
                        if(SDL_AtomicGet(&a->audio_source)>=2 && stereo_sound_enabled(a->c.rate,a->if_bandwidth))
                            SDL_AtomicSet(&a->audio_swap,!SDL_AtomicGet(&a->audio_swap));
                    }
                    else {
                        int next=(SDL_AtomicGet(&a->audio_source)+1)%4;
                        if(next>=2 && !stereo_sound_enabled(a->c.rate,a->if_bandwidth)) next=0;
                        SDL_AtomicSet(&a->audio_source,next);
                    }
                    a->command|=4;
                }
                if(key==SDLK_a && !e.key.repeat) SDL_AtomicSet(&a->audio_muted,!SDL_AtomicGet(&a->audio_muted));
                if(key==SDLK_d && !e.key.repeat) { a->fm_mode=(a->fm_mode+1)%3; a->command|=2; }
                if(key==SDLK_e && !e.key.repeat) { a->video_deemphasis^=1; a->command|=2; }
                if(key==SDLK_g && !e.key.repeat) { a->experimental_sync^=1; a->command|=2; }
                if(key==SDLK_f) { a->free_sync=!a->free_sync; a->command|=2; }
                if(key==SDLK_LEFTBRACKET || key==SDLK_RIGHTBRACKET) {
                    double previous_if=a->if_bandwidth;
                    a->if_bandwidth=fmin(a->c.rate,fmax(250000,a->if_bandwidth+(key==SDLK_LEFTBRACKET?-1e6:1e6))); a->command|=2;
                    if(a->if_bandwidth!=previous_if) if_notice=SDL_GetTicks();
                }
                if(key==SDLK_COMMA || key==SDLK_PERIOD) {
                    a->line_hz=fmin(16250,fmax(15000,a->line_hz+(key==SDLK_COMMA?-.1:.1))); a->command|=2;
                }
                if(key==SDLK_b || key==SDLK_v) {
                    a->vertical_shift+=key==SDLK_b?-1:1;
                    if(a->vertical_shift>312) a->vertical_shift=312;
                    if(a->vertical_shift< -312) a->vertical_shift=-312;
                    a->command|=2;
                }
            }
            int action = 0;
            if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                SDL_Keycode key = e.key.keysym.sym;
                if(key==SDLK_F9) {
                    record_notice=SDL_GetTicks();
                    record_warning=a->c.record?"CLI --record is active; F9 recording is unavailable":
                        !a->recorder?"Cannot start the IQ recorder worker":NULL;
                    if(!record_warning) {
                        IQRecordSettings recording={.c=a->c,.audio_source=SDL_AtomicGet(&a->audio_source),.vertical_shift=a->vertical_shift};
                        recording.c.video_deemphasis=a->video_deemphasis;
                        recording.c.experimental_sync=a->experimental_sync;
                        recording.c.frequency=a->actual_hz; recording.c.gain=a->actual_gain;
                        recording.c.if_bandwidth=a->if_bandwidth; recording.c.line_hz=a->line_hz;
                        recording.c.fm_mode=a->fm_mode; recording.c.invert=a->invert; recording.c.free_sync=a->free_sync;
                        iq_recorder_toggle(a->recorder,&recording);
                    }
                }
                int timebase=SDL_AtomicGet(&a->scope_timebase);
                int step=(key==SDLK_PAGEUP)-(key==SDLK_PAGEDOWN);
                if(key==SDLK_o || (step && SDL_AtomicGet(&a->scope_enabled) &&
                   timebase+step>=0 && timebase+step<VIDEO_SCOPE_SCALES)) {
                    if(key==SDLK_o) SDL_AtomicSet(&a->scope_enabled,!SDL_AtomicGet(&a->scope_enabled));
                    else SDL_AtomicSet(&a->scope_timebase,timebase+step);
                    SDL_AtomicAdd(&a->scope_epoch,1);
                    memset(&a->scope,0,sizeof(a->scope)); a->scope_tick=0;
                }
                if (key == SDLK_s) action = 1;
                if (key == SDLK_MINUS || key == SDLK_KP_MINUS) action = 2;
                if (key == SDLK_EQUALS || key == SDLK_PLUS || key == SDLK_KP_PLUS) action = 3;
                if (key == SDLK_r) action = 4;
                if (key == SDLK_p) {
                    if(e.key.keysym.mod & KMOD_SHIFT) action=5;
                    else {
                        screenshot_requested=(e.key.keysym.mod & KMOD_CTRL)?2:1;
                        screenshot_notice=SDL_GetTicks();
                        if(exposure) memset(exposure,0,sizeof(*exposure));
                    }
                }
                if (key == SDLK_m) action = 6;
            }
            stack_action(a,action);
            int save=e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_w && !e.key.repeat;
            Settings snapshot={0};
            if(save) current_settings(a,&snapshot);
            SDL_UnlockMutex(a->mutex);
            if(save) {
                save_result=settings_save(&snapshot,config_path);
                save_notice=SDL_GetTicks();
                if(save_result) fprintf(stderr,"Settings saved: %s\n",config_path);
                else fprintf(stderr,"Save failed (%s): %s\n",config_path,strerror(errno));
            }
        }
        status_server_poll(status_server,status_json,a);
        diagnostic_log_poll(diagnosis);
        SDL_Rect full_viewport;
        SDL_RenderGetViewport(r,&full_viewport);
        SDL_SetRenderDrawColor(r,14,19,27,255); SDL_RenderClear(r);
        SDL_Rect centre={full_viewport.x+UI_SIDE_WIDTH,full_viewport.y,960,UI_HEIGHT};
        SDL_RenderSetViewport(r,&centre);
        if(info_screen.active) {
            draw_info_screen(r,&a->c);
            SDL_RenderSetViewport(r,&full_viewport);
            SDL_RenderPresent(r); SDL_Delay(16);
            continue;
        }
        capture_view(a,view);
        const View *v=view;
        int screenshot_ready=screenshot_requested==2 || (screenshot_requested && (v->scope_enabled || v->light_stacking));
        if(screenshot_requested==1 && !v->scope_enabled && !v->light_stacking) {
            if(!exposure) exposure=calloc(1,sizeof(*exposure));
            if(!exposure) {
                fprintf(stderr,"Screenshot: cannot allocate exposure buffer\n");
                screenshot_requested=0;
            } else screenshot_ready=screenshot_expose(exposure,v,SDL_GetTicks());
        }
        int averaged_photo=screenshot_ready && screenshot_requested==1 && !v->scope_enabled && !v->light_stacking;
        const uint32_t *photo=averaged_photo?exposure->pixels:NULL;
        if(v->scope_enabled) draw_video_scope(r,v);
        else {
            SDL_UpdateTexture(texture,NULL,photo && !v->stacking?photo:v->pixels,WIDTH*sizeof(uint32_t));
            SDL_Rect video=v->stacking ? (SDL_Rect){10,160,460,345} : (SDL_Rect){120,65,720,540};
            SDL_RenderCopy(r,texture,NULL,&video);
            if(v->stacking) {
                SDL_UpdateTexture(texture,NULL,photo?photo:v->live,WIDTH*sizeof(uint32_t));
                SDL_Rect live={490,160,460,345}; SDL_RenderCopy(r,texture,NULL,&live);
                SDL_SetRenderDrawColor(r,220,230,240,255);
                text(r,20,135,v->stack_paused ? "STACK PAUSED" :
                    v->stack.status ? v->stack.status : "SEEKING ALIGNMENT",2);
                text(r,500,135,"LIVE",2);
            }
        }
        SDL_SetRenderDrawColor(r,220,230,240,255);
        char buf[256];
        time_t timestamp=time(NULL);
        struct tm utc;
        char utc_text[32];
        if(gmtime_r(&timestamp,&utc) && strftime(utc_text,sizeof(utc_text),"%Y-%m-%d %H:%M:%S UTC",&utc)) {
            snprintf(buf,sizeof(buf),"%s%s%s%s%s",utc_text,
                *a->my_call?"  ":"",a->my_call,*a->my_locator?"  ":"",a->my_locator);
            /* Scale 2 uses eight-pixel cells; both video layouts centre at x=480. */
            int caption_x=(960-(int)strlen(buf)*8)/2;
            text(r,caption_x,v->stacking && !v->scope_enabled?515:612,buf,2);
        }
        snprintf(buf,sizeof(buf),"%.3f MHZ",(v->actual_hz+v->c.lo)/1e6);
        text(r,20,8,buf,6);
        int settings_x=20+(int)strlen(buf)*24+24;
        const char *sdr_type=!strcmp(v->c.mode,"b210")?"B210":
            !strcmp(v->c.mode,"hackrf")?"HackRF":!strcmp(v->c.mode,"pluto")?"PlutoSDR":
            !strcmp(v->c.mode,"sdrplay")?"SDRplay RSP1B":!strcmp(v->c.mode,"demo")?"Demo":
            !strcmp(v->c.mode,"file")?"File":v->c.mode;
        snprintf(buf,sizeof(buf),"SDR: %s  %s: %.0f dB  Samplerate: %.2f MS/s  BW: %.2f MHz",
            sdr_type,!strcmp(v->c.mode,"hackrf")?"LNA":"Gain",v->actual_gain,v->c.rate/1e6,v->c.bandwidth/1e6);
        text(r,settings_x,15,buf,2);
        text(r,settings_x,30,"T FREQUENCY MHZ",2);
        /* Keep the former status row empty without moving the video up. */
        SDL_Rect status_space={0,45,960,16};
        SDL_SetRenderDrawColor(r,0,0,0,255); SDL_RenderFillRect(r,&status_space);
        spectrum_scale_update(&spectrum_scale,v->spectrum);
        for (int db=spectrum_scale.top_db;db>=spectrum_scale.bottom_db;db-=spectrum_scale.step_db) {
            int y=spectrum_y(&spectrum_scale,(float)db);
            SDL_SetRenderDrawColor(r,45,55,66,255); SDL_RenderDrawLine(r,60,y,920,y);
            snprintf(buf,sizeof(buf),"%d",db); SDL_SetRenderDrawColor(r,140,155,170,255); text(r,20,y-3,buf,2);
        }
        SDL_SetRenderDrawColor(r,160,115,60,255);
        int left=490-(int)(430*v->if_bandwidth/v->c.rate), right=490+(int)(430*v->if_bandwidth/v->c.rate);
        SDL_RenderDrawLine(r,left,630,left,770); SDL_RenderDrawLine(r,right,630,right,770);
        SDL_SetRenderDrawColor(r,75,220,170,255);
        for (int x=0;x<859;++x) {
            int j=x*FFT_SIZE/860, k=(x+1)*FFT_SIZE/860;
            float peak=-160;
            for (int z=j;z<=k && z<FFT_SIZE;++z) peak=fmaxf(peak,v->spectrum[z]);
            int y=spectrum_y(&spectrum_scale,peak);
            SDL_RenderDrawPoint(r,60+x,y);
        }
        /* Zero IF is the actual hardware centre, including AFC correction. */
        SDL_SetRenderDrawColor(r,255,210,80,255);
        SDL_RenderDrawLine(r,490,630,490,770);
        SDL_SetRenderDrawColor(r,140,155,170,255);
        for(int k=0;k<3;++k) {
            snprintf(buf,sizeof(buf),"%.3f MHZ",(v->actual_hz+v->c.lo+(k-1)*v->c.rate/2)/1e6);
            text(r,60+k*365,780,buf,2);
        }

        if (v->stacking) {
            if(v->stack.rolling && v->c.rate<=10e6) snprintf(buf,sizeof(buf),"LIVE ROLLING RGB  SKIP %llu",(unsigned long long)v->stack_skipped);
            else if(v->stack_synced) snprintf(buf,sizeof(buf),"SYNC ALIGN  H AND V CONFIRMED  SKIP %llu",(unsigned long long)v->stack_skipped);
            else snprintf(buf,sizeof(buf),"IMAGE ALIGN  MATCH %.2f  X %.1f  Y %.1f  SKEW %.1f  SKIP %llu",
                v->stack.confidence,v->stack.dx,v->stack.dy,v->stack.shear,(unsigned long long)v->stack_skipped);
            text(r,20,866,buf,2);
        }

        int selected=SDL_AtomicGet(&a->audio_source);
        const char *audio_source=selected==3?"FM 7.38/7.56":selected==2?"FM 7.02/7.22":selected==1?"NICAM":"FM 6";
        if(!sound_enabled(v->c.rate,v->if_bandwidth))
            buf[0]='\0';
        else if(selected>=2 && !stereo_sound_enabled(v->c.rate,v->if_bandwidth))
            snprintf(buf,sizeof(buf),"STEREO OFF: RATE/IF >=16M | N SOURCE");
        else if(!a->audio_device)
            snprintf(buf,sizeof(buf),"%s: NO AUDIO DEVICE | N SOURCE",audio_source);
        else if(SDL_AtomicGet(&a->audio_muted))
            snprintf(buf,sizeof(buf),"%s MUTED | A UNMUTE | N SOURCE",audio_source);
        else if(selected>=2)
            snprintf(buf,sizeof(buf),"%s %s %s | N SOURCE",selected==3?"7.38/7.56":"7.02/7.22",SDL_AtomicGet(&a->audio_locked)?"LOCK":"SEEK",SDL_AtomicGet(&a->audio_swap)?"R/L":"L/R");
        else if(selected==1)
            snprintf(buf,sizeof(buf),"NICAM %s | N SOURCE | A MUTE",SDL_AtomicGet(&a->nicam_lock)?"LOCK":"SEEK");
        else
            snprintf(buf,sizeof(buf),"FM %s | %s",SDL_AtomicGet(&a->audio_locked)?"LOCK":"SEEK",
                SDL_AtomicGet(&a->nicam_detect)?"NICAM DETECT | N SOURCE":"N NICAM | A MUTE");
        text(r,550,803,buf,2);
        uint32_t clip_now=SDL_GetTicks();
        int repeated_clipping=clipping_repeated(&v->clipping,clip_now);
        if(clipping_active(&v->clipping,clip_now) &&
           (repeated_clipping || clip_now-v->clipping.last_clip_tick<500)) {
            SDL_Rect bar={120,65,720,26};
            SDL_SetRenderDrawColor(r,repeated_clipping?200:150,repeated_clipping?0:95,0,255); SDL_RenderFillRect(r,&bar);
            SDL_SetRenderDrawColor(r,255,255,255,255);
            snprintf(buf,sizeof(buf),"%s  %llu TOTAL SAMPLES",
                repeated_clipping?"REPEATED CLIPPING - LOWER GAIN":"IQ PEAK",
                (unsigned long long)v->clipping.clipped_samples);
            text(r,130,72,buf,2);
        }
        if(station_input.active) {
            SDL_Rect panel={40,250,880,270};
            SDL_SetRenderDrawColor(r,24,34,46,255); SDL_RenderFillRect(r,&panel);
            SDL_SetRenderDrawColor(r,100,180,220,255); SDL_RenderDrawRect(r,&panel);
            SDL_SetRenderDrawColor(r,235,240,245,255);
            text(r,60,267,"MY STATION",3);
            for(int i=0;i<2;++i) {
                int y=305+70*i;
                SDL_Rect field={60,y,840,40};
                SDL_SetRenderDrawColor(r,station_input.field==i?40:24,station_input.field==i?65:34,station_input.field==i?85:46,255);
                SDL_RenderFillRect(r,&field);
                SDL_SetRenderDrawColor(r,235,240,245,255);
                snprintf(buf,sizeof(buf),"%s: %s%s",i?"my_locator":"my_call",
                    i?station_input.my_locator:station_input.my_call,station_input.field==i?"_":"");
                text(r,70,y+12,buf,2);
            }
            text(r,60,437,"TAB NEXT FIELD   ENTER NEXT / APPLY   ESC CANCEL",2);
            text(r,60,460,"CTRL+A CLEAR FIELD   W SAVES SETTINGS AFTER APPLY",2);
            if(station_input.error) {
                SDL_SetRenderDrawColor(r,255,170,110,255);
                text(r,60,490,station_input.error,2);
            }
        }
        if(frequency_input.active) {
            SDL_Rect panel={40,250,880,190};
            SDL_SetRenderDrawColor(r,24,34,46,255); SDL_RenderFillRect(r,&panel);
            SDL_SetRenderDrawColor(r,100,180,220,255); SDL_RenderDrawRect(r,&panel);
            SDL_SetRenderDrawColor(r,235,240,245,255);
            text(r,60,270,"FREQUENCY IN MHZ",3);
            snprintf(buf,sizeof(buf),"%s_",frequency_input.value);
            text(r,60,307,buf,3);
            snprintf(buf,sizeof(buf),"CURRENT %.6f MHZ   CONVERTER LO %+.6f MHZ",
                (v->actual_hz+v->c.lo)/1e6,v->c.lo/1e6);
            text(r,60,350,buf,2);
            text(r,60,377,"ENTER TUNE   ESC CANCEL   BACKSPACE DELETE   CTRL+A CLEAR",2);
            if(frequency_input.error) {
                SDL_SetRenderDrawColor(r,255,170,110,255);
                text(r,60,407,frequency_input.error,2);
            }
        }
        draw_iq_recording(r,&v->recording,record_warning && SDL_GetTicks()-record_notice<8000?record_warning:NULL);
        SDL_RenderSetViewport(r,&full_viewport);
        draw_side_panels(r,v,save_notice,save_result,screenshot_notice,if_notice);
        draw_diagnosis(r,diagnosis);
        if(diagnostic_screenshot_requested) {
            ScreenshotResult shot={0};
            const char *directory=diagnostic_log_directory(diagnosis);
            save_window_png(r,v->actual_hz+v->c.lo,directory,v->stacking?v->live:v->pixels,&shot);
            diagnostic_log_screenshot(diagnosis,*shot.path?shot.path:directory,shot.error,shot.capture_issue);
            diagnostic_screenshot_requested=0;
        }
        if(screenshot_ready) {
            ScreenshotResult shot={0};
            if(photo) fprintf(stderr,"Screenshot exposure: %u distinct displayed fields over %u ms\n",
                exposure->count,SDL_GetTicks()-exposure->started);
            save_window_png(r,v->actual_hz+v->c.lo,"",NULL,&shot);
            screenshot_requested=0;
            free(exposure); exposure=NULL;
        }
        /* The averaged image is for the PNG only. Keep the last live image on
         * screen during this one redraw; the next iteration presents live. */
        if(!averaged_photo) SDL_RenderPresent(r);
        if (a->c.seconds>0 && (SDL_GetTicks()-start)/1000.0>=a->c.seconds) SDL_AtomicSet(&a->stop,1);
        SDL_Delay(16);
    }
    SDL_AtomicSet(&a->stop,1);
    SDL_LockMutex(a->mutex); SDL_CondBroadcast(a->stack_ready); SDL_UnlockMutex(a->mutex);
    SDL_WaitThread(thread,NULL); thread=NULL;
    SDL_WaitThread(stack_thread,NULL); stack_thread=NULL;
    result=SDL_AtomicGet(&a->failed)?1:0;
    if (a->c.snapshot) {
        int w,h; SDL_GetRendererOutputSize(r,&w,&h);
        SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormat(0,w,h,32,SDL_PIXELFORMAT_ARGB8888);
        if (!surface || SDL_RenderReadPixels(r,NULL,SDL_PIXELFORMAT_ARGB8888,surface->pixels,surface->pitch) || SDL_SaveBMP(surface,a->c.snapshot)) {
            fprintf(stderr,"Snapshot: %s\n",SDL_GetError()); result=1;
        }
        if (surface) SDL_FreeSurface(surface);
    }
    if (a->stacking) fprintf(stderr,"Stacked fields: %u/%u\n",a->stack.count,a->stack.target);
    if(a->light_stacking) fprintf(stderr,"Light rolling stack: %u/%u fields\n",a->light_count,LIGHT_STACK_FRAMES);
    if(a->stack.rolling) fprintf(stderr,"Rolling accepted fields: %llu\n",(unsigned long long)a->stack.accepted);
    fprintf(stderr,"Stack frames skipped: %llu\n",(unsigned long long)a->stack_skipped);
    fprintf(stderr,"Displayed fields: %u; stream discontinuities: %u\n",a->frames,a->overflows);
cleanup:
    diagnostic_log_destroy(diagnosis);
    status_server_close(status_server);
    if (result && !SDL_AtomicGet(&a->failed)) fprintf(stderr,"UI: %s\n",SDL_GetError());
    if(thread || stack_thread) {
        SDL_AtomicSet(&a->stop,1);
        SDL_LockMutex(a->mutex); SDL_CondBroadcast(a->stack_ready); SDL_UnlockMutex(a->mutex);
        if(thread) SDL_WaitThread(thread,NULL);
        if(stack_thread) SDL_WaitThread(stack_thread,NULL);
    }
    if(reopen_setup && !result) current_settings(a,&settings);
    if (texture) SDL_DestroyTexture(texture);
    iq_recorder_destroy(a->recorder);
    free(view);
    free(exposure);
    if (r) { ui_text_destroy(r); SDL_DestroyRenderer(r); }
    if (window) SDL_DestroyWindow(window);
    if(a->stack_ready) SDL_DestroyCond(a->stack_ready);
    if (a->mutex) SDL_DestroyMutex(a->mutex);
    if(a->audio_device) SDL_CloseAudioDevice(a->audio_device);
    source_close(a->source); stack_destroy(&a->stack); free(a);
    if(reopen_setup && !result) {
        a=calloc(1,sizeof(*a));
        if(!a) return 1;
        a->if_power_dbfs=NAN;
        apply_settings(a,&settings);
        rolling=settings.rolling;
        wizard=1; settings.reopen_menu=1;
        goto configure_receiver;
    }
    return result;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    char log_path[4096];
    char *pref=SDL_GetPrefPath("PE1ITR","ATV Receiver");
    snprintf(log_path,sizeof(log_path),"%satv-rx.log",pref?pref:"");
    SDL_free(pref);
    FILE *log=freopen(log_path,"w",stderr);
    if(log) setvbuf(stderr,NULL,_IONBF,0);
    char *base=SDL_GetBasePath();
    if(base) {
        char modules[4096];
        snprintf(modules,sizeof(modules),"%smodules",base);
        SDL_setenv("SOAPY_SDR_PLUGIN_PATH",modules,1);
        SDL_free(base);
    }
#endif
    int result=atv_main(argc,argv);
#ifdef _WIN32
    if(result) {
        char message[4608];
        snprintf(message,sizeof(message),"Startup or reception failed. Check the SDR connection and settings.\n\nDetails are in:\n%s",log_path);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"FM ATV",message,NULL);
    }
#endif
    /* Includes cancellation and errors after the graphical wizard. */
    SDL_Quit();
    return result;
}
