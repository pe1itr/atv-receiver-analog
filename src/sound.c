#include "sound.h"
#include <math.h>
#include "fm_math.h"
#include <stdlib.h>
#include <string.h>
#define PI 3.14159265358979323846
#define TAPS 257
struct Sound {
    double carrier, cutoff;
    double rate, cs, sn, c, s, output_phase;
    unsigned decimation, phase, pos, used;
    float alpha, re[4], im[4], old_re, old_im;
    float fir[TAPS], history[TAPS], output[256];
    double power, magnitude, frequency;
    float dc, deemphasis, gain;
    unsigned good;
    int valid, locked;
    SoundOutput sink;
    void *opaque;
};
int sound_enabled(double rate, double bandwidth)
{
    /* Leave room above 6 MHz for modulation and the video IF transition. */
    return rate>=14e6 && bandwidth>=14e6;
}
void sound_reset(Sound *s)
{
    double carrier=s->carrier, cutoff=s->cutoff;
    double rate=s->rate; SoundOutput sink=s->sink; void *opaque=s->opaque;
    memset(s,0,sizeof(*s)); s->rate=rate; s->sink=sink; s->opaque=opaque;
    s->carrier=carrier; s->cutoff=cutoff;
    s->c=1; s->cs=cos(2*PI*carrier/rate); s->sn=sin(2*PI*carrier/rate);
    s->decimation=(unsigned)floor(rate/500000);
    if(!s->decimation) s->decimation=1;
    s->alpha=1-exp(-2*PI*cutoff/rate);
    double intermediate=rate/s->decimation, sum=0;
    for(int k=0;k<TAPS;++k) {
        int x=k-TAPS/2;
        double sinc=x?sin(2*PI*16000/intermediate*x)/(PI*x):2*16000/intermediate;
        double window=.42-.5*cos(2*PI*k/(TAPS-1))+.08*cos(4*PI*k/(TAPS-1));
        s->fir[k]=sinc*window; sum+=s->fir[k];
    }
    for(int k=0;k<TAPS;++k) s->fir[k]/=sum;
}
static Sound *create_channel(double rate, double carrier, double cutoff, SoundOutput output, void *opaque)
{
    if(rate<14e6 || !isfinite(rate)) return NULL;
    Sound *s=calloc(1,sizeof(*s));
    if(s) { s->rate=rate; s->carrier=carrier; s->cutoff=cutoff; s->sink=output; s->opaque=opaque; sound_reset(s); }
    return s;
}
void sound_process(void *opaque, const float *video, size_t n)
{
    Sound *s=opaque;
    double intermediate=s->rate/s->decimation;
    const float fm_scale=intermediate/(2*PI*50000);
    const double stats=1-exp(-1/(intermediate*.020));
    for(size_t j=0;j<n;++j) {
        float v=isfinite(video[j])?video[j]:0;
        float re=v*s->c, im=-v*s->s;
        double c=s->c*s->cs-s->s*s->sn;
        s->s=s->s*s->cs+s->c*s->sn; s->c=c;
        /* Four real poles per quadrature before decimation. The narrow
         * channel rejects video/chroma and the negative-frequency image. */
        for(unsigned k=0;k<4;++k) {
            s->re[k]+=s->alpha*(re-s->re[k]);
            s->im[k]+=s->alpha*(im-s->im[k]);
            re=s->re[k]; im=s->im[k];
        }
        if(++s->phase<s->decimation) continue;
        s->phase=0;
        double power=re*re+im*im;
        s->power+=stats*(power-s->power);
        s->magnitude+=stats*(sqrt(power)-s->magnitude);
        float fm=0;
        if(s->valid) fm=fm_atan2f(im*s->old_re-re*s->old_im,re*s->old_re+im*s->old_im)*fm_scale;
        s->old_re=re; s->old_im=im; s->valid=1;
        s->frequency+=stats*(fm-s->frequency);
        /* Noise has a fluctuating envelope; an FM carrier is almost constant.
         * Hysteresis prevents rapid switching near the detection threshold. */
        int present=s->power>1e-6 && s->magnitude*s->magnitude>(s->locked?.83:.90)*s->power && fabs(s->frequency)<1.5;
        if(present) { if(s->good<(unsigned)(intermediate*.020)) ++s->good; }
        else if(s->good) --s->good;
        if(s->good>=(unsigned)(intermediate*.020)) s->locked=1;
        if(!s->good) s->locked=0;
        s->history[s->pos]=fm; s->pos=(s->pos+1)%TAPS;
        s->output_phase+=48000;
        if(s->output_phase<intermediate) continue;
        s->output_phase-=intermediate;
        /* Low-pass before resampling. Interpolate adjacent filtered instants
         * to retain an exact 48 kHz clock at arbitrary input sample rates. */
        double y=0, previous=0;
        unsigned at=(s->pos+TAPS-1)%TAPS;
        for(unsigned k=0;k<TAPS-1;++k) {
            y+=s->fir[k]*s->history[at];
            at=at?at-1:TAPS-1;
            previous+=s->fir[k]*s->history[at];
        }
        y+=(previous-y)*(s->output_phase/48000);
        s->dc+=.0039193f*(y-s->dc); /* 30 Hz DC blocker. */
        s->deemphasis+=.3407594f*(y-s->dc-s->deemphasis); /* 50 us. */
        /* Carrier detection is informational: keep weak signals and noise
         * audible while tuning. The playback mute remains user controlled. */
        s->gain+=.004f*(.7f-s->gain);
        s->output[s->used++]=fmaxf(-1,fminf(1,s->gain*s->deemphasis));
        if(s->used==256) {
            if(s->sink) s->sink(s->opaque,s->output,s->used);
            s->used=0;
        }
    }
    /* Do not lose the last partial audio packet at EOF or a small IQ block. */
    if(s->used && s->sink) s->sink(s->opaque,s->output,s->used);
    s->used=0;
}
double sound_power(const Sound *s) { return s?s->power:0; }
int sound_locked(const Sound *s) { return s && s->locked; }
void sound_destroy(Sound *s) { free(s); }

