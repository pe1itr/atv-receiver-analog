#include "alignment.h"
#include "iq_dc_block.h"
#include "iq_power.h"
#include <SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
    IQ *iq;
    float *video;
    size_t n;
    double power;
    uint32_t power_tick;
} FilterBlock;

struct Alignment {
    SDL_mutex *mutex;
    SDL_cond *changed;
    SDL_Thread *thread;
    IFFilter *filter, *deemphasis;
    FMDemod demod;
    FilterBlock blocks[2];
    uint64_t submitted, completed;
    size_t capacity;
    double rate, bandwidth, deviation;
    double if_power_dbfs;
    uint32_t power_tick;
    uint64_t power_countdown;
    int stop, video_mode, fm_mode;
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
        while(a->completed==a->submitted && !a->stop) SDL_CondWait(a->changed,a->mutex);
        if(a->stop) break;
        FilterBlock *block=&a->blocks[a->completed%2];
        SDL_UnlockMutex(a->mutex);
        if_filter_process(a->filter,block->iq,block->n);
        /* Sample one block about 25 times/s, only on the video branch.
         * Measure AC power before FM removes amplitude information. Exclude
         * DC regardless of whether the FM path's DC blocker is enabled. */
        if(a->video_mode && block->n) {
            if(!a->power_countdown) {
                a->if_power_dbfs=iq_ac_power_dbfs(block->iq,block->n);
                a->power_tick=SDL_GetTicks();
                a->power_countdown=(uint64_t)(a->rate/25);
            }
            a->power_countdown=a->power_countdown>block->n?a->power_countdown-block->n:0;
        }
        /* FM correction is independent of the DC-excluding power estimate.
         * Raw clipping and recordings are unaffected by this DSP step. */
        if(a->dc_block_enabled) iq_dc_block_process(&a->dc_block,block->iq,block->n);
        fm_demod_process(&a->demod,block->iq,block->video,block->n);
        if(a->consume) a->consume(a->consumer,block->video,block->n);
        if(a->deemphasis) video_deemphasis_process(a->deemphasis,block->video,block->n);
        SDL_LockMutex(a->mutex);
        block->power=a->if_power_dbfs;
        block->power_tick=a->power_tick;
        ++a->completed;
        SDL_CondBroadcast(a->changed);
    }
    SDL_UnlockMutex(a->mutex);
    return 0;
}
static const float *result(Alignment *a, unsigned newer, double *power, uint32_t *measured_at)
{
    SDL_LockMutex(a->mutex);
    uint64_t sequence=a->submitted-newer;
    while(a->completed<sequence) SDL_CondWait(a->changed,a->mutex);
    FilterBlock *block=&a->blocks[(sequence-1)%2];
    if(power) *power=block->power;
    if(measured_at) *measured_at=block->power_tick;
    SDL_UnlockMutex(a->mutex);
    return block->video;
}
const float *alignment_previous(Alignment *a, double *power, uint32_t *measured_at)
{
    return result(a,1,power,measured_at);
}
const float *alignment_wait(Alignment *a)
{
    if(!a->submitted) return a->blocks[0].video;
    return result(a,0,NULL,NULL);
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
    a->blocks[0].iq=malloc(capacity*sizeof(IQ)); a->blocks[0].video=malloc(capacity*sizeof(float));
    if(!a->mutex || !a->changed || !a->filter || !a->blocks[0].iq || !a->blocks[0].video) goto fail;
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
static int submit(Alignment *a, const IQ *iq, size_t n, unsigned limit)
{
    if(n>a->capacity) return -1;
    SDL_LockMutex(a->mutex);
    if(a->submitted-a->completed>=limit || a->stop) { SDL_UnlockMutex(a->mutex); return -1; }
    FilterBlock *block=&a->blocks[a->submitted%2];
    SDL_UnlockMutex(a->mutex);
    /* The sole producer owns this slot until publication. The consumer of
     * its old output has already finished before requesting reuse. */
    if(!block->iq) block->iq=malloc(a->capacity*sizeof(IQ));
    if(!block->video) block->video=malloc(a->capacity*sizeof(float));
    if(!block->iq || !block->video) return -1;
    memcpy(block->iq,iq,n*sizeof(IQ)); block->n=n;
    SDL_LockMutex(a->mutex);
    ++a->submitted;
    SDL_CondSignal(a->changed);
    SDL_UnlockMutex(a->mutex);
    return 0;
}
int alignment_submit(Alignment *a, const IQ *iq, size_t n) { return submit(a,iq,n,1); }
int alignment_queue(Alignment *a, const IQ *iq, size_t n) { return submit(a,iq,n,2); }
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
    if_filter_destroy(a->deemphasis); if_filter_destroy(a->filter);
    for(unsigned i=0;i<2;++i) { free(a->blocks[i].iq); free(a->blocks[i].video); }
    free(a);
}
