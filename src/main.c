#include "alignment.h"
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
#include "clipping.h"
#include <unistd.h>
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

typedef struct {
    uint32_t pixels[WIDTH*HEIGHT];
    uint32_t guide[WIDTH*HEIGHT];
    int guided, synced;
    int valid, phase, clear_pending;
    const char *reason;
    double period_x, period_y;
    uint64_t serial;
} StackFrame;

typedef struct {
    Config c;
    Decoder *decoder; /* Worker-owned, used in its callback. */
    FrameStack stack; /* UI statistics/settings only; worker owns the accumulator. */
    StackFrame queued;
    SDL_cond *stack_ready;
    int stack_pending, stack_busy;
    unsigned stack_epoch, stack_reset_id, stack_motion_id;
    uint64_t stack_skipped;
    int stacking, stack_paused, stack_hsync, sync_locked;
    unsigned stack_vsync;
    int stack_sync_input, stack_synced;
    uint32_t last_stacked;
    Source *source;
    SDL_mutex *mutex;
    SDL_atomic_t stop, failed, done, audio_muted, audio_locked, audio_source, audio_swap, nicam_detect, nicam_lock;
    SDL_atomic_t audio_dsp_queued, audio_dsp_overflows;
    SDL_atomic_t nicam_good_frames, nicam_bad_frames, nicam_muted_frames, nicam_sync_bits, nicam_queue_overflows;
    SDL_atomic_t playback_refills, playback_resets, playback_max_gap_us;
    SDL_AudioDeviceID audio_device;
    double requested_hz, requested_gain, actual_hz, actual_gain;
    int command, invert, free_sync, vertical_shift, fm_mode;
    int afc_enabled, afc_state; /* 0 seeking, 1 tracking, 2 outside capture range. */
    unsigned afc_epoch;
    double afc_anchor;
    int info_paused;
    double if_bandwidth, line_hz;
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
} App;

