#include "alignment.h"
#include "pal_pipeline.h"
#include "test_signal.h"
#include <SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_colour_blocks(void)
{
    const double rate=12e6;
    TestSignal *signal=malloc(sizeof(*signal));
    Decoder *serial=malloc(sizeof(*serial)),*parallel=malloc(sizeof(*parallel));
    PalPipeline *pipeline=pal_pipeline_create();
    IQ *iq=malloc(BLOCK*sizeof(*iq));
    float *video=malloc(BLOCK*sizeof(*video));
    assert(signal && serial && parallel && pipeline && iq && video);
    for(int epoch=0;epoch<2;++epoch) {
        test_signal_init(signal,(TestSignalConfig){.rate=rate,.deviation=4e6,.amplitude=.5,.color=1});
        decoder_init(serial,rate,4e6,0,NULL,NULL);
        decoder_init(parallel,rate,4e6,0,NULL,NULL);
        serial->color=parallel->color=serial->free_sync=parallel->free_sync=1;
        pal_init(&serial->pal,rate); pal_init(&parallel->pal,rate);
        FMDemod fm;
        fm_demod_init(&fm,rate,4e6,0,1); fm.alpha=1;
        const size_t sizes[]={1,BLOCK,8191,8192,8193,17};
        for(size_t i=0;i<240;++i) {
            size_t n=sizes[i%6];
            test_signal_generate(signal,iq,n);
            fm_demod_process(&fm,iq,video,n);
            decoder_process_video(serial,video,NULL,n);
            pal_pipeline_decode(pipeline,parallel,video,NULL,n);
            assert(serial->tick==parallel->tick && serial->fields==parallel->fields);
            assert(serial->pal.last_lock==parallel->pal.last_lock);
            assert(serial->pal.burst_updates==parallel->pal.burst_updates);
            assert(!memcmp(serial->pixels,parallel->pixels,sizeof(serial->pixels)));
        }
        assert(serial->pal.burst_updates && serial->fields);
    }
    pal_pipeline_destroy(pipeline);
    free(signal); free(serial); free(parallel); free(iq); free(video);
}

static void test_filter_queue(void)
{
    Alignment *ref=alignment_create_video(12e6,10e6,4e6,0,BLOCK,1);
    Alignment *queue=alignment_create_video(12e6,10e6,4e6,0,BLOCK,1);
    IQ *iq=malloc(BLOCK*sizeof(*iq));
    float *expected=malloc(BLOCK*sizeof(*expected));
    uint64_t tick=0; double phase=0;
    size_t previous=0;
    assert(ref && queue && iq && expected);
    for(unsigned i=0;i<50;++i) {
        size_t n=i%2?BLOCK:BLOCK-113;
        demo_generate(iq,n,12e6,4e6,&tick,&phase);
        assert(!alignment_queue(queue,iq,n));
        if(previous) assert(!memcmp(alignment_previous(queue,NULL,NULL),expected,previous*sizeof(float)));
        assert(!alignment_submit(ref,iq,n));
        memcpy(expected,alignment_wait(ref),n*sizeof(float));
        previous=n;
    }
    assert(!memcmp(alignment_wait(queue),expected,previous*sizeof(float)));
    alignment_destroy(queue); alignment_destroy(ref);
    free(iq); free(expected);
}

int main(void)
{
    assert(!SDL_Init(SDL_INIT_TIMER));
    test_colour_blocks();
    test_filter_queue();
    SDL_Quit();
    puts("PAL pipeline: identical pixels and burst across chunks and resets; ordered IF results OK");
}
