#include "atv.h"
#include <fftw3.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#define N 4096
#define TAPS 257
#define OVERLAP (TAPS-1)
#define HOP (N-OVERLAP)
struct IFFilter {
    fftwf_complex *time, *freq, *response;
    fftwf_plan forward, inverse;
    IQ history[OVERLAP];
    float *real_time, real_history[OVERLAP];
    fftwf_complex *real_freq;
    fftwf_plan real_forward, real_inverse;
};
IFFilter *if_filter_create(double rate, double bandwidth)
{
    IFFilter *f=calloc(1,sizeof(*f));
    if (!f) return NULL;
    f->time=fftwf_alloc_complex(N); f->freq=fftwf_alloc_complex(N); f->response=fftwf_alloc_complex(N);
    if (!f->time || !f->freq || !f->response) goto fail;
    f->forward=fftwf_plan_dft_1d(N,f->time,f->freq,FFTW_FORWARD,FFTW_ESTIMATE);
    f->inverse=fftwf_plan_dft_1d(N,f->freq,f->time,FFTW_BACKWARD,FFTW_ESTIMATE);
    if (!f->forward || !f->inverse) goto fail;
    memset(f->time,0,N*sizeof(*f->time));
    double sum=0, cutoff=fmin(.49,bandwidth/(2*rate));
    for(int j=0;j<TAPS;++j) {
        int k=j-OVERLAP/2;
        double h=k ? sin(2*PI*cutoff*k)/(PI*k) : 2*cutoff;
        double w=.42-.5*cos(2*PI*j/(TAPS-1))+.08*cos(4*PI*j/(TAPS-1));
        f->time[j][0]=h*w; sum+=h*w;
    }
    for(int j=0;j<TAPS;++j) f->time[j][0]/=sum;
    fftwf_execute(f->forward);
    memcpy(f->response,f->freq,N*sizeof(*f->freq));
    return f;
fail: if_filter_destroy(f); return NULL;
}
IFFilter *video_deemphasis_create(double rate)
{
    IFFilter *f=if_filter_create(rate,rate);
    if(!f) return NULL;
    /* Digital Baseband uses linear-phase emphasis. Sample the inverse
     * F.405-1 625-line magnitude, then window its symmetric impulse response.
     * A=11 dB, B=.4083, C=10.21; frequency is expressed in MHz. */
    for(int k=0;k<N;++k) {
        double mhz=(k<=N/2?k:N-k)*rate/N/1e6;
        f->freq[k][0]=pow(10,11.0/20)*sqrt((1+.4083*mhz*mhz)/(1+10.21*mhz*mhz));
        f->freq[k][1]=0;
    }
    fftwf_execute(f->inverse);
    float taps[TAPS]; double sum=0;
    for(int j=0;j<TAPS;++j) {
        int k=j-VIDEO_EQ_DELAY;
        double window=.42-.5*cos(2*PI*j/(TAPS-1))+.08*cos(4*PI*j/(TAPS-1));
        taps[j]=f->time[(k+N)%N][0]/N*window; sum+=taps[j];
    }
    memset(f->time,0,N*sizeof(*f->time));
    for(int j=0;j<TAPS;++j) f->time[j][0]=taps[j]*pow(10,11.0/20)/sum;
    fftwf_execute(f->forward);
    memcpy(f->response,f->freq,N*sizeof(*f->freq));
    /* Same FIR and delay, but real input needs only N/2+1 spectral bins.
     * Keep the complex entry point for callers filtering IQ and regression
     * comparisons; the video worker uses the real entry point below. */
    f->real_time=fftwf_alloc_real(N);
    f->real_freq=fftwf_alloc_complex(N/2+1);
    if(!f->real_time || !f->real_freq) goto fail;
    f->real_forward=fftwf_plan_dft_r2c_1d(N,f->real_time,f->real_freq,FFTW_ESTIMATE);
    f->real_inverse=fftwf_plan_dft_c2r_1d(N,f->real_freq,f->real_time,FFTW_ESTIMATE);
    if(!f->real_forward || !f->real_inverse) goto fail;
    return f;
fail: if_filter_destroy(f); return NULL;
}
void if_filter_reset(IFFilter *f)
{
    memset(f->history,0,sizeof(f->history));
    memset(f->real_history,0,sizeof(f->real_history));
}
void video_deemphasis_process(IFFilter *f, float *video, size_t count)
{
    while(count) {
        size_t n=count<HOP?count:HOP;
        memcpy(f->real_time,f->real_history,sizeof(f->real_history));
        memcpy(f->real_time+OVERLAP,video,n*sizeof(float));
        memset(f->real_time+OVERLAP+n,0,(N-OVERLAP-n)*sizeof(float));
        if(n>=OVERLAP) memcpy(f->real_history,video+n-OVERLAP,sizeof(f->real_history));
        else {
            memmove(f->real_history,f->real_history+n,(OVERLAP-n)*sizeof(float));
            memcpy(f->real_history+OVERLAP-n,video,n*sizeof(float));
        }
        fftwf_execute(f->real_forward);
        for(int j=0;j<=N/2;++j) {
            float re=f->real_freq[j][0], im=f->real_freq[j][1];
            f->real_freq[j][0]=re*f->response[j][0]-im*f->response[j][1];
            f->real_freq[j][1]=re*f->response[j][1]+im*f->response[j][0];
        }
        fftwf_execute(f->real_inverse);
        for(size_t j=0;j<n;++j) video[j]=f->real_time[j+OVERLAP]/N;
        video+=n; count-=n;
    }
}
void if_filter_process(IFFilter *f, IQ *iq, size_t count)
{
    while(count) {
        size_t n=count<HOP?count:HOP;
        for(int j=0;j<OVERLAP;++j) { f->time[j][0]=f->history[j].i; f->time[j][1]=f->history[j].q; }
        for(size_t j=0;j<n;++j) { f->time[OVERLAP+j][0]=iq[j].i; f->time[OVERLAP+j][1]=iq[j].q; }
        memset(f->time+OVERLAP+n,0,(N-OVERLAP-n)*sizeof(*f->time));
        if(n>=OVERLAP) memcpy(f->history,iq+n-OVERLAP,sizeof(f->history));
        else { memmove(f->history,f->history+n,(OVERLAP-n)*sizeof(IQ)); memcpy(f->history+OVERLAP-n,iq,n*sizeof(IQ)); }
        fftwf_execute(f->forward);
        for(int j=0;j<N;++j) {
            float re=f->freq[j][0], im=f->freq[j][1];
            f->freq[j][0]=re*f->response[j][0]-im*f->response[j][1];
            f->freq[j][1]=re*f->response[j][1]+im*f->response[j][0];
        }
        fftwf_execute(f->inverse);
        for(size_t j=0;j<n;++j) { iq[j].i=f->time[j+OVERLAP][0]/N; iq[j].q=f->time[j+OVERLAP][1]/N; }
        iq+=n; count-=n;
    }
}
void if_filter_destroy(IFFilter *f)
{
    if(!f) return;
    if(f->forward) fftwf_destroy_plan(f->forward);
    if(f->inverse) fftwf_destroy_plan(f->inverse);
    if(f->real_forward) fftwf_destroy_plan(f->real_forward);
    if(f->real_inverse) fftwf_destroy_plan(f->real_inverse);
    fftwf_free(f->real_time); fftwf_free(f->real_freq);
    fftwf_free(f->time); fftwf_free(f->freq); fftwf_free(f->response); free(f);
}
