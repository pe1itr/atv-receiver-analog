#include "alignment.h"
#include "iq_dc_block.h"
#include "iq_power.h"
#include <SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
struct Alignment {
    SDL_mutex *mutex;
    SDL_cond *changed;
    SDL_Thread *thread;
    IFFilter *filter, *deemphasis;
    FMDemod demod;
    IQ *iq;
    float *video;
    size_t capacity, n;
    double rate, bandwidth, deviation;
    double if_power_dbfs;
    uint32_t power_tick;
    uint64_t power_countdown;
    int busy, stop, video_mode, fm_mode;
    int dc_block_enabled;
    IQDCBlock dc_block;
    void (*consume)(void *, const float *, size_t);
    void *consumer;
};
static int run(void *opaque)
{
    Alignment *a=opaque;
    SDL_LockMutex(a->mutex);
    for(;;) {
        while(!a->busy && !a->stop) SDL_CondWait(a->changed,a->mutex);
        if(a->stop) break;
        SDL_UnlockMutex(a->mutex);
        if_filter_process(a->filter,a->iq,a->n);
        /* Sample one block about 25 times/s, only on the video branch.
         * Measure AC power before FM removes amplitude information. Exclude
         * DC regardless of whether the FM path's DC blocker is enabled. */
        if(a->video_mode && a->n) {
            if(!a->power_countdown) {
                a->if_power_dbfs=iq_ac_power_dbfs(a->iq,a->n);
                a->power_tick=SDL_GetTicks();
                a->power_countdown=(uint64_t)(a->rate/25);
            }
            a->power_countdown=a->power_countdown>a->n?a->power_countdown-a->n:0;
        }
        /* FM correction is independent of the DC-excluding power estimate.
         * Raw clipping and recordings are unaffected by this DSP step. */
        if(a->dc_block_enabled) iq_dc_block_process(&a->dc_block,a->iq,a->n);
        fm_demod_process(&a->demod,a->iq,a->video,a->n);
        if(a->consume) a->consume(a->consumer,a->video,a->n);
        if(a->deemphasis) video_deemphasis_process(a->deemphasis,a->video,a->n);
        SDL_LockMutex(a->mutex);
        a->busy=0;
        SDL_CondBroadcast(a->changed);
    }
    SDL_UnlockMutex(a->mutex);
    return 0;
}
const float *alignment_wait(Alignment *a)
{
    SDL_LockMutex(a->mutex);
    while(a->busy) SDL_CondWait(a->changed,a->mutex);
    SDL_UnlockMutex(a->mutex);
    return a->video;
}
double alignment_if_power(Alignment *a, uint32_t *measured_at)
{
    alignment_wait(a);
    if(measured_at) *measured_at=a->power_tick;
    return a->if_power_dbfs;
}
void alignment_set_consumer(Alignment *a, void (*consume)(void *, const float *, size_t), void *opaque)
{
    alignment_wait(a);
    a->consume=consume; a->consumer=opaque;
}
int alignment_set_deemphasis(Alignment *a)
{
    alignment_wait(a);
    if(!a->video_mode) return -1;
    if(!a->deemphasis) a->deemphasis=video_deemphasis_create(a->rate);
    if(!a->deemphasis) return -1;
    return 0;
}
void alignment_reset(Alignment *a, int invert)
{
    alignment_wait(a);
    /* Worker is idle until this sole caller submits another block. */
    a->if_power_dbfs=NAN; a->power_countdown=0; a->power_tick=0;
    if_filter_reset(a->filter);
    if(a->deemphasis) if_filter_reset(a->deemphasis);
    iq_dc_block_reset(&a->dc_block,a->rate);
    unsigned stride=(a->video_mode || a->fm_mode)?1:(unsigned)fmax(1,floor(a->rate/fmax(4e6,2*a->bandwidth)));
    fm_demod_init(&a->demod,a->rate,a->deviation,invert,stride);
    a->demod.mode=a->fm_mode;
    if(a->video_mode==2) a->demod.alpha=1;
}
void alignment_set_dc_block(Alignment *a,int enabled)
{
    alignment_wait(a);
    a->dc_block_enabled=enabled!=0;
    iq_dc_block_reset(&a->dc_block,a->rate);
}
void alignment_set_fm_mode(Alignment *a, int mode, int invert)
{
    alignment_wait(a);
    a->fm_mode=mode!=0;
    alignment_reset(a,invert);
}
static Alignment *create(double rate, double bandwidth, double deviation, int invert, size_t capacity, int video_mode)
{
    if(!capacity || capacity>SIZE_MAX/sizeof(IQ)) return NULL;
    Alignment *a=calloc(1,sizeof(*a));
    if(!a) return NULL;
    a->video_mode=video_mode;
    a->rate=rate; a->bandwidth=bandwidth; a->deviation=deviation; a->capacity=capacity;
    a->mutex=SDL_CreateMutex(); a->changed=SDL_CreateCond();
    a->filter=if_filter_create(rate,bandwidth);
    a->iq=malloc(capacity*sizeof(*a->iq)); a->video=malloc(capacity*sizeof(*a->video));
    if(!a->mutex || !a->changed || !a->filter || !a->iq || !a->video) goto fail;
    alignment_reset(a,invert);
    a->thread=SDL_CreateThread(run,video_mode?"Video-IF-FM":"Alignment-IF-FM",a);
    if(!a->thread) goto fail;
    return a;
fail:
    alignment_destroy(a); return NULL;
}
Alignment *alignment_create(double rate, double bandwidth, double deviation, int invert, size_t capacity)
{
    return create(rate,bandwidth,deviation,invert,capacity,0);
}
Alignment *alignment_create_video(double rate, double bandwidth, double deviation, int invert, size_t capacity, int raw_composite)
{
    return create(rate,bandwidth,deviation,invert,capacity,raw_composite?2:1);
}
int alignment_submit(Alignment *a, const IQ *iq, size_t n)
{
    if(n>a->capacity) return -1;
    SDL_LockMutex(a->mutex);
    if(a->busy || a->stop) { SDL_UnlockMutex(a->mutex); return -1; }
    memcpy(a->iq,iq,n*sizeof(*iq)); a->n=n; a->busy=1;
    SDL_CondSignal(a->changed);
    SDL_UnlockMutex(a->mutex);
    return 0;
}
void alignment_destroy(Alignment *a)
{
    if(!a) return;
    if(a->thread) {
        alignment_wait(a);
        SDL_LockMutex(a->mutex); a->stop=1; SDL_CondSignal(a->changed); SDL_UnlockMutex(a->mutex);
        SDL_WaitThread(a->thread,NULL);
    }
    if(a->changed) SDL_DestroyCond(a->changed);
    if(a->mutex) SDL_DestroyMutex(a->mutex);
    if_filter_destroy(a->deemphasis); if_filter_destroy(a->filter); free(a->iq); free(a->video); free(a);
}