Sound *sound_create(double rate, SoundOutput output, void *opaque)
{
    return create_channel(rate,6e6,160000,output,opaque);
}
struct StereoSound {
    Sound *left, *right;
    SoundOutput sink;
    void *opaque;
    float left_pcm[256];
    int swap;
};
static void capture_left(void *opaque, const float *pcm, size_t n)
{
    StereoSound *s=opaque;
    memcpy(s->left_pcm,pcm,n*sizeof(float));
}
static void capture_right(void *opaque, const float *pcm, size_t n)
{
    StereoSound *s=opaque;
    float stereo[512];
    for(size_t k=0;k<n;++k) {
        stereo[2*k+s->swap]=s->left_pcm[k];
        stereo[2*k+1-s->swap]=pcm[k];
    }
    if(s->sink) s->sink(s->opaque,stereo,n);
}
int stereo_sound_enabled(double rate, double bandwidth)
{
    return rate>=16e6 && bandwidth>=16e6;
}
StereoSound *stereo_sound_create(double rate, SoundOutput output, void *opaque)
{
    if(!stereo_sound_enabled(rate,rate)) return NULL;
    StereoSound *s=calloc(1,sizeof(*s));
    if(!s) return NULL;
    s->sink=output; s->opaque=opaque;
    /* Narrower filters separate the carriers spaced only 180-200 kHz apart. */
    s->left=create_channel(rate,7.02e6,55000,capture_left,s);
    s->right=create_channel(rate,7.22e6,55000,capture_right,s);
    if(!s->left || !s->right) { stereo_sound_destroy(s); return NULL; }
    return s;
}
void stereo_sound_reset(StereoSound *s, int upper_pair, int swap)
{
    if(!s) return;
    s->left->carrier=upper_pair?7.38e6:7.02e6;
    s->right->carrier=upper_pair?7.56e6:7.22e6;
    sound_reset(s->left); sound_reset(s->right); s->swap=!!swap;
}
void stereo_sound_process(StereoSound *s, const float *samples, size_t n)
{
    /* Identical channel clocks; each chunk emits at most one packet (<256
     * frames), so the left packet is ready when the right callback runs. */
    while(n) {
        size_t chunk=n<4096?n:4096;
        sound_process(s->left,samples,chunk);
        sound_process(s->right,samples,chunk);
        samples+=chunk; n-=chunk;
    }
}
int stereo_sound_locked(const StereoSound *s)
{
    return s && sound_locked(s->left) && sound_locked(s->right);
}
void stereo_sound_destroy(StereoSound *s)
{
    if(s) { sound_destroy(s->left); sound_destroy(s->right); free(s); }
}
