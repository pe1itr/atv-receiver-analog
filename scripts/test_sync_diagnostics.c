#define main receiver_main
#include "../src/main.c"
#undef main
#include <assert.h>

static void observed(App *a,uint32_t now,int h,int v)
{
    sync_diagnostics_frame(a,now,h,v);
    a->last_frame=now;
}
/* Alternating stronger interference hides the genuine group from a
 * strongest-only detector. Its phase changes by milliseconds every field. */
static void test_competing_vertical_groups(void)
{
    const int samples=80000;
    float video[80000];
    Decoder *d=malloc(sizeof(*d));
    assert(d);
    for(int mode=1;mode<=2;++mode) {
        decoder_init(d,4e6,4e6,0,NULL,NULL);
        d->free_sync=1;
        d->multi_candidate_sync=mode==2;
        for(int field=0;field<24;++field) {
            int interference=8000+16000*(field%3);
            memset(video,0,sizeof(video));
            for(int j=0;j<640;++j) {
                video[interference+j]=-.35f;
                if(field<16) video[56000+j]=-.15f;
            }
            decoder_process_video(d,video,NULL,samples);
            if(field==15) {
                if(mode==1) assert(d->vsync_updates==0);
                else {
                    assert(d->vsync_updates>=10);
                    assert(fabs(remainder((double)d->vsync_confirmed-56000,samples))<64);
                }
            }
        }
        if(mode==2) assert(d->tick-d->vsync_confirmed>6u*samples);
    }
    free(d);
}

/* Same continuous PAL timing at each input rate. Odd block lengths split
 * interpolation pairs. Check the grid and 8 us bin phase at every split. */
static void test_sync_sample_grid(void)
{
    const unsigned rates[]={10000000,20000000,9375000};
    float video[997];
    Decoder *d=malloc(sizeof(*d)); assert(d);
    for(unsigned k=0;k<sizeof(rates)/sizeof(*rates);++k) {
        unsigned rate=rates[k];
        decoder_init(d,rate,4e6,0,NULL,NULL);
        d->free_sync=1;
        uint64_t end=rate/5;
        while(d->tick<end) {
            size_t n=end-d->tick<997?(size_t)(end-d->tick):997;
            for(size_t j=0;j<n;++j) {
                double t=(d->tick+j)/(double)rate;
                double field=fmod(t,.020);
                video[j]=(field>=.005 && field<.005160) || fmod(t,64e-6)<4.7e-6 ? -.30f : 0;
            }
            decoder_process_video(d,video,NULL,n);
            uint64_t points=(d->tick-1)*4000000/rate;
            assert(fabs(d->fold_tick-points*(rate/4e6))<1e-6);
            assert(d->vsync_samples==points%32);
        }
        assert(d->vsync_updates>=5 && d->hsync_updates>=5);
        assert(fabs(remainder(d->vsync_confirmed/(double)rate-.005,.020))<16e-6);
        printf("PAL grid %.3f MS/s: H %u, V %u, %.4f Hz\n",
               rate/1e6,d->hsync_updates,d->vsync_updates,rate/d->vsync_period);
    }
    free(d);
}

