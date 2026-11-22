#define main receiver_main
#include "../src/main.c"
#undef main
#include <assert.h>

static void idle(App *a)
{
    uint32_t start=SDL_GetTicks();
    SDL_LockMutex(a->mutex);
    while(a->stack_busy || a->stack_pending) {
        assert(SDL_GetTicks()-start<3000);
        SDL_CondWaitTimeout(a->stack_ready,a->mutex,100);
    }
    SDL_UnlockMutex(a->mutex);
}
static void feed(App *a,uint32_t color)
{
    for(size_t j=0;j<WIDTH*HEIGHT;++j) a->decoder->pixels[j]=color;
    if(a->decoder->preview) live_frame(a,a->decoder->pixels);
    frame(a,a->decoder->pixels); idle(a);
}
int main(void)
{
    assert(SDL_Init(SDL_INIT_TIMER)==0);
    App *a=calloc(1,sizeof(*a)); Decoder *d=calloc(1,sizeof(*d)); assert(a && d);
    a->c=(Config){.mode="b210",.rate=10e6}; a->decoder=d;
    a->mutex=SDL_CreateMutex(); a->stack_ready=SDL_CreateCond();
    decoder_init(d,10e6,4e6,0,NULL,NULL); d->free_sync=1;
    reset_stack(a,5,1); a->stacking=1;
    SDL_Thread *thread=SDL_CreateThread(stack_worker,"rolling-test",a); assert(thread);
    /* Flat, incomplete fields without sync must still advance, in RGB. */
    for(int k=0;k<5;++k) feed(a,0xffff0000);
    assert(a->stack.count==5 && a->stack.accepted==5 && a->pixels[0]==0xffff0000);
    /* Recovering sync must not restart a running live averaging window. */
    d->hsync_updates=d->vsync_updates=d->vsync_epoch=1;
    d->hsync_field=d->fields; d->vsync_confirmed=d->tick;
    feed(a,0xff0000ff);
    assert(a->stack.accepted==6 && a->pixels[0]==0xffcc0033);
    /* Preview and alignment callbacks must count a displayed picture once. */
    d->preview=d; feed(a,0xff0000ff);
    assert(a->stack.accepted==7 && a->pixels[0]==0xff990066);
    SDL_LockMutex(a->mutex); stack_action(a,5); SDL_UnlockMutex(a->mutex);
    feed(a,0xff00ff00); assert(a->stack.accepted==7 && a->pixels[0]==0xff990066);
    SDL_LockMutex(a->mutex); stack_action(a,5); SDL_UnlockMutex(a->mutex);
    feed(a,0xff00ff00); assert(a->stack.count==1 && a->pixels[0]==0xff00ff00);
    SDL_LockMutex(a->mutex); stack_action(a,2); SDL_UnlockMutex(a->mutex);
    feed(a,0xff123456); assert(a->stack.target==4 && a->stack.count==1 && a->pixels[0]==0xff123456);
    /* Fixed still rejects incomplete frames; switching back is immediate. */
    SDL_LockMutex(a->mutex); stack_action(a,6); SDL_UnlockMutex(a->mutex);
    feed(a,0xff000000); assert(!a->stack.rolling && a->stack.count==0);
    SDL_LockMutex(a->mutex); stack_action(a,6); SDL_UnlockMutex(a->mutex);
    feed(a,0xffabcdef); assert(a->stack.count==1 && a->pixels[0]==0xffabcdef);
    View *v=calloc(1,sizeof(*v)); assert(v);
    capture_view(a,v);
    assert(v->pixels[0]==0xffabcdef && v->live[0]==0xffabcdef);
    SDL_LockMutex(a->mutex); stack_action(a,1); SDL_UnlockMutex(a->mutex);
    feed(a,0xff2468ac);
    capture_view(a,v);
    assert(!v->stacking && v->pixels[0]==0xff2468ac);
    d->preview=NULL;
    feed(a,0xff345678);
    capture_view(a,v);
    assert(v->pixels[0]==0xff345678);
    free(v);
    SDL_AtomicSet(&a->stop,1); SDL_CondBroadcast(a->stack_ready); SDL_WaitThread(thread,NULL);
    SDL_DestroyCond(a->stack_ready); SDL_DestroyMutex(a->mutex); free(d); free(a); SDL_Quit();
    puts("Live rolling: five-picture RGB eviction, no frame gating, preview once, pause, resize and fixed/rolling switch OK");
}
