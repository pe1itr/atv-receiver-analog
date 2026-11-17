#include "alignment.h"
#include "iq_dc_block.h"
#include "iq_power.h"
#include "clipping.h"
#include "settings.h"
#include <SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void check_settings(void)
{
    Settings s={.c={.mode="hackrf",.antenna="TX/RX",.ref="internal",.rate=1e7,.frequency=1487e6,
        .bandwidth=8e6,.if_bandwidth=8e6,.line_hz=15625,.rx_block=BLOCK,
        .status_port=8765},.stack_frames=3};
    char path[]="/tmp/atv-dc-settings-XXXXXX";
    int fd=mkstemp(path); assert(fd>=0); close(fd);
    for(int mode=0;mode<3;++mode) {
        s.c.iq_dc_block=mode; assert(settings_save(&s,path));
        Settings loaded={0}; assert(settings_load(&loaded,path,1));
        assert(loaded.c.iq_dc_block==mode);
        assert(iq_dc_block_enabled(&loaded.c)==(mode!=2));
    }
    assert(!unlink(path));
}

static void check_dc_state(void)
{
    IQ one[4096],split[4096];
    for(size_t j=0;j<4096;++j)
        one[j]=split[j]=(IQ){.03f+.02f*cos(j*.4),-.05f+.02f*sin(j*.4)};
    IQDCBlock a,b; iq_dc_block_reset(&a,1e7); iq_dc_block_reset(&b,1e7);
    /* Equal acquisition block, then arbitrary streaming boundaries. */
    iq_dc_block_process(&a,one,1024); iq_dc_block_process(&b,split,1024);
    iq_dc_block_process(&a,one+1024,3072);
    for(size_t j=1024;j<4096;j+=128) iq_dc_block_process(&b,split+j,128);
    for(size_t j=0;j<4096;++j) {
        assert(one[j].i==split[j].i && one[j].q==split[j].q);
    }
    iq_dc_block_reset(&a,1e7);
    IQ constant[256]; for(size_t j=0;j<256;++j) constant[j]=(IQ){.03f,-.05f};
    iq_dc_block_process(&a,constant,256);
    for(size_t j=0;j<256;++j) assert(fabs(constant[j].i)+fabs(constant[j].q)<1e-8);
    Config c={.mode="hackrf"}; assert(iq_dc_block_enabled(&c));
    c.mode="b210"; assert(!iq_dc_block_enabled(&c));
    c.iq_dc_block=1; assert(iq_dc_block_enabled(&c));
    c.mode="hackrf"; c.iq_dc_block=2; assert(!iq_dc_block_enabled(&c));
}

static void check_fm(int mode,int deemphasis,double bw)
{
    Alignment *clean=alignment_create_video(1e7,bw,4e6,0,BLOCK,0);
    Alignment *corrected=alignment_create_video(1e7,bw,4e6,0,BLOCK,0);
    Alignment *uncorrected=alignment_create_video(1e7,bw,4e6,0,BLOCK,0);
    assert(clean && corrected && uncorrected);
    alignment_set_dc_block(corrected,1);
    alignment_set_fm_mode(clean,mode,0); alignment_set_fm_mode(corrected,mode,0);
    alignment_set_fm_mode(uncorrected,mode,0);
    if(deemphasis) {
        assert(!alignment_set_deemphasis(clean)); assert(!alignment_set_deemphasis(corrected));
        assert(!alignment_set_deemphasis(uncorrected));
    }
    IQ *reference=malloc(BLOCK*sizeof(IQ)),*offset=malloc(BLOCK*sizeof(IQ)); assert(reference && offset);
    for(int epoch=0;epoch<2;++epoch) {
        /* Gain/tuning resets must discard the previous offset estimate,
         * while retaining the enabled correction and FM selection. */
        alignment_reset(clean,0); alignment_reset(corrected,0); alignment_reset(uncorrected,0);
        double good_error=0,bad_error=0; uint64_t tick=0; double phase=0;
        for(unsigned block=0;block<70;++block) {
            demo_generate(reference,BLOCK,1e7,4e6,&tick,&phase);
            for(size_t j=0;j<BLOCK;++j) {
                reference[j].i*=.02f; reference[j].q*=.02f;
                offset[j]=(IQ){reference[j].i+(epoch?-.04f:.03f),reference[j].q+(epoch?.02f:-.05f)};
            }
            assert(!alignment_submit(clean,reference,BLOCK));
            assert(!alignment_submit(corrected,offset,BLOCK));
            assert(!alignment_submit(uncorrected,offset,BLOCK));
            const float *r=alignment_wait(clean),*g=alignment_wait(corrected),*b=alignment_wait(uncorrected);
            if(block>=40) for(size_t j=0;j<BLOCK;++j) {
                good_error+=(g[j]-r[j])*(g[j]-r[j]); bad_error+=(b[j]-r[j])*(b[j]-r[j]);
            }
        }
        /* DC is excluded from the IF level, independent of FM correction. */
        assert(fabs(alignment_if_power(corrected,NULL)-alignment_if_power(uncorrected,NULL))<1e-9);
        assert(fabs(alignment_if_power(corrected,NULL)-alignment_if_power(clean,NULL))<.001);
        printf("FM%d, EQ %d, IF %.1f MHz, reset %d: corrected RMS %.6f, original %.6f\n",
            mode,deemphasis,bw/1e6,epoch,sqrt(good_error/(30*BLOCK)),sqrt(bad_error/(30*BLOCK)));
        fflush(stdout);
        /* The narrow sync IF intentionally removes much of this wide FM
         * test signal; require clear recovery, not a perfect full-band image. */
        assert(good_error<bad_error*(bw<8e6?.15:.01));
    }
    alignment_destroy(clean); alignment_destroy(corrected); alignment_destroy(uncorrected);
    free(reference); free(offset);
}

