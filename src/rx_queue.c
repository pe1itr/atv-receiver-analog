#include "rx_queue.h"
#include <SDL.h>
#include <stdlib.h>
#include <string.h>

typedef struct { size_t count; int gap; } Slot;
struct RxQueue {
    SDL_mutex *mutex;
    SDL_cond *changed;
    SDL_Thread *thread;
    IQ *data, *scratch;
    Slot *slots;
    size_t block, capacity, head, count, offset;
    int stop, failed, gap, control_pending, control_result;
    int live_control;
    uint64_t tune_epoch;
    RxReadFn read;
    void *opaque;
    RxControlFn control;
    void *control_opaque;
    SourceStats stats;
};

static void flush(RxQueue *q)
{
    q->head=q->count=q->offset=0;
    q->stats.queued_samples=0;
}
static int receive(void *opaque)
{
    RxQueue *q=opaque;
    for(;;) {
        SDL_LockMutex(q->mutex);
        if(q->stop) { SDL_UnlockMutex(q->mutex); break; }
        if(q->control_pending) {
            RxControlFn fn=q->control; void *arg=q->control_opaque;
            SDL_UnlockMutex(q->mutex);
            int result=fn(arg);
            SDL_LockMutex(q->mutex);
            /* No old-frequency samples may follow a completed retune. The
             * caller already resets DSP for this intentional discontinuity. */
            flush(q); q->gap=0;
            q->control_result=result; q->control_pending=0;
            SDL_CondBroadcast(q->changed);
        }
        int stop=q->stop;
        uint64_t epoch=q->tune_epoch;
        SDL_UnlockMutex(q->mutex);
        if(stop) break;

        int n=q->read(q->opaque,q->scratch,q->block);
        SDL_LockMutex(q->mutex);
        if(q->stop) { SDL_UnlockMutex(q->mutex); break; }
        if(n==-2) {
            ++q->stats.hardware_overflows; q->gap=1;
        } else if(n<0 || (size_t)n>q->block) {
            q->failed=1;
        } else if(q->live_control || epoch!=q->tune_epoch) {
            /* Keep draining during concurrent tuning, but never publish a
             * block whose read overlaps the frequency transition. */
        } else if(n>0) {
            if(q->count==q->capacity) {
                /* Never stop draining the SDR. Catch up to current samples
                 * and explicitly mark the gap before the next delivered IQ. */
                q->stats.discarded_samples+=q->stats.queued_samples;
                flush(q); q->gap=1;
            }
            size_t tail=(q->head+q->count)%q->capacity;
            memcpy(q->data+tail*q->block,q->scratch,(size_t)n*sizeof(IQ));
            q->slots[tail]=(Slot){(size_t)n,q->gap}; q->gap=0;
            ++q->count; q->stats.queued_samples+=(size_t)n;
            if(q->stats.queued_samples>q->stats.peak_samples)
                q->stats.peak_samples=q->stats.queued_samples;
        }
        int failed=q->failed;
        if(n) SDL_CondBroadcast(q->changed);
        SDL_UnlockMutex(q->mutex);
        if(failed) break;
    }
    return 0;
}

RxQueue *rx_queue_create(size_t block, size_t slots, RxReadFn read, void *opaque)
{
    if(!block || !slots || !read || slots>SIZE_MAX/block/sizeof(IQ)) return NULL;
    RxQueue *q=calloc(1,sizeof(*q));
    if(!q) return NULL;
    q->block=block; q->capacity=slots; q->read=read; q->opaque=opaque;
    q->stats.capacity_samples=block*slots;
    q->mutex=SDL_CreateMutex(); q->changed=SDL_CreateCond();
    q->data=malloc(block*slots*sizeof(IQ)); q->scratch=malloc(block*sizeof(IQ));
    q->slots=calloc(slots,sizeof(*q->slots));
    if(!q->mutex || !q->changed || !q->data || !q->scratch || !q->slots) goto fail;
    memset(q->data,0,block*slots*sizeof(IQ));
    memset(q->scratch,0,block*sizeof(IQ));
    q->thread=SDL_CreateThread(receive,"SDR-RX",q);
    if(!q->thread) goto fail;
    return q;
fail:
    rx_queue_destroy(q); return NULL;
}
int rx_queue_read(RxQueue *q, IQ *iq, size_t capacity)
{
    if(!capacity) return 0;
    SDL_LockMutex(q->mutex);
    while(!q->count && !q->gap && !q->failed && !q->stop) {
        int result=SDL_CondWaitTimeout(q->changed,q->mutex,100);
        if(result) { SDL_UnlockMutex(q->mutex); return result==SDL_MUTEX_TIMEDOUT?0:-1; }
    }
    int result=0;
    if(q->count) {
        Slot *s=&q->slots[q->head];
        if(s->gap) { s->gap=0; result=-2; }
        else {
            size_t n=s->count-q->offset;
            if(n>capacity) n=capacity;
            memcpy(iq,q->data+q->head*q->block+q->offset,n*sizeof(IQ));
            q->offset+=n; q->stats.queued_samples-=n; result=(int)n;
            if(q->offset==s->count) {
                q->head=(q->head+1)%q->capacity; --q->count; q->offset=0;
            }
        }
    } else if(q->gap) { q->gap=0; result=-2; }
    else if(q->failed) result=-1;
    SDL_UnlockMutex(q->mutex);
    return result;
}
int rx_queue_control(RxQueue *q, RxControlFn fn, void *opaque)
{
    SDL_LockMutex(q->mutex);
    if(q->stop || q->failed || q->control_pending || q->live_control) { SDL_UnlockMutex(q->mutex); return -1; }
    q->control=fn; q->control_opaque=opaque; q->control_pending=1;
    while(q->control_pending && !q->stop && !q->failed)
        SDL_CondWait(q->changed,q->mutex);
    int result=q->stop || q->failed ? -1 : q->control_result;
    SDL_UnlockMutex(q->mutex);
    return result;
}
int rx_queue_control_live(RxQueue *q, RxControlFn fn, void *opaque)
{
    SDL_LockMutex(q->mutex);
    if(q->stop || q->failed || q->control_pending || q->live_control) { SDL_UnlockMutex(q->mutex); return -1; }
    q->live_control=1; ++q->tune_epoch;
    flush(q); q->gap=0;
    SDL_UnlockMutex(q->mutex);
    int result=fn(opaque);
    SDL_LockMutex(q->mutex);
    q->live_control=0; ++q->tune_epoch;
    /* Invalidate any read still in progress, including one begun during
     * tuning. Real hardware errors remain visible in the statistics. */
    flush(q);
    if(q->stop || q->failed) result=-1;
    SDL_CondBroadcast(q->changed);
    SDL_UnlockMutex(q->mutex);
    return result;
}
void rx_queue_stats(RxQueue *q, SourceStats *stats)
{
    SDL_LockMutex(q->mutex); *stats=q->stats; SDL_UnlockMutex(q->mutex);
}
void rx_queue_stop(RxQueue *q)
{
    if(!q || !q->mutex) return;
    SDL_LockMutex(q->mutex); q->stop=1;
    if(q->changed) SDL_CondBroadcast(q->changed);
    SDL_UnlockMutex(q->mutex);
    if(q->thread) { SDL_WaitThread(q->thread,NULL); q->thread=NULL; }
}
void rx_queue_destroy(RxQueue *q)
{
    if(!q) return;
    rx_queue_stop(q);
    if(q->changed) SDL_DestroyCond(q->changed);
    if(q->mutex) SDL_DestroyMutex(q->mutex);
    free(q->data); free(q->scratch); free(q->slots); free(q);
}