int main(void)
{
    test_sync_sample_grid();
    test_competing_vertical_groups();
    assert(SDL_Init(SDL_INIT_TIMER)==0);
    App *a=calloc(1,sizeof(*a)); assert(a);
    a->mutex=SDL_CreateMutex(); assert(a->mutex);
    a->c.mode="demo"; a->c.rate=6e6;
    a->if_power_dbfs=NAN;
    assert(!strcmp(sync_indicator_reason(a,100),"no_video"));
    observed(a,100,0,0); /* Initial acquisition is not a loss. */
    observed(a,120,1,1);
    assert(!a->sync_losses);
    observed(a,140,1,0);
    assert(!strcmp(sync_indicator_reason(a,140),"no_v_sync"));
    observed(a,160,1,1);
    observed(a,180,0,1);
    assert(!strcmp(sync_indicator_reason(a,180),"no_h_sync"));
    observed(a,200,0,0); /* Losing V while already unlocked adds no combined loss. */
    assert(!strcmp(sync_indicator_reason(a,200),"no_hv_sync"));
    observed(a,220,1,1);
    assert(a->horizontal_losses==1 && a->vertical_losses==2 && a->sync_losses==2);
    assert(!strcmp(sync_indicator_reason(a,369),"locked"));
    assert(!strcmp(sync_indicator_reason(a,370),"stale_video"));
    observed(a,370,1,1); /* A frame pause need not lose decoder sync. */
    assert(a->frame_stalls==1 && a->max_frame_gap_ms==150 && a->sync_losses==2);
    observed(a,390,1,1);
    assert(a->frame_stalls==1 && a->max_frame_gap_ms==150);
    /* A discontinuity clears the timestamp before the next frame. */
    a->last_frame=0;
    observed(a,600000,1,1);
    assert(a->frame_stalls==1 && a->max_frame_gap_ms==150);
    /* SDL's millisecond clock wraps; short gaps must remain short. */
    a->last_frame=UINT32_MAX-9;
    observed(a,10,1,1);
    assert(a->frame_stalls==1 && a->max_frame_gap_ms==150);
    assert(!strcmp(sync_indicator_reason(a,159),"locked"));
    assert(!strcmp(sync_indicator_reason(a,160),"stale_video"));
    /* Recovery between 1 Hz snapshots must still be present in both exports. */
    char raw[8192],diagnostic[16384];
    int n=status_json(a,raw,sizeof(raw)); assert(n>0 && (size_t)n<sizeof(raw));
    assert(strstr(raw,"\"horizontal_losses\":1,\"vertical_losses\":2,\"combined_losses\":2"));
    assert(strstr(raw,"\"horizontal_age_fields\":null,\"vertical_age_ms\":null"));
    assert(strstr(raw,"\"completed_frame_stalls\":1,\"max_completed_frame_gap_ms\":150"));
    n=diagnostic_snapshot(a,diagnostic,sizeof(diagnostic));
    assert(n>0 && (size_t)n<sizeof(diagnostic));
    assert(strstr(diagnostic,"observed losses H=1 V=2 combined=2"));
    assert(strstr(diagnostic,"completed frame gaps >=150 ms=1 max=150 ms"));
    assert(strstr(diagnostic,"awaiting completed blocks"));
    a->diagnosis_previous_tick=1;
    uint64_t freq=SDL_GetPerformanceFrequency();
    a->timing=(BlockTiming){.blocks=2,.wait_ticks=freq/2,.read_overhead_ticks=freq/50,
        .processing_ticks=freq/4,.empty_reads=3,.pal_ticks=freq/8};
    n=diagnostic_snapshot(a,diagnostic,sizeof(diagnostic));
    assert(n>0 && strstr(diagnostic,"2 completed blocks"));
    assert(strstr(diagnostic,"empty queue wait=250.000"));
    assert(strstr(diagnostic,"including copy)=10.000"));
    assert(strstr(diagnostic,"including DSP waits)=125.000"));
    assert(strstr(diagnostic,"empty reads=3"));
    assert(strstr(diagnostic,"PAL decode=62.500"));
    diagnostic_snapshot(a,diagnostic,sizeof(diagnostic));
    assert(strstr(diagnostic,"awaiting completed blocks"));
    /* With flat demodulated video both detectors can explain the absent lock. */
    Decoder *d=calloc(1,sizeof(*d)); assert(d);
    decoder_init(d,5500000,4000000,0,NULL,NULL);
    d->free_sync=1;
    float flat[4096]={0};
    for(int i=0;i<80;++i) decoder_process_video(d,flat,NULL,4096);
    assert(d->sync_evidence.v_scans>=2 && !d->sync_evidence.v_candidates);
    assert(!strcmp(d->sync_evidence.v_result,"broad_pulse_too_shallow"));
    assert(!strcmp(d->sync_evidence.h_result,"fold_insufficient_contrast"));
    a->sync_evidence=d->sync_evidence;
    free(d);
    n=diagnostic_snapshot(a,diagnostic,sizeof(diagnostic));
    assert(n>0 && (size_t)n<sizeof(diagnostic));
    assert(strstr(diagnostic,"latest V scan: broad_pulse_too_shallow"));
    assert(strstr(diagnostic,"\"v_decision\":\"broad_pulse_too_shallow\""));

    /* Even without a readable renderer F10 saves the actual video pixels. */
    ScreenshotResult shot={0};
    a->pixels[0]=0xff123456;
    assert(!save_window_png(NULL,1252e6,"/tmp/",a->pixels,&shot));
    assert(*shot.capture_issue && !*shot.error);
    png_image png={0}; png.version=PNG_IMAGE_VERSION;
    assert(png_image_begin_read_from_file(&png,shot.path));
    assert(png.width==WIDTH && png.height==HEIGHT);
    png.format=PNG_FORMAT_RGBA;
    unsigned char *rgba=malloc(PNG_IMAGE_SIZE(png)); assert(rgba);
    assert(png_image_finish_read(&png,NULL,rgba,0,NULL));
    assert(rgba[0]==0x12 && rgba[1]==0x34 && rgba[2]==0x56);
    free(rgba); png_image_free(&png); assert(!unlink(shot.path));
    SDL_DestroyMutex(a->mutex); free(a); SDL_Quit();
    puts("Sync diagnostics: brief H/V losses, frame stalls, export and clock wrap OK");
}
