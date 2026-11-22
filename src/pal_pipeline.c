#include "pal_pipeline.h"
#include <SDL.h>
#include <stdlib.h>

/* Filter the next group while the decoder uses the recovered line clock to
 * track burst and draw the current one. Two bounded slots also work for the
 * largest Pluto blocks without allocating components for an entire block. */
struct PalPipeline {
    SDL_Thread *thread;
    SDL_mutex *mutex;
    SDL_cond *changed;
    PalDecoder filters;
    PalComponents samples[2][PAL_CHUNK];
    const float *video;
    uint64_t tick;
    size_t remaining;
    unsigned produced, consumed;
    int stop;
};
static int prepare(void *opaque)
{
    PalPipeline *p=opaque;
    SDL_LockMutex(p->mutex);
    for(;;) {
        while(!p->stop && (!p->remaining || p->produced-p->consumed==2))
            SDL_CondWait(p->changed,p->mutex);
        if(p->stop) break;
        size_t n=p->remaining<PAL_CHUNK?p->remaining:PAL_CHUNK;
        const float *video=p->video;
        uint64_t tick=p->tick;
        PalComponents *out=p->samples[p->produced%2];
        SDL_UnlockMutex(p->mutex);
        pal_prepare_block(&p->filters,video,tick,out,n);
        SDL_LockMutex(p->mutex);
        ++p->produced; p->tick+=n; p->video+=n; p->remaining-=n;
        SDL_CondBroadcast(p->changed);
    }
    SDL_UnlockMutex(p->mutex);
    return 0;
}
PalPipeline *pal_pipeline_create(void)
{
    PalPipeline *p=calloc(1,sizeof(*p));
    if(!p) return NULL;
    p->mutex=SDL_CreateMutex(); p->changed=SDL_CreateCond();
    if(p->mutex && p->changed) p->thread=SDL_CreateThread(prepare,"PAL-components",p);
    if(!p->thread) { pal_pipeline_destroy(p); return NULL; }
    return p;
}
void pal_pipeline_destroy(PalPipeline *p)
{
    if(!p) return;
    if(p->thread) {
        SDL_LockMutex(p->mutex); p->stop=1; SDL_CondBroadcast(p->changed); SDL_UnlockMutex(p->mutex);
        SDL_WaitThread(p->thread,NULL);
    }
    if(p->changed) SDL_DestroyCond(p->changed);
    if(p->mutex) SDL_DestroyMutex(p->mutex);
    free(p);
}
void pal_pipeline_decode(PalPipeline *p, Decoder *d, const float *video, const float *guide, size_t n)
{
    if(!p || !d->color) { decoder_process_video(d,video,guide,n); return; }
    if(!n) return;
    if(!d->previous_valid) {
        pal_init(&p->filters,d->rate);
        /* Decoder's first input seeds FM history and yields no pixel. */
        decoder_process_video(d,video,guide,1);
        ++video; if(guide) ++guide;
        if(!--n) return;
    }
    SDL_LockMutex(p->mutex);
    p->tick=d->tick; p->video=video; p->remaining=n;
    SDL_CondBroadcast(p->changed);
    while(n) {
        while(p->consumed==p->produced) SDL_CondWait(p->changed,p->mutex);
        size_t part=n<PAL_CHUNK?n:PAL_CHUNK;
        const PalComponents *samples=p->samples[p->consumed%2];
        SDL_UnlockMutex(p->mutex);
        decoder_process_prepared(d,video,guide,samples,part);
        video+=part; if(guide) guide+=part; n-=part;
        SDL_LockMutex(p->mutex);
        ++p->consumed;
        SDL_CondBroadcast(p->changed);
    }
    SDL_UnlockMutex(p->mutex);
}