static double measured_tone(double amplitude,double dc,int blocker)
{
    const size_t n=16384;
    IQ *iq=malloc(n*sizeof(*iq)); assert(iq);
    /* Integer cycles: no finite-block leakage into the mean. */
    for(size_t j=0;j<n;++j) {
        double phase=2*PI*1024*j/n;
        iq[j]=(IQ){dc+amplitude*cos(phase),-.7*dc+amplitude*sin(phase)};
    }
    double expected=amplitude?20*log10(amplitude):-160;
    assert(fabs(iq_ac_power_dbfs(iq,n)-expected)<.001);
    Alignment *a=alignment_create_video(1e7,3e6,4e6,0,n,0); assert(a);
    alignment_set_dc_block(a,blocker);
    for(int block=0;block<64;++block) { assert(!alignment_submit(a,iq,n)); alignment_wait(a); }
    double power=alignment_if_power(a,NULL);
    alignment_destroy(a); free(iq); return power;
}

static void check_power(void)
{
    assert(isnan(iq_ac_power_dbfs(NULL,0)));
    /* The FFT IF filter can leave float roundoff on an otherwise constant
     * input. Reject the DC offset down to that numerical noise floor. */
    double dc_only=measured_tone(0,.5,0);
    assert(dc_only<-130);
    double clean=measured_tone(.001,0,0);
    double offset=measured_tone(.001,.5,0);
    double corrected=measured_tone(.001,.5,1);
    assert(fabs(clean+60)<.01);
    assert(fabs(offset-clean)<.001 && fabs(corrected-clean)<.001);
    double gain=measured_tone(.001*sqrt(10),.5,0);
    assert(fabs(gain-offset-10)<.001);
    printf("DC-free level: clean %.5f, offset %.5f, blocker %.5f, gain step %.5f dB\n",
        clean,offset,corrected,gain-offset);
}

static void check_clipping(void)
{
    Clipping c; clipping_init(&c,"hackrf");
    IQ rail={1,0},quiet={0,0};
    clipping_process(&c,&rail,1,0);
    for(uint32_t t=10;t<=500;t+=10) clipping_process(&c,&quiet,1,t);
    assert(c.clipped_samples==1 && !clipping_repeated(&c,500));
    clipping_init(&c,"hackrf");
    for(uint32_t t=0;t<=300;t+=10) clipping_process(&c,&rail,1,t);
    assert(clipping_repeated(&c,300));
    assert(!clipping_repeated(&c,800));
    assert(c.clipped_samples==31 && c.samples==31);
    clipping_process(&c,&rail,1,2000); assert(!clipping_repeated(&c,2000));
    clipping_init(&c,"hackrf");
    for(uint32_t j=0;j<=300;j+=10) clipping_process(&c,&rail,1,UINT32_MAX-150+j);
    assert(clipping_repeated(&c,149));
}

int main(void)
{
    assert(!SDL_Init(SDL_INIT_TIMER));
    check_dc_state(); check_clipping(); check_settings(); check_power();
    for(int fm=0;fm<2;++fm) for(int eq=0;eq<2;++eq) {
        check_fm(fm,eq,8e6); check_fm(fm,eq,3.2e6);
    }
    SDL_Quit(); puts("HackRF DSP: DC recovery, FM0/FM1, sync IF, EQ, resets, DC-free power and clipping windows OK");
    return 0;
}
