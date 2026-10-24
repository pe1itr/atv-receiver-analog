#include "nicam.h"
#include "nicam_core/core.h"
#include <SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#define PI 3.14159265358979323846
#define NTAPS 65
#define PHASES 64
#define QUEUE_IQ 364000u
struct Nicam {
    double rate,c,s,cs,sn,midrate,outphase,power;
    float alpha,re[4],im[4],det_re[4],det_im[4],taps[NTAPS];
    float hist_re[2*NTAPS],hist_im[2*NTAPS],poly[PHASES][NTAPS];
    unsigned stride,phase,pos,overflows,good,bad;
    int detected,selected,stop,abort;
    SDL_Thread *thread;
    SDL_mutex *mutex;
    SDL_cond *ready;
    SDL_atomic_t locked,last_good;
    SDL_atomic_t good_frames,bad_frames,muted_frames,sync_bits_discarded;
    int16_t *queue;
    size_t head,count;
    SDL_AudioStream *convert;
    NicamOutput output;
    void *opaque;
};
static int read_iq(void *opaque,void *dest,size_t bytes)
{
    Nicam *n=opaque; size_t want=bytes/4,got=0;
    int16_t *out=dest;
    SDL_LockMutex(n->mutex);
    while(got<want) {
        while(!n->count && !n->stop) SDL_CondWait(n->ready,n->mutex);
        if(n->abort || (!n->count && n->stop)) break;
        size_t take=want-got;
        if(take>n->count) take=n->count;
        if(take>QUEUE_IQ-n->head) take=QUEUE_IQ-n->head;
        memcpy(out+got*2,n->queue+n->head*2,take*4);
        n->head=(n->head+take)%QUEUE_IQ; n->count-=take; got+=take;
    }
    int aborted=n->abort;
    SDL_UnlockMutex(n->mutex);
    return aborted?0:(int)(got*4);
}
static void quality(void *opaque,int valid)
{
    Nicam *n=opaque;
    SDL_AtomicAdd(valid?&n->good_frames:&n->bad_frames,1);
    if(valid) {
        if(SDL_GetTicks()-(Uint32)SDL_AtomicGet(&n->last_good)>=100) n->good=0;
        n->bad=0; if(n->good<4) ++n->good;
        if(n->good>=4) SDL_AtomicSet(&n->locked,1);
        SDL_AtomicSet(&n->last_good,(int)SDL_GetTicks());
    } else {
        n->good=0;
        if(++n->bad>=16) SDL_AtomicSet(&n->locked,0);
    }
}
static int output_pcm(void *opaque,const void *pcm,size_t bytes)
{
    Nicam *n=opaque;
    SDL_LockMutex(n->mutex); int abort=n->abort; SDL_UnlockMutex(n->mutex);
    if(abort) return -1;
    int16_t silence[64]={0};
    if(bytes!=sizeof(silence)) return -1;
    int locked=SDL_AtomicGet(&n->locked);
    if(!locked) SDL_AtomicAdd(&n->muted_frames,1);
    if(SDL_AudioStreamPut(n->convert,locked?pcm:silence,(int)bytes)) return -1;
    float samples[512]; int got;
    while((got=SDL_AudioStreamGet(n->convert,samples,sizeof(samples)))>0)
        if(n->output) n->output(n->opaque,samples,(size_t)got/(2*sizeof(float)));
    return got<0?-1:0;
}
static void discard_bits(void *opaque,size_t count)
{
    Nicam *n=opaque;
    SDL_AtomicAdd(&n->sync_bits_discarded,(int)count);
}
void nicam_stats(Nicam *n,NicamStats *stats)
{
    memset(stats,0,sizeof(*stats));
    if(!n) return;
    stats->good_frames=(unsigned)SDL_AtomicGet(&n->good_frames);
    stats->bad_frames=(unsigned)SDL_AtomicGet(&n->bad_frames);
    stats->muted_frames=(unsigned)SDL_AtomicGet(&n->muted_frames);
    stats->sync_bits_discarded=(unsigned)SDL_AtomicGet(&n->sync_bits_discarded);
}
static int decode(void *opaque)
{
    Nicam *n=opaque;
    NicamCoreIO io={read_iq,output_pcm,quality,n,discard_bits};
    int result=nicam_core_run(&io);
    SDL_LockMutex(n->mutex); n->stop=1; SDL_UnlockMutex(n->mutex);
    return result;
}
static void stop(Nicam *n,int abort)
{
    SDL_LockMutex(n->mutex); n->stop=1; n->abort=n->abort || abort;
    SDL_CondBroadcast(n->ready); SDL_UnlockMutex(n->mutex);
    if(n->thread) { SDL_WaitThread(n->thread,NULL); n->thread=NULL; }
}
int nicam_select(Nicam *n,int enabled)
{
    stop(n,1);
    n->selected=0; n->head=n->count=0; n->good=n->bad=0;
    SDL_AtomicSet(&n->locked,0); SDL_AtomicSet(&n->last_good,0);
    SDL_AudioStreamClear(n->convert);
    if(!enabled) return 0;
    n->stop=n->abort=0;
    n->thread=SDL_CreateThread(decode,"NICAM",n);
    if(!n->thread) { n->stop=1; return -1; }
    n->selected=1;
    return 0;
}
void nicam_reset(Nicam *n)
{
    int selected=n->selected;
    nicam_select(n,0);
    n->c=1; n->s=n->outphase=n->power=0; n->phase=n->pos=0; n->detected=0;
    memset(n->re,0,sizeof(n->re)); memset(n->im,0,sizeof(n->im));
    memset(n->det_re,0,sizeof(n->det_re)); memset(n->det_im,0,sizeof(n->det_im));
    memset(n->hist_re,0,sizeof(n->hist_re)); memset(n->hist_im,0,sizeof(n->hist_im));
    if(selected) nicam_select(n,1);
}
Nicam *nicam_create(double rate,NicamOutput output,void *opaque)
{
    if(!isfinite(rate) || rate<14e6) return NULL;
    Nicam *n=calloc(1,sizeof(*n)); if(!n) return NULL;
    n->rate=rate; n->c=1; n->cs=cos(2*PI*6552000/rate); n->sn=sin(2*PI*6552000/rate);
    n->stride=(unsigned)floor(rate/3e6); n->midrate=rate/n->stride;
    n->alpha=1-exp(-2*PI*700000/rate);
    n->output=output; n->opaque=opaque;
    n->queue=malloc(QUEUE_IQ*4);
    n->mutex=SDL_CreateMutex(); n->ready=SDL_CreateCond();
    n->convert=SDL_NewAudioStream(AUDIO_S16SYS,2,32000,AUDIO_F32SYS,2,48000);
    if(!n->queue || !n->mutex || !n->ready || !n->convert) {
        if(n->convert) SDL_FreeAudioStream(n->convert);
        if(n->ready) SDL_DestroyCond(n->ready);
        if(n->mutex) SDL_DestroyMutex(n->mutex);
        free(n->queue); free(n); return NULL;
    }
    double sum=0;
    for(int k=0;k<NTAPS;++k) {
        int x=k-NTAPS/2;
        double sinc=x?sin(2*PI*380000/n->midrate*x)/(PI*x):2*380000/n->midrate;
        n->taps[k]=sinc*(.42-.5*cos(2*PI*k/(NTAPS-1))+.08*cos(4*PI*k/(NTAPS-1)));
        sum+=n->taps[k];
    }
    for(int k=0;k<NTAPS;++k) n->taps[k]/=sum;
    for(int phase=0;phase<PHASES;++phase) {
        float fraction=(float)phase/PHASES;
        for(int k=0;k<NTAPS;++k)
            n->poly[phase][k]=(1-fraction)*n->taps[k]+fraction*(k?n->taps[k-1]:0);
    }
    return n;
}
int nicam_push_iq(Nicam *n,const int16_t *iq,size_t count)
{
    SDL_LockMutex(n->mutex);
    if(n->stop) { SDL_UnlockMutex(n->mutex); return -1; }
    if(count>QUEUE_IQ-n->count) {
        ++n->overflows; n->stop=n->abort=1; n->count=0;
        SDL_AtomicSet(&n->locked,0); SDL_CondSignal(n->ready);
        SDL_UnlockMutex(n->mutex); return -1;
    }
    size_t tail=(n->head+n->count)%QUEUE_IQ,first=QUEUE_IQ-tail;
    if(first>count) first=count;
    memcpy(n->queue+tail*2,iq,first*4);
    memcpy(n->queue,iq+first*2,(count-first)*4);
    n->count+=count; SDL_CondSignal(n->ready); SDL_UnlockMutex(n->mutex);
    return 0;
}
static inline int16_t quantize(float value)
{
    value*=131072.f;
    if(!(value>-32767.f)) return -32767;
    if(value>32767.f) return 32767;
    return (int16_t)lrintf(value);
}
void nicam_process(Nicam *n,const float *video,size_t count,double fm_power)
{
    const float da=1-exp(-2*PI*100000/n->midrate);
    const double pa=1-exp(-1/(n->midrate*.030));
    int16_t iq[512]; unsigned used=0;
    for(size_t j=0;j<count;++j) {
        float v=isfinite(video[j])?video[j]:0;
        float re=v*n->c,im=-v*n->s;
        double c=n->c*n->cs-n->s*n->sn;
        n->s=n->s*n->cs+n->c*n->sn; n->c=c;
        for(int k=0;k<4;++k) {
            n->re[k]+=n->alpha*(re-n->re[k]); n->im[k]+=n->alpha*(im-n->im[k]);
            re=n->re[k]; im=n->im[k];
        }
        if(++n->phase<n->stride) continue;
        n->phase=0;
        if(!n->selected) {
            float dr=re,di=im;
            for(int k=0;k<4;++k) {
                n->det_re[k]+=da*(dr-n->det_re[k]); n->det_im[k]+=da*(di-n->det_im[k]);
                dr=n->det_re[k]; di=n->det_im[k];
            }
            n->power+=pa*(dr*dr+di*di-n->power);
            continue;
        }
        /* Mirrored history keeps the FIR contiguous. Polyphase coefficients
         * combine filtering and fractional delay in one dot product, instead
         * of filtering two adjacent instants and then interpolating them. */
        n->pos=n->pos?n->pos-1:NTAPS-1;
        n->hist_re[n->pos]=n->hist_re[n->pos+NTAPS]=re;
        n->hist_im[n->pos]=n->hist_im[n->pos+NTAPS]=im;
        n->outphase+=1456000;
        if(n->outphase<n->midrate) continue;
        n->outphase-=n->midrate;
        unsigned phase=(unsigned)(n->outphase*(PHASES/1456000.0));
        const float *h=n->poly[phase],*hr=n->hist_re+n->pos,*hi=n->hist_im+n->pos;
        float r0=0,r1=0,r2=0,r3=0,i0=0,i1=0,i2=0,i3=0;
        for(int k=0;k<64;k+=4) {
            r0+=h[k]*hr[k]; r1+=h[k+1]*hr[k+1]; r2+=h[k+2]*hr[k+2]; r3+=h[k+3]*hr[k+3];
            i0+=h[k]*hi[k]; i1+=h[k+1]*hi[k+1]; i2+=h[k+2]*hi[k+2]; i3+=h[k+3]*hi[k+3];
        }
        float rr=r0+r1+r2+r3+h[64]*hr[64],ii=i0+i1+i2+i3+h[64]*hi[64];
        /* Fixed headroom; the imported carrier loop includes AGC. */
        iq[2*used]=quantize(rr);
        iq[2*used+1]=quantize(ii);
        if(++used==256) { nicam_push_iq(n,iq,used); used=0; }
    }
    if(used) nicam_push_iq(n,iq,used);
    /* This indicates spectral energy only, never confirmed NICAM framing.
     * Relative threshold suppresses leakage from the adjacent FM carrier. */
    double threshold=fmax(1e-6,fm_power*.02);
    n->detected=n->selected?nicam_locked(n):n->power>threshold*(n->detected?.65:1.0);
}
int nicam_detected(const Nicam *n) { return n && n->detected; }
int nicam_locked(Nicam *n)
{
    if(!n) return 0;
    SDL_LockMutex(n->mutex); int aborted=n->abort; SDL_UnlockMutex(n->mutex);
    return !aborted && SDL_AtomicGet(&n->locked) && SDL_GetTicks()-(Uint32)SDL_AtomicGet(&n->last_good)<100;
}
unsigned nicam_overflows(Nicam *n) { return n?n->overflows:0; }
int nicam_active(const Nicam *n) { return n && n->selected; }
void nicam_finish(Nicam *n) { if(n) stop(n,0); }
void nicam_destroy(Nicam *n)
{
    if(!n) return;
    stop(n,1); SDL_FreeAudioStream(n->convert);
    SDL_DestroyCond(n->ready); SDL_DestroyMutex(n->mutex); free(n->queue); free(n);
}
