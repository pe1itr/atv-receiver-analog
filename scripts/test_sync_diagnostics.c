#define main receiver_main
#include "../src/main.c"
#undef main
#include <assert.h>

static void observed(App *a,uint32_t now,int h,int v)
{
    sync_diagnostics_frame(a,now,h,v);
    a->last_frame=now;
}
int main(void)
{
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
    SDL_DestroyMutex(a->mutex); free(a); SDL_Quit();
    puts("Sync diagnostics: brief H/V losses, frame stalls, export and clock wrap OK");
}