/* Only scalar diagnostics are copied; no image or IQ data enters the API. */
static double json_number(double x) { return isfinite(x)?x:0; }
static int status_json(void *opaque,char *out,size_t capacity)
{
    App *a=opaque;
#define B(x) ((x)?"true":"false")
#define N(x) json_number(x)
    SDL_LockMutex(a->mutex);
    char hackrf_gains[160]="";
    if(!strcmp(a->c.mode,"hackrf"))
        snprintf(hackrf_gains,sizeof(hackrf_gains),",\"gain_control\":\"lna\",\"rx_lna_db\":%.12g,\"rx_vga_db\":%.12g,\"rf_amp_db\":14",a->actual_gain,a->c.hackrf_vga);
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
        "{\"schema_version\":1,\"version\":\"%s\",\"build_utc\":\"%s\","
        "\"timestamp_unix_s\":%lld,\"metrics_available\":%s,\"metrics_age_ms\":%u,"
        "\"source\":\"%s\",\"done\":%s,\"failed\":%s,\"reception_paused\":%s,"
        "\"receiver\":{\"frequency_hz\":%.12g,\"sdr_frequency_hz\":%.12g,\"requested_frequency_hz\":%.12g,"
        "\"converter_lo_hz\":%.12g,\"sample_rate_sps\":%.12g,\"rf_bandwidth_hz\":%.12g,"
        "\"if_bandwidth_hz\":%.12g,\"alignment_bandwidth_hz\":%.12g,\"gain_db\":%.12g,"
        "\"fm_mode\":%d,\"deviation_hz\":%.12g,\"invert\":%s,\"pal_deemphasis\":%s,"
        "\"rx_block_samples\":%zu,\"channel\":%zu,\"experimental_sync\":%s,\"requested_gain_db\":%.12g%s},"
        "\"video\":{\"signal_dbfs\":%.12g,\"if_signal_dbfs\":%s,\"if_signal_age_ms\":%u,\"fields\":%u,\"discontinuities\":%u,"
        "\"sync_locked\":%s,\"color_enabled\":%s,\"color_locked\":%s,\"free_sync\":%s,\"line_hz\":%.12g,\"vertical_shift_lines\":%d},"
        "\"clipping\":{\"active\":%s,\"clipped_samples\":%llu,\"inspected_samples\":%llu,\"last_clip_age_ms\":%u},"
        "\"video_quality\":{\"valid\":%s,\"age_ms\":%u,\"video_snr_db\":%s,\"noise_rms\":%s,"
        "\"sync_depth\":%s,\"sync_depth_error\":%s,\"reason\":\"%s\","
        "\"lines\":%u,\"method\":\"pal_back_porch_sync_scaled_unweighted\"},"
        "\"afc\":{\"enabled\":%s,\"state\":%d,\"anchor_sdr_hz\":%.12g},"
        "\"stack\":{\"enabled\":%s,\"paused\":%s,\"rolling\":%s,\"synced\":%s,"
        "\"count\":%u,\"target\":%u,\"skipped\":%llu,\"confidence\":%.12g,\"dx\":%.12g,\"dy\":%.12g},"
        "\"audio\":{\"source\":%d,\"muted\":%s,\"locked\":%s,\"nicam_detected\":%s,\"nicam_locked\":%s,\"swap_lr\":%s,\"output_open\":%s,\"dsp_queued_samples\":%d,\"dsp_queue_overflows\":%d,"
        "\"nicam_good_frames\":%u,\"nicam_bad_frames\":%u,\"nicam_muted_frames\":%u,\"nicam_sync_bits_discarded\":%u,\"nicam_queue_overflows\":%u,"
        "\"playback_refills\":%u,\"playback_resets\":%u,\"playback_max_supply_gap_ms\":%.3f},"
        "\"rx_queue\":{\"queued_samples\":%zu,\"peak_samples\":%zu,\"capacity_samples\":%zu,"
        "\"discarded_samples\":%llu,\"hardware_overflows\":%llu}}\n",
        ATV_VERSION,ATV_BUILD_UTC,(long long)time(NULL),B(a->metrics_tick!=0),
        a->metrics_tick?SDL_GetTicks()-a->metrics_tick:0,a->c.mode,
        B(SDL_AtomicGet(&a->done)),B(SDL_AtomicGet(&a->failed)),B(a->info_paused),
        N(a->actual_hz+a->c.lo),N(a->actual_hz),N(a->requested_hz+a->c.lo),N(a->c.lo),N(a->c.rate),N(a->c.bandwidth),
        N(a->if_bandwidth),N(a->c.alignment_bandwidth>0?a->c.alignment_bandwidth:a->if_bandwidth),N(a->actual_gain),
        a->fm_mode,N(a->c.deviation),B(a->invert),B(a->c.video_deemphasis),
        a->c.rx_block,a->c.channel,B(a->c.experimental_sync),N(a->requested_gain),hackrf_gains,
        N(a->power),if_level,isfinite(a->if_power_dbfs)?SDL_GetTicks()-a->if_power_tick:0,a->frames,a->overflows,B(a->sync_locked),B(color_enabled(a->c.rate,a->if_bandwidth)),
        B(a->color_locked),B(a->free_sync),N(a->line_hz),a->vertical_shift,
        B(clipping_active(&a->clipping,SDL_GetTicks())),
        (unsigned long long)a->clipping.clipped_samples,(unsigned long long)a->clipping.samples,
        a->clipping.clipped_samples?SDL_GetTicks()-a->clipping.last_clip_tick:0,
        B(quality_valid),a->quality_tick?SDL_GetTicks()-a->quality_tick:0,snr,noise,depth,uncertainty,quality_reason,
        a->quality.lines,B(a->afc_enabled),a->afc_state,N(a->afc_anchor),
        B(a->stacking),B(a->stack_paused),B(a->stack.rolling),B(a->stack_synced),a->stack.count,a->stack.target,
        (unsigned long long)a->stack_skipped,N(a->stack.confidence),N(a->stack.dx),N(a->stack.dy),
        SDL_AtomicGet(&a->audio_source),B(SDL_AtomicGet(&a->audio_muted)),B(SDL_AtomicGet(&a->audio_locked)),
        B(SDL_AtomicGet(&a->nicam_detect)),B(SDL_AtomicGet(&a->nicam_lock)),
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

typedef struct {
    int active;
    char value[32];
    const char *error;
} FrequencyInput;

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
    uint32_t pixels[WIDTH*HEIGHT], live[WIDTH*HEIGHT], last_frame, last_stacked;
    float spectrum[FFT_SIZE], power;
    double actual_hz, actual_gain, if_bandwidth, line_hz, if_power_dbfs;
    unsigned frames, overflows;
    Clipping clipping;
    int color_locked;
    uint64_t stack_skipped;
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
    memcpy(v->pixels,a->stacking && !a->stack.count?a->live:a->pixels,sizeof(v->pixels));
    if(a->stacking) memcpy(v->live,a->live,sizeof(v->live));
    memcpy(v->spectrum,a->spectrum,sizeof(v->spectrum));
#define COPY_VIEW(name) v->name=a->name
    COPY_VIEW(quality); COPY_VIEW(quality_tick); COPY_VIEW(clipping);
    COPY_VIEW(last_frame); COPY_VIEW(last_stacked); COPY_VIEW(power); COPY_VIEW(if_power_dbfs);
    COPY_VIEW(actual_hz); COPY_VIEW(actual_gain); COPY_VIEW(if_bandwidth); COPY_VIEW(line_hz);
    COPY_VIEW(frames); COPY_VIEW(overflows); COPY_VIEW(stack_skipped);
    COPY_VIEW(stacking); COPY_VIEW(stack_paused); COPY_VIEW(stack_synced);
    COPY_VIEW(afc_enabled); COPY_VIEW(afc_state); COPY_VIEW(afc_anchor);
    COPY_VIEW(fm_mode); COPY_VIEW(color_locked); COPY_VIEW(sync_locked); COPY_VIEW(free_sync);
    COPY_VIEW(stack.count); COPY_VIEW(stack.target); COPY_VIEW(stack.rolling);
    COPY_VIEW(stack.confidence); COPY_VIEW(stack.dx); COPY_VIEW(stack.dy);
    COPY_VIEW(stack.shear); COPY_VIEW(stack.status);
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
    ++a->stack_epoch; ++a->stack_reset_id;
    SDL_CondBroadcast(a->stack_ready);
}
static void interrupt_stack(App *a)
{
    ++a->stack_epoch; ++a->stack_motion_id;
    a->stack_pending=0;
    SDL_CondBroadcast(a->stack_ready);
}
static int stack_worker(void *opaque)
{
    App *a=opaque;
    FrameStack *s=calloc(1,sizeof(*s));
    StackFrame *input=malloc(sizeof(*input));
    uint32_t *output=calloc(WIDTH*HEIGHT,sizeof(*output));
    unsigned reset=0,motion=0;
    if(!s || !input || !output) { SDL_AtomicSet(&a->failed,1); goto cleanup; }
    for(;;) {
        SDL_LockMutex(a->mutex);
        while(!a->stack_pending && !SDL_AtomicGet(&a->stop) && !SDL_AtomicGet(&a->done))
            SDL_CondWait(a->stack_ready,a->mutex);
        if(!a->stack_pending) { SDL_UnlockMutex(a->mutex); break; }
        *input=a->queued;
        a->stack_pending=0; a->stack_busy=1;
        unsigned epoch=a->stack_epoch, next_reset=a->stack_reset_id, next_motion=a->stack_motion_id;
        unsigned target=a->stack.target;
        int rolling=a->stack.rolling;
        SDL_UnlockMutex(a->mutex);

        /* Neither reception nor rendering holds this accumulator's lock. */
        int configured=1;
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
        int added=configured && (input->synced ?
            stack_add_synced(s,input->pixels,input->guided?input->guide:input->pixels,input->valid,input->phase,output) : input->guided ?
            stack_add_guided(s,input->pixels,input->guide,input->valid,input->phase,output) :
            stack_add(s,input->pixels,input->valid,input->phase,output));

        SDL_LockMutex(a->mutex);
        if(epoch==a->stack_epoch && a->stacking && !a->stack_paused && !a->command) {
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
    free(s); free(input); free(output);
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
    if(!a->stacking) memcpy(a->pixels,pixels,sizeof(a->pixels));
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
    a->sync_locked=a->decoder->hsync_updates && a->decoder->vsync_updates &&
        a->decoder->fields-a->decoder->hsync_field<=5 &&
        a->decoder->tick-a->decoder->vsync_confirmed<a->decoder->rate*.1;
    int synced=a->decoder->hsync_updates && a->decoder->vsync_updates &&
        a->decoder->fields-a->decoder->hsync_field<=2 &&
        a->decoder->tick-a->decoder->vsync_confirmed<a->decoder->rate*.060;
    if(!a->stack_paused && a->decoder->free_sync &&
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
    if (!a->stacking) memcpy(a->pixels, a->live, sizeof(a->pixels));
    else if(!a->command && !a->stack_paused && !SDL_AtomicGet(&a->failed)) {
        if(a->stack_pending) ++a->stack_skipped;
        memcpy(a->queued.pixels,pixels,sizeof(a->queued.pixels));
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
    a->last_frame = SDL_GetTicks(); ++a->frames;
    SDL_UnlockMutex(a->mutex);
}
static void init_decoders(App *a, Decoder *d, Decoder *preview,
                          int invert, int free_sync, double line_hz, int vertical_shift, double if_bw)
{
    decoder_init(d,a->c.rate,a->c.deviation,invert,frame,a);
    /* Both display modes use the adaptive sync clock, which keeps producing
     * rasters while searching/coasting. F only selects the independent live
     * preview versus the sync-following raster, not the legacy pulse gate. */
    d->experimental_sync=a->c.experimental_sync;
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
    d->video_deemphasis=a->c.video_deemphasis;
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
    if(p->source==0) sound_process(p->sound,samples,n);
    if(p->source>=2) stereo_sound_process(p->stereo,samples,n);
    SDL_AtomicSet(&p->app->audio_locked,p->source>=2?stereo_sound_locked(p->stereo):p->source==0 && sound_locked(p->sound));
    if(p->nicam && p->source<2) {
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
}
/* Called only with the audio worker idle, or by that worker after a gap. */
static void reset_audio_state(Playback *p)
{
    stereo_sound_reset(p->stereo,p->source==3,p->swap);
    if(p->nicam) { nicam_select(p->nicam,0); nicam_reset(p->nicam); }
    SDL_AtomicSet(&p->app->nicam_detect,0); SDL_AtomicSet(&p->app->nicam_lock,0);
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
    alignment_set_consumer(video,p->thread && p->sound && (p->source>=2?stereo_sound_enabled(p->app->c.rate,bandwidth):sound_enabled(p->app->c.rate,bandwidth))?enqueue_audio:NULL,p);
}

/* Drain a pending block before a discontinuity, control change or EOF. */
static void finish_video(Decoder *d, Alignment *video, Alignment *guide, size_t *pending)
{
    if(!*pending) return;
    const float *v=alignment_wait(video);
    const float *g=guide?alignment_wait(guide):NULL;
    decoder_process_video(d,v,g,*pending);
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
    IQ *iq = malloc(a->c.rx_block * sizeof(*iq));
    Decoder *d = malloc(sizeof(*d));
    Decoder *preview = malloc(sizeof(*preview));
    fftwf_complex *in = fftwf_alloc_complex(FFT_SIZE), *out = fftwf_alloc_complex(FFT_SIZE);
    fftwf_plan plan = NULL;
    Alignment *video_path=NULL;
    size_t pending=0;
    Alignment *alignment=NULL;
    float *video=malloc(a->c.rx_block*sizeof(*video));
    float *guide=malloc(a->c.rx_block*sizeof(*guide));
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
    if (!iq || !d || !preview || !in || !out || !video || !guide) goto fail;
    plan = fftwf_plan_dft_1d(FFT_SIZE, in, out, FFTW_FORWARD, FFTW_ESTIMATE);
    if (!plan) goto fail;
    SDL_LockMutex(a->mutex);
    int initial_vertical_shift=a->vertical_shift;
    SDL_UnlockMutex(a->mutex);
    init_decoders(a,d,preview,a->c.invert,a->c.free_sync,a->c.line_hz,initial_vertical_shift,a->c.if_bandwidth);
    a->decoder=d;
    video_path=alignment_create_video(a->c.rate,a->c.if_bandwidth,a->c.deviation,a->c.invert,a->c.rx_block,d->color);
    if(video_path && a->c.video_deemphasis && alignment_set_deemphasis(video_path)) {
        alignment_destroy(video_path); video_path=NULL;
    }
    if(!video_path) goto fail;
    if(a->c.rate>=14e6) {
        audio.sound=sound_create(a->c.rate,play_audio,&audio);
        if(a->c.rate>=16e6) audio.stereo=stereo_sound_create(a->c.rate,play_stereo,&audio);
        audio.nicam=nicam_create(a->c.rate,play_stereo,&audio);
        if(!audio.sound || !audio.nicam || (a->c.rate>=16e6 && !audio.stereo)) goto fail;
        if(start_audio_worker(&audio,a->c.rx_block)) goto fail;
        reset_audio(&audio,video_path,a->c.if_bandwidth);
        fprintf(stderr,"Audio: 6 MHz mono / NICAM (rate and IF >= 14 MHz), 7.02/7.22 or 7.38/7.56 MHz stereo (>= 16 MHz); A mute, N source, Shift+N swap L/R\n");
    }
    if(a->c.experimental_sync) fprintf(stderr,"Experimental sync: echo-resistant line timing and strongest field group enabled\n");
    if(a->c.video_deemphasis) fprintf(stderr,"Video de-emphasis: PAL ITU-R F.405-1, linear phase; raw FM/NICAM audio preserved\n");
    fprintf(stderr,"Video IF/FM: pipelined thread; PAL color automatic for IF >= 10 MHz at >= 12 MS/s\n");
    if(a->c.alignment_bandwidth>0) {
        alignment=alignment_create(a->c.rate,a->c.alignment_bandwidth,a->c.deviation,a->c.invert,a->c.rx_block);
        if(!alignment) goto fail;
        fprintf(stderr,"Alignment IF/FM: dedicated thread, sample-aligned blocks\n");
    }
    alignment_set_fm_mode(video_path,a->c.fm_mode,a->c.invert);
    if(alignment) alignment_set_fm_mode(alignment,a->c.fm_mode,a->c.invert);
    fprintf(stderr,"FM%d detector: video and alignment; D switches FM0/FM1\n",a->c.fm_mode);
    AFC afc; afc_reset(&afc,a->c.rate,a->c.line_hz);
    unsigned afc_epoch=0;
    double applied_if = a->c.if_bandwidth;
    uint64_t count = 0, next_fft = 0;
    uint32_t start = SDL_GetTicks();
    Clipping clipping; clipping_init(&clipping,a->c.mode);
    while (!SDL_AtomicGet(&a->stop)) {
        SDL_LockMutex(a->mutex);
        if(a->info_paused) {
            uint32_t pause_start=SDL_GetTicks();
            double pause_if=a->if_bandwidth;
            SDL_UnlockMutex(a->mutex);
            finish_video(d,video_path,alignment,&pending);
            source_pause(a->source);
            reset_audio(&audio,video_path,pause_if);
            SDL_LockMutex(a->mutex);
            while(a->info_paused && !SDL_AtomicGet(&a->stop))
                SDL_CondWait(a->stack_ready,a->mutex);
            start+=SDL_GetTicks()-pause_start;
            if(strcmp(a->c.mode,"demo") && strcmp(a->c.mode,"file")) {
                /* A live source resumes at the current signal, after a gap. */
                a->command|=2;
                reset_stack(a,a->stack.target,a->stack.rolling);
                if(recorded && record_count<record_capacity) record_count=0;
            }
        }
        if(SDL_AtomicGet(&a->stop)) { SDL_UnlockMutex(a->mutex); break; }
        int command = a->command, invert = a->invert, free_sync=a->free_sync, vertical_shift=a->vertical_shift;
        int afc_enabled=a->afc_enabled, fm_mode=a->fm_mode;
        unsigned epoch=a->afc_epoch;
        double if_bw=a->if_bandwidth, line_hz=a->line_hz;
        double hz = a->requested_hz, gain = a->requested_gain;
        a->command = 0;
        SDL_UnlockMutex(a->mutex);
        if(epoch!=afc_epoch) { afc_reset(&afc,a->c.rate,line_hz); afc_epoch=epoch; }
        if(command & 4) {
            finish_video(d,video_path,alignment,&pending);
            reset_audio(&audio,video_path,if_bw);
            command&=~4;
        }
        if (command) {
            finish_video(d,video_path,alignment,&pending);
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
            init_decoders(a,d,preview,invert,free_sync,line_hz,vertical_shift,if_bw);
            if(alignment) alignment_reset(alignment,invert);
            if (if_bw != applied_if) {
                alignment_destroy(video_path);
                video_path=alignment_create_video(a->c.rate,if_bw,a->c.deviation,invert,a->c.rx_block,d->color);
                if(video_path && a->c.video_deemphasis && alignment_set_deemphasis(video_path)) {
                    alignment_destroy(video_path); video_path=NULL;
                }
                if(!video_path) goto fail;
                applied_if=if_bw;
            } else alignment_reset(video_path,invert);
            alignment_set_fm_mode(video_path,fm_mode,invert);
            if(alignment) alignment_set_fm_mode(alignment,fm_mode,invert);
            reset_audio(&audio,video_path,if_bw);
        }
        int n = source_read(a->source, iq, a->c.rx_block);
        if(n<=0) finish_video(d,video_path,alignment,&pending);
        if (n == -2 || n == -3) {
            video_quality_reset(&quality); publish_video_quality(a,&quality);
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
            init_decoders(a,d,preview,invert,free_sync,line_hz,vertical_shift,if_bw);
            afc_reset(&afc,a->c.rate,line_hz);
            alignment_reset(video_path,invert);
            reset_audio(&audio,video_path,if_bw);
            if(alignment) alignment_reset(alignment,invert);
            if(n==-3) { count=0; next_fft=0; start=SDL_GetTicks(); }
            continue;
        }
        if (n < 0) goto fail;
        if (!n) {
            if (!strcmp(a->c.mode, "file")) { audio_idle(&audio,0); nicam_finish(audio.nicam); SDL_AtomicSet(&a->nicam_lock,nicam_locked(audio.nicam)); break; }
            continue;
        }
        clipping_process(&clipping,iq,(size_t)n,SDL_GetTicks());
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
            double power = 0;
            for (int j = 0; j < n; ++j) power += iq[j].i*iq[j].i + iq[j].q*iq[j].q;
            for (int j = 0; j < FFT_SIZE; ++j) {
                float w = .5f - .5f*cosf(2*PI*j/(FFT_SIZE-1));
                in[j][0] = iq[j].i*w; in[j][1] = iq[j].q*w;
            }
            fftwf_execute(plan);
            SourceStats metrics;
            source_get_stats(a->source,&metrics);
            SDL_LockMutex(a->mutex);
            a->rx_stats=metrics; a->metrics_tick=SDL_GetTicks();
            a->clipping=clipping;
            for (int j = 0; j < FFT_SIZE; ++j) {
                int k = (j + FFT_SIZE/2) % FFT_SIZE;
                float p = (out[k][0]*out[k][0]+out[k][1]*out[k][1]) / (FFT_SIZE*FFT_SIZE*.25f);
                a->spectrum[j] = 10*log10f(p + 1e-16f);
            }
            a->power = 10*log10(power/n + 1e-16);
            SDL_UnlockMutex(a->mutex);
            next_fft = count + (uint64_t)(a->c.rate / 25);
        }
        /* Copy completed block N before workers reuse their buffers for N+1.
         * Colour/raster N then overlaps both IF/FM branches of N+1. */
        size_t ready=pending;
        if(ready) {
            memcpy(video,alignment_wait(video_path),ready*sizeof(*video));
            uint32_t measured_at;
            double if_power=alignment_if_power(video_path,&measured_at);
            SDL_LockMutex(a->mutex);
            a->if_power_dbfs=if_power; a->if_power_tick=measured_at;
            SDL_UnlockMutex(a->mutex);
            if(alignment) memcpy(guide,alignment_wait(alignment),ready*sizeof(*guide));
        }
        if(alignment_submit(video_path,iq,(size_t)n)) goto fail;
        if(alignment && alignment_submit(alignment,iq,(size_t)n)) goto fail;
        pending=(size_t)n;
        if(ready) {
            uint64_t first_tick=d->tick;
            decoder_process_video(d,video,alignment?guide:NULL,ready);
            video_quality_process(&quality,video,ready,first_tick,quality_sync_valid(d),
                d->free_origin,d->free_period>0?d->free_period:d->rate/d->line_hz,
                (double)d->field,d->vsync_period>0?d->vsync_period:d->rate/50);
            publish_video_quality(a,&quality);
        }
        if(ready && afc_enabled) {
            double error=0;
            /* PAL de-emphasis amplifies DC (and tuning error) by 11 dB. */
            double hz_per_unit=a->c.deviation/(a->c.video_deemphasis?pow(10,11.0/20):1);
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
    alignment_destroy(alignment); alignment_destroy(video_path); free(video); free(guide);
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
    text(r,40,70,"VERSION " ATV_VERSION "   " ATV_BUILD_UTC " UTC",2);
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
    } else wizard_lines(r,40,178,q->details,14);
    SDL_SetRenderDrawColor(r,160,205,235,255);
    text(r,40,500,"CURRENT VALUE:",2);
    wizard_lines(r,40,525,*q->current?q->current:"-",2);
    SDL_Rect box={40,580,880,44};
    SDL_SetRenderDrawColor(r,35,49,65,255); SDL_RenderFillRect(r,&box);
    SDL_SetRenderDrawColor(r,235,240,245,255);
    char visible[108]; size_t len=strlen(input);
    snprintf(visible,sizeof(visible),"%s_",input+(len>104?len-104:0));
    text(r,50,595,visible,2);
    SDL_SetRenderDrawColor(r,255,170,110,255);
    wizard_lines(r,40,654,*error?error:q->error,2);
    SDL_Rect next={40,730,320,46}, cancel={380,730,320,46};
    SDL_SetRenderDrawColor(r,45,65,80,255); SDL_RenderFillRect(r,&next); SDL_RenderFillRect(r,&cancel);
    SDL_SetRenderDrawColor(r,235,240,245,255);
    text(r,55,745,(q->kind==SETTINGS_WELCOME)?"SET UP":"CONTINUE - ENTER",2);
    text(r,395,745,(q->kind==SETTINGS_WELCOME)?"SKIP - ENTER":"EXIT - ESC",2);
    SDL_SetRenderDrawColor(r,140,155,170,255);
    text(r,40,805,"BACKSPACE: DELETE CHARACTER   CTRL+A: CLEAR FIELD   CTRL+V: PASTE",2);
    text(r,40,837,"AFTER SETUP: PRESS W ON THE MAIN SCREEN TO SAVE ALL SETTINGS",2);
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
                if(strlen(line)+strlen(e.text.text)<n) { strcat(line,e.text.text); error=""; }
                else error="INPUT TOO LONG. USE BACKSPACE OR CTRL+A.";
            }
            if(e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT && e.button.y>=730 && e.button.y<776) {
                if(e.button.x>=40 && e.button.x<360) {
                    if((q->kind==SETTINGS_WELCOME)) snprintf(line,n,"w");
                    SDL_StopTextInput(); return 1;
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

static int graphical_wizard(Settings *settings)
{
    if(init_ui()) { fprintf(stderr,"SDL: %s\n",SDL_GetError()); return -1; }
    SDL_Window *window=SDL_CreateWindow("FM ATV - Startup Configuration",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,960,880,SDL_WINDOW_RESIZABLE);
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
static void draw_info_screen(SDL_Renderer *r, const Config *c)
{
    static const struct { const char *key, *description; } commands[]={
        {"LEFT / RIGHT", "TUNE IN 100 KHZ STEPS"},
        {"SHIFT / CTRL", "WHEN TUNING: 1 MHZ / 10 KHZ PER STEP"},
        {"UP / DOWN", "INCREASE / DECREASE RECEIVE GAIN BY 1 DB"},
        {"T", "ENTER FREQUENCY IN MHZ, INCLUDING CONVERTER LO"},
        {"ENTER / ESC", "DURING FREQUENCY ENTRY: TUNE / CANCEL"},
        {"BACKSPACE / CTRL+A", "DURING FREQUENCY ENTRY: DELETE CHARACTER / CLEAR FIELD"},
        {"W", "SAVE CURRENT SETTINGS AS STARTUP CONFIGURATION"},
        {"C", "AFC ON / OFF: TRACK SIGNAL WITHIN +/-500 KHZ (LIVE SDR)"},
        {"I", "INVERT FM VIDEO POLARITY"},
        {"D", "SWITCH FM0 / FM1 FOR VIDEO AND ALIGNMENT"},
        {"F", "FREE-RUNNING VIDEO SYNC ON / OFF"},
        {"[ / ]", "DECREASE / INCREASE VIDEO IF BANDWIDTH BY 1 MHZ"},
        {", / .", "DECREASE / INCREASE LINE FREQUENCY BY 0.1 HZ"},
        {"B / V", "SHIFT VERTICAL IMAGE POSITION BY ONE STEP"},
        {"S", "FRAME STACKING ON / OFF"},
        {"- / +", "DECREASE / INCREASE NUMBER OF FIELDS TO STACK"},
        {"R", "RESTART STACK"},
        {"M", "SWITCH FIXED / ROLLING STACKING"},
        {"SHIFT+P", "PAUSE / RESUME STACKING"},
        {"P", "SAVE SCREENSHOT AS PNG"},
        {"A", "MUTE / UNMUTE AUDIO"},
        {"N", "AUDIO SOURCE: FM 6 / NICAM / 7.02+7.22 / 7.38+7.56 MHZ"},
        {"SHIFT+N", "SWAP LEFT AND RIGHT AUDIO CHANNELS"},
        {"SHIFT+1 / !", "OPEN THIS INFO SCREEN"},
        {"ESC", "QUIT APPLICATION FROM THE MAIN SCREEN"},
        {"MOUSE", "WHEEL: TUNE. CLICK SPECTRUM: SELECT FREQUENCY"}
    };
    SDL_SetRenderDrawColor(r,14,19,27,255); SDL_RenderClear(r);
    SDL_SetRenderDrawColor(r,90,210,180,255);
    text(r,40,28,"FM ATV - INFO AND CONTROLS",4);
    SDL_SetRenderDrawColor(r,225,235,245,255);
    text(r,40,60,"VERSION " ATV_VERSION "   BUILT " ATV_BUILD_UTC " UTC",2);
    text(r,40,78,"PURPOSE: RECEIVE ANALOG FM AMATEUR TELEVISION WITH AN SDR.",2);
    SDL_SetRenderDrawColor(r,160,205,235,255);
    text(r,40,104,"ATV CONTESTS: WEAK-SIGNAL FRAME STACKING AT <= 10 MS/S (LIVE SDR).",2);
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
        snprintf(gain_info,sizeof(gain_info),"HACKRF: RX VGA %.0f DB FIXED; RF AMP ON (14 DB). UP/DOWN: RX LNA.",c->hackrf_vga);
        text(r,40,784,gain_info,2);
    }
    text(r,40,804,"FILE / DEMO: FRAME STACKING IS ALSO AVAILABLE ABOVE 10 MS/S.",2);
    SDL_SetRenderDrawColor(r,90,210,180,255);
    text(r,40,842,"PRESS ANY KEY TO RETURN",3);
}

/* Capture the completed backbuffer before SDL_RenderPresent. RGBA32 is
 * byte-ordered RGBA on either endian, as required by libpng's simple API. */
static int save_window_png(SDL_Renderer *renderer,double frequency,char *path,size_t capacity)
{
    struct timespec now; struct tm utc; char stamp[32]; int w,h;
    if(clock_gettime(CLOCK_REALTIME,&now) || !gmtime_r(&now.tv_sec,&utc) ||
       !strftime(stamp,sizeof(stamp),"%Y%m%dT%H%M%S",&utc)) {
        fprintf(stderr,"Screenshot: cannot determine UTC time\n"); return -1;
    }
    if(SDL_GetRendererOutputSize(renderer,&w,&h)) {
        fprintf(stderr,"Screenshot: %s\n",SDL_GetError()); return -1;
    }
    SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormat(0,w,h,32,SDL_PIXELFORMAT_RGBA32);
    if(!surface || SDL_RenderReadPixels(renderer,NULL,SDL_PIXELFORMAT_RGBA32,surface->pixels,surface->pitch)) {
        fprintf(stderr,"Screenshot: %s\n",SDL_GetError()); SDL_FreeSurface(surface); return -1;
    }
    FILE *file=NULL;
    for(unsigned suffix=0;suffix<1000;++suffix) {
        int length=snprintf(path,capacity,"fm-atv-%.0fHz-%s-%03ldZ-%03u.png",
            frequency,stamp,now.tv_nsec/1000000,suffix);
        if(length<0 || (size_t)length>=capacity) break;
        file=fopen(path,"wbx");
        if(file || errno!=EEXIST) break;
    }
    if(!file) {
        fprintf(stderr,"Screenshot: cannot create file: %s\n",strerror(errno));
        SDL_FreeSurface(surface); return -1;
    }
    png_image png={0}; png.version=PNG_IMAGE_VERSION;
    png.width=w; png.height=h; png.format=PNG_FORMAT_RGBA;
    int ok=png_image_write_to_stdio(&png,file,0,surface->pixels,surface->pitch,NULL);
    if(!ok) fprintf(stderr,"Screenshot: %s\n",png.message);
    if(fclose(file)) { fprintf(stderr,"Screenshot: %s\n",strerror(errno)); ok=0; }
    png_image_free(&png); SDL_FreeSurface(surface);
    if(!ok) { remove(path); return -1; }
    fprintf(stderr,"Screenshot saved: %s\n",path);
    return 0;
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
         "  --hackrf-vga DB        fixed RX VGA, default 28 (0..62, step 2); RF AMP on\n"
         "  --deviation HZ         Test/file video scale in Hz (default 4000000)\n"
         "  --video-deemphasis MODE PAL F.405-1 linear-phase correction: pal or off (default pal)\n"
         "  --if-bandwidth HZ      digital IF width (default 18000000); auto PAL color >= 10 MHz at >= 12 MS/s\n"
         "  --alignment-bandwidth HZ separate IF for sync/alignment (default 3200000; 0 = video IF)\n"
         "  --stack-frames N       enable aligned stack of 1..512 fields (live SDR: rate <= 10 MS/s)\n"
         "  --stack-rolling        continuously replace oldest accepted field\n"
         "  --free-sync            free-running live view; stack still follows sync\n"
         "  --experimental-sync    experimental line and field sync together (default off)\n"
         "  --line-hz HZ           free-running line clock (default 15625)\n"
         "  --status-server        enable read-only HTTP JSON debug status (default off)\n"
         "  --no-status-server     disable status API, including a saved setting\n"
         "  --status-bind IPV4     local bind address (default 127.0.0.1; LAN: 0.0.0.0)\n"
         "  --status-port PORT     TCP port (default 8765); GET /status\n"
         "  --fm-mode 0|1          FM detector for video and alignment (default 0); D toggles\n"
         "  --invert               reverse FM video polarity\n"
         "  --snapshot PATH        save UI screenshot as BMP on exit\n"
         "  --record PATH          capture raw IQ in RAM, save native cf32 on exit (no overwrite)\n"
         "  --record-seconds N     capture duration, default 3; maximum 15 s / 2 GiB\n"
         "  --seconds N            stop after N seconds (0 = unlimited)\n"
         "  --nicam                start with NICAM 6.552 MHz audio selected (N cycles audio sources)\n"
         "  Audio: A mute; N select FM 6 MHz / NICAM / FM 7.02+7.22 / FM 7.38+7.56 MHz; Shift+N swap L/R.\n"
         "Shift+1 (!) = application information and all controls; any key returns.\n"
         "Controls: left/right or wheel = 100 kHz, Shift = 1 MHz, Ctrl = 10 kHz;\n"
         "C = AFC on/off (+/-500 kHz, live SDR); T = type frequency in MHz, Enter = tune, Esc = cancel; up/down = gain 1 dB; I = invert; click spectrum = tune; Esc = quit.\n"
         "[ / ] = IF narrower/wider by 1 MHz; F = free sync; , / . = line clock -/+ 0.1 Hz; B/V = vertical position.\n"
         "S = stacking on/off; -/+ = stack size; R = reset stack; M = fixed/rolling; P = PNG screenshot; Shift+P = pause/resume stacking (also clickable buttons).");
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
    s->c.invert=a->invert; s->c.free_sync=a->free_sync;
    s->c.if_bandwidth=a->if_bandwidth; s->c.line_hz=a->line_hz;
    s->stacking=a->stacking; s->stack_frames=(int)a->stack.target;
    s->rolling=a->stack.rolling; s->paused=a->stack_paused;
    s->vertical_shift=a->vertical_shift;
    s->audio_source=SDL_AtomicGet(&a->audio_source);
    s->audio_muted=SDL_AtomicGet(&a->audio_muted);
    s->audio_swap=SDL_AtomicGet(&a->audio_swap);
}
static int atv_main(int argc, char **argv)
{
    App *a = calloc(1, sizeof(*a));
    if (!a) return 1;
    a->if_power_dbfs=NAN;
    a->c = (Config){.mode="demo", .args="", .video_deemphasis=1, .images=ATV_DEFAULT_IMAGES,
        .status_bind="127.0.0.1", .status_port=8765,
        .antenna="TX/RX", .ref="internal", .rate=20e6, .frequency=1252e6, .bandwidth=18e6, .gain=30, .hackrf_vga=28, .deviation=4e6, .if_bandwidth=18e6, .line_hz=15625, .rx_block=0, .record_seconds=3, .alignment_bandwidth=3200000};
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
        if(!strcmp(argv[j],"--version")) { printf("FM ATV %s (built %s UTC)\n",ATV_VERSION,ATV_BUILD_UTC); free(a); return 0; }
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
    a->c=settings.c;
    int rolling=settings.rolling, explicit_nicam=0;
    a->stacking=settings.stacking; a->stack_paused=settings.paused;
    a->vertical_shift=settings.vertical_shift;
    SDL_AtomicSet(&a->audio_source,settings.audio_source);
    SDL_AtomicSet(&a->audio_muted,settings.audio_muted);
    SDL_AtomicSet(&a->audio_swap,settings.audio_swap);
    stack_reset(&a->stack, (unsigned)settings.stack_frames);
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
        if (!strcmp(o,"--stack-rolling")) { rolling=1; a->stacking=1; continue; }
        if (!strcmp(o,"--loop")) { a->c.loop=1; continue; }
        if (!strcmp(o,"--experimental-sync") || !strcmp(o,"--experimental-vsync") ||
            !strcmp(o,"--experimental-echo-sync")) { a->c.experimental_sync=1; continue; }
        if (!strcmp(o,"--free-sync")) { a->c.free_sync=1; continue; }
        if (!strcmp(o,"--nicam")) { SDL_AtomicSet(&a->audio_source,1); explicit_nicam=1; continue; }
        if (!strcmp(o,"--invert")) { a->c.invert=1; continue; }
        if (++j >= argc) { fprintf(stderr,"Missing value for %s\n",o); free(a); return 2; }
        const char *v = argv[j];
        if (!strcmp(o,"--args")) a->c.args=v;
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
            if(strcmp(v,"0") && strcmp(v,"1")) { fprintf(stderr,"--fm-mode requires 0 or 1\n"); free(a); return 2; }
            a->c.fm_mode=atoi(v);
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
            stack_reset(&a->stack, (unsigned)n); a->stacking = 1;
        }
        else if (!strcmp(o,"--channel")) {
            double ch = number(v); if (ch < 0 || ch > 1 || floor(ch) != ch) { free(a); return 2; }
            a->c.channel=(size_t)ch;
        } else { fprintf(stderr,"Unknown option: %s\n",o); free(a); return 2; }
    }
    if(!a->c.rx_block) a->c.rx_block=settings_default_rx_block(a->c.mode);
    settings.c=a->c;
    if(wizard) {
        int setup=wizard==2?settings_wizard(&settings,stdin,stdout):graphical_wizard(&settings);
        if(setup<=0) { free(a); return setup<0?1:0; }
    }
    a->c=settings.c;
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
        a->stacking=0;
        fprintf(stderr,"Frame stacking disabled above 10 MS/s for live SDR (actual rate %.3f MS/s)\n",a->c.rate/1e6);
    }
    if (init_ui()) { fprintf(stderr,"SDL: %s\n",SDL_GetError()); source_close(a->source); free(a); return 1; }
    if(a->c.rate>=14e6) {
        a->audio_device=open_audio_output(a->c.audio_output);
    }
    a->mutex = SDL_CreateMutex();
    SDL_Window *window = SDL_CreateWindow("FM ATV - PAL", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 880, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *r = window ? SDL_CreateRenderer(window, -1, 0) : NULL;
    SDL_Texture *texture = r ? SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, WIDTH, HEIGHT) : NULL;
    SDL_Thread *thread = NULL, *stack_thread = NULL;
    View *view=malloc(sizeof(*view));
    a->stack_ready=SDL_CreateCond();
    StatusServer *status_server=NULL;
    int result = 1;
    if (!a->mutex || !a->stack_ready || !window || !r || !texture || !view || !ui_text_init(r)) goto cleanup;
    SDL_RenderSetLogicalSize(r,960,880);
    a->afc_anchor=a->c.frequency;
    a->requested_hz=a->actual_hz=a->c.frequency; a->requested_gain=a->actual_gain=a->c.gain; a->invert=a->c.invert;
    a->fm_mode=a->c.fm_mode;
    a->if_bandwidth=a->c.if_bandwidth; a->line_hz=a->c.line_hz; a->free_sync=a->c.free_sync;
    for (int k=0;k<FFT_SIZE;++k) a->spectrum[k]=-120;
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
    int screenshot_requested=0, save_result=0;
    uint32_t save_notice=0;
    FrequencyInput frequency_input={0};
    InfoScreen info_screen={0};
    uint32_t info_opened=0;
    SDL_StopTextInput();
    /* Keep a completed file visible for inspection; errors still exit. */
    while (!SDL_AtomicGet(&a->stop) && !SDL_AtomicGet(&a->failed) &&
           (!SDL_AtomicGet(&a->done) || !strcmp(a->c.mode,"file"))) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            int info_was_active=info_screen.active;
            if(!frequency_input.active && info_screen_event(&info_screen,&e)) {
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
            if(frequency_input_event(&frequency_input,a,&e)) continue;
            double delta=0, gain_delta=0;
            double step = SDL_GetModState() & KMOD_CTRL ? 1e4 : SDL_GetModState() & KMOD_SHIFT ? 1e6 : 1e5;
            if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) SDL_AtomicSet(&a->stop,1);
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
            if((e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_c && !e.key.repeat) ||
               (e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT &&
                e.button.x>=750 && e.button.x<=940 && e.button.y>=27 && e.button.y<=56)) afc_toggle(a);
            if (e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_i) { a->invert=!a->invert; a->command|=2; }
            if(e.type==SDL_KEYDOWN) {
                SDL_Keycode key=e.key.keysym.sym;
                if(key==SDLK_n && !e.key.repeat && sound_enabled(a->c.rate,a->if_bandwidth)) {
                    if(e.key.keysym.mod & KMOD_SHIFT)
                        SDL_AtomicSet(&a->audio_swap,!SDL_AtomicGet(&a->audio_swap));
                    else {
                        int next=(SDL_AtomicGet(&a->audio_source)+1)%4;
                        if(next>=2 && !stereo_sound_enabled(a->c.rate,a->if_bandwidth)) next=0;
                        SDL_AtomicSet(&a->audio_source,next);
                    }
                    a->command|=4;
                }
                if(key==SDLK_a && !e.key.repeat) SDL_AtomicSet(&a->audio_muted,!SDL_AtomicGet(&a->audio_muted));
                if(key==SDLK_d && !e.key.repeat) { a->fm_mode^=1; a->command|=2; }
                if(key==SDLK_f) { a->free_sync=!a->free_sync; a->command|=2; }
                if(key==SDLK_LEFTBRACKET || key==SDLK_RIGHTBRACKET) {
                    a->if_bandwidth=fmin(a->c.rate,fmax(250000,a->if_bandwidth+(key==SDLK_LEFTBRACKET?-1e6:1e6))); a->command|=2;
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
                if (key == SDLK_s) action = 1;
                if (key == SDLK_MINUS || key == SDLK_KP_MINUS) action = 2;
                if (key == SDLK_EQUALS || key == SDLK_PLUS || key == SDLK_KP_PLUS) action = 3;
                if (key == SDLK_r) action = 4;
                if (key == SDLK_p) {
                    if(e.key.keysym.mod & KMOD_SHIFT) action=5;
                    else screenshot_requested=1;
                }
                if (key == SDLK_m) action = 6;
            }
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT &&
                e.button.y >= 830 && e.button.y < 860) {
                int x = e.button.x;
                if (x >= 20 && x < 160) action = 1;
                if (x >= 170 && x < 210) action = 2;
                if (x >= 220 && x < 260) action = 3;
                if (x >= 270 && x < 370) action = 4;
                if (x >= 380 && x < 500) action = 5;
                if (x >= 510 && x < 650) action = 6;
            }
            if(action && stacking_limited(&a->c)) action=0;
            if (action) {
                unsigned target = a->stack.target;
                if (action == 1) {
                    a->stacking = !a->stacking; a->stack_paused = 0;
                }
                if (action == 5 && a->stacking) {
                    a->stack_paused = !a->stack_paused; interrupt_stack(a);
                }
                if (action == 2 && target > 1) --target;
                if (action == 3 && target < 512) ++target;
                /* Increasing the target extends an existing acquisition. */
                if (action == 1 || action == 4 || action == 6 || target < a->stack.count ||
                    (a->stack.rolling && target!=a->stack.target)) {
                    int mode=action==6?!a->stack.rolling:a->stack.rolling;
                    reset_stack(a,target,mode);
                    /* Retain the displayed image while awaiting new frames. */
                } else if(a->stack.target!=target) {
                    a->stack.target=target; ++a->stack_epoch;
                }
            }
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
        if(info_screen.active) {
            draw_info_screen(r,&a->c); SDL_RenderPresent(r); SDL_Delay(16);
            continue;
        }
        SDL_SetRenderDrawColor(r,14,19,27,255); SDL_RenderClear(r);
        capture_view(a,view);
        const View *v=view;
        SDL_UpdateTexture(texture,NULL,v->pixels,WIDTH*sizeof(uint32_t));
        SDL_Rect video=v->stacking ? (SDL_Rect){10,160,460,345} : (SDL_Rect){120,65,720,540};
        SDL_RenderCopy(r,texture,NULL,&video);
        if(v->stacking) {
            SDL_UpdateTexture(texture,NULL,v->live,WIDTH*sizeof(uint32_t));
            SDL_Rect live={490,160,460,345}; SDL_RenderCopy(r,texture,NULL,&live);
            SDL_SetRenderDrawColor(r,220,230,240,255);
            text(r,20,135,v->stack_paused ? "STACK PAUSED" :
                v->stack.status ? v->stack.status : "SEEKING ALIGNMENT",2);
            text(r,500,135,"LIVE",2);
        }
        SDL_SetRenderDrawColor(r,220,230,240,255);
        char buf[256];
        snprintf(buf,sizeof(buf),"%.3f MHZ",(v->actual_hz+v->c.lo)/1e6);
        text(r,20,8,buf,6);
        int settings_x=20+(int)strlen(buf)*24+24;
        snprintf(buf,sizeof(buf),"%s %.0f DB   %.2f MS/S   BW %.2f MHZ",!strcmp(v->c.mode,"hackrf")?"LNA":"GAIN",v->actual_gain,v->c.rate/1e6,v->c.bandwidth/1e6);
        text(r,settings_x,15,buf,2);
        text(r,settings_x,30,"T FREQUENCY MHZ   SHIFT+1 INFO",2);
        int locked=v->sync_locked && v->last_frame && SDL_GetTicks()-v->last_frame<150;
        snprintf(buf,sizeof(buf),"%s  %s  IF %.1f DBFS  FIELDS %u  DROPS %u",
            !strcmp(v->c.mode,"demo")?"DEMO":!strcmp(v->c.mode,"file")?(v->done?"FILE END":"FILE"):"LIVE",
            v->free_sync?"FREE SYNC":locked?"PAL SYNC":"SYNC SEEK",v->if_power_dbfs,v->frames,v->overflows);
        text(r,20,45,buf,2);
        snprintf(buf,sizeof(buf),"IF %.2f MHZ [/]  ALIGN %.2f MHZ  FM%d [D]  F SYNC  LINE %.1f HZ ,/.  B/V",
            v->if_bandwidth/1e6,(v->c.alignment_bandwidth>0?v->c.alignment_bandwidth:v->if_bandwidth)/1e6,v->fm_mode,v->line_hz);
        text(r,20,612,buf,2);
        for (int db=0;db>=-100;db-=20) {
            int y=630-db*140/100;
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
            int y=630+(int)(fminf(100,fmaxf(0,-peak))*1.4f);
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

        SDL_Rect afc_button={750,27,190,29};
        SDL_SetRenderDrawColor(r,v->afc_enabled?45:35,v->afc_enabled?85:45,65,255);
        SDL_RenderFillRect(r,&afc_button);
        SDL_SetRenderDrawColor(r,220,230,240,255);
        int can_afc=strcmp(v->c.mode,"file") && strcmp(v->c.mode,"demo");
        snprintf(buf,sizeof(buf),"C AFC %s",!can_afc?"N/A":!v->afc_enabled?"OFF":
            v->afc_state==1?"TRACK":v->afc_state==2?"LIMIT":"SEEK");
        if(v->afc_enabled) {
            size_t used=strlen(buf);
            snprintf(buf+used,sizeof(buf)-used," %+.0fK",(v->actual_hz-v->afc_anchor)/1e3);
        }
        text(r,758,36,buf,2);

        const SDL_Rect buttons[] = {{20,830,140,30},{170,830,40,30},{220,830,40,30},{270,830,100,30},{380,830,120,30},{510,830,140,30}};
        const char *labels[] = {stacking_limited(&v->c) ? "STACK N/A" : v->stacking ? "STACK ON S" : "STACK OFF S", "-", "+", "RESET R", v->stack_paused ? "RESUME" : "PAUSE", v->stack.rolling ? "ROLLING M" : "FIXED M"};
        for (int k=0;k<6;++k) {
            SDL_SetRenderDrawColor(r,45,65,80,255); SDL_RenderFillRect(r,&buttons[k]);
            SDL_SetRenderDrawColor(r,220,230,240,255);
            text(r,buttons[k].x+8,840,labels[k],2);
        }
        if(stacking_limited(&v->c)) {
            SDL_SetRenderDrawColor(r,220,230,240,255);
            text(r,20,866,"LIVE SDR: STACK DISABLED ABOVE 10 MS/S",2);
        }
        text(r,660,866,save_notice && SDL_GetTicks()-save_notice<5000 ?
            (save_result?"CONFIG SAVED":"SAVE FAILED: SEE TERMINAL"):"W SAVE SETTINGS",2);
        const char *status = !v->stacking ? "OFF" : v->stack_paused ? "PAUSED" :
            !v->stack.rolling && v->stack.count >= v->stack.target ? "DONE" :
            v->last_stacked && SDL_GetTicks()-v->last_stacked < 200 ? "ADDING" : "HOLD";
        snprintf(buf,sizeof(buf),"FRAMES %u/%u  %s",v->stack.count,v->stack.target,status);
        text(r,660,840,buf,2);
        if (v->stacking) {
            if(v->stack_synced) snprintf(buf,sizeof(buf),"SYNC ALIGN  H AND V CONFIRMED  SKIP %llu",(unsigned long long)v->stack_skipped);
            else snprintf(buf,sizeof(buf),"IMAGE ALIGN  MATCH %.2f  X %.1f  Y %.1f  SKEW %.1f  SKIP %llu",
                v->stack.confidence,v->stack.dx,v->stack.dy,v->stack.shear,(unsigned long long)v->stack_skipped);
            text(r,20,866,buf,2);
        }

        if(color_enabled(v->c.rate,v->if_bandwidth))
            text(r,20,803,v->color_locked?"PAL COLOR LOCK  /  I INVERT  F FREE SYNC":"PAL AUTO: SEEK BURST / MONO  /  I INVERT",2);
        else text(r,20,803,"MONO / ARROWS TUNE/GAIN / I INVERT",2);
        int selected=SDL_AtomicGet(&a->audio_source);
        const char *audio_source=selected==3?"FM 7.38/7.56":selected==2?"FM 7.02/7.22":selected==1?"NICAM":"FM 6";
        if(v->c.rate<14e6)
            snprintf(buf,sizeof(buf),"AUDIO OFF: MIN RATE 14 MS/S");
        else if(v->if_bandwidth<14e6)
            snprintf(buf,sizeof(buf),"AUDIO OFF: MIN IF 14 MHZ");
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
        if(v->quality.valid && SDL_GetTicks()-v->quality_tick<1000)
            snprintf(buf,sizeof(buf),"VIDEO SNR~ %.1f DB",v->quality.video_snr_db);
        else snprintf(buf,sizeof(buf),"VIDEO SNR~ -- (%s)",
            v->quality_tick && SDL_GetTicks()-v->quality_tick>=1000?"stale":video_quality_status(&v->quality));
        text(r,20,820,buf,1);
        snprintf(buf,sizeof(buf),"CLIPPING: %llu SAMPLES",(unsigned long long)v->clipping.clipped_samples);
        text(r,550,820,buf,1);
        if(clipping_active(&v->clipping,SDL_GetTicks())) {
            SDL_Rect bar={120,65,720,26};
            SDL_SetRenderDrawColor(r,200,0,0,255); SDL_RenderFillRect(r,&bar);
            SDL_SetRenderDrawColor(r,255,255,255,255);
            snprintf(buf,sizeof(buf),"CLIPPING  %llu SAMPLES - LOWER GAIN",
                (unsigned long long)v->clipping.clipped_samples);
            text(r,130,72,buf,2);
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
        if(screenshot_requested) {
            char path[160];
            save_window_png(r,v->actual_hz+v->c.lo,path,sizeof(path));
            screenshot_requested=0;
        }
        SDL_RenderPresent(r);
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
    if(a->stack.rolling) fprintf(stderr,"Rolling accepted fields: %llu\n",(unsigned long long)a->stack.accepted);
    fprintf(stderr,"Stack frames skipped: %llu\n",(unsigned long long)a->stack_skipped);
    fprintf(stderr,"Displayed fields: %u; stream discontinuities: %u\n",a->frames,a->overflows);
cleanup:
    status_server_close(status_server);
    if (result && !SDL_AtomicGet(&a->failed)) fprintf(stderr,"UI: %s\n",SDL_GetError());
    if(thread || stack_thread) {
        SDL_AtomicSet(&a->stop,1);
        SDL_LockMutex(a->mutex); SDL_CondBroadcast(a->stack_ready); SDL_UnlockMutex(a->mutex);
        if(thread) SDL_WaitThread(thread,NULL);
        if(stack_thread) SDL_WaitThread(stack_thread,NULL);
    }
    if (texture) SDL_DestroyTexture(texture);
    free(view);
    if (r) { ui_text_destroy(r); SDL_DestroyRenderer(r); }
    if (window) SDL_DestroyWindow(window);
    if(a->stack_ready) SDL_DestroyCond(a->stack_ready);
    if (a->mutex) SDL_DestroyMutex(a->mutex);
    if(a->audio_device) SDL_CloseAudioDevice(a->audio_device);
    source_close(a->source); stack_destroy(&a->stack); free(a); return result;
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
