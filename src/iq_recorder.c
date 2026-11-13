#include "iq_recorder.h"
#include "version.h"
#include <SDL.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "platform.h"

struct IQRecorder {
    SDL_mutex *mutex;
    SDL_cond *changed;
    SDL_Thread *thread;
    IQRecordSettings settings;
    IQRecordStatus status;
    IQ *samples;
    int finish, shutdown;
};

static int create_files(const Config *c,char *path,char *metadata,FILE **raw,FILE **json)
{
    struct timespec now; struct tm utc; char stamp[32];
    if(clock_gettime(CLOCK_REALTIME,&now) || !gmtime_r(&now.tv_sec,&utc) ||
       !strftime(stamp,sizeof(stamp),"%Y%m%dT%H%M%S",&utc)) return 0;
    for(unsigned suffix=0;suffix<1000;++suffix) {
        snprintf(path,192,"fm-atv-%.0fHz-%s-%03ldZ-%03u.cf32",
            c->frequency+c->lo,stamp,now.tv_nsec/1000000,suffix);
        snprintf(metadata,208,"%s.json",path);
        *raw=fopen(path,"wbx");
        if(!*raw) { if(errno==EEXIST) continue; return 0; }
        *json=fopen(metadata,"wbx");
        if(*json) return 1;
        int error=errno;
        fclose(*raw); *raw=NULL; remove(path);
        if(error!=EEXIST) { errno=error; return 0; }
    }
    errno=EEXIST; return 0;
}
static int save_metadata(FILE *f,const IQRecordSettings *s,const IQRecordStatus *status)
{
    const Config *c=&s->c;
#ifdef _WIN32
    const char *program="atv-rx.exe";
#else
    const char *program="./build/atv-rx";
#endif
    return fprintf(f,
        "{\n  \"format\": \"cf32-native\",\n  \"version\": \"%s\",\n"
        "  \"samples\": %zu, \"duration_s\": %.9g, \"end_reason\": \"%s\",\n"
        "  \"source\": \"%s\", \"frequency_hz\": %.12g, \"sdr_frequency_hz\": %.12g,\n"
        "  \"converter_lo_hz\": %.12g, \"sample_rate_sps\": %.12g,\n"
        "  \"rf_bandwidth_hz\": %.12g, \"gain_db\": %.12g, \"hackrf_vga_db\": %.12g,\n"
        "  \"if_bandwidth_hz\": %.12g, \"alignment_bandwidth_hz\": %.12g,\n"
        "  \"fm_mode\": %d, \"deviation_hz\": %.12g, \"pal_deemphasis\": %s,\n"
        "  \"invert\": %s, \"free_sync\": %s, \"experimental_sync\": %s,\n"
        "  \"line_hz\": %.12g, \"vertical_shift_lines\": %d, \"audio_source\": %d,\n"
        "  \"playback_command\": \"%s --no-config --no-wizard --file %s"
        " --rate %.12g --frequency %.12g --lo %.12g --bandwidth %.12g"
        " --if-bandwidth %.12g --alignment-bandwidth %.12g --deviation %.12g"
        " --fm-mode %d --video-deemphasis %s --line-hz %.12g%s%s%s\"\n}\n",
        ATV_VERSION,status->count,status->count/c->rate,status->reason,
        c->mode,c->frequency+c->lo,c->frequency,c->lo,c->rate,
        c->bandwidth,c->gain,c->hackrf_vga,c->if_bandwidth,c->alignment_bandwidth,
        c->fm_mode,c->deviation,c->video_deemphasis?"true":"false",
        c->invert?"true":"false",c->free_sync?"true":"false",c->experimental_sync?"true":"false",
        c->line_hz,s->vertical_shift,s->audio_source,program,status->path,
        c->rate,c->frequency+c->lo,c->lo,c->bandwidth,c->if_bandwidth,c->alignment_bandwidth,
        c->deviation,c->fm_mode,c->video_deemphasis?"pal":"off",c->line_hz,
        c->invert?" --invert":"",c->free_sync?" --free-sync":"",
        c->experimental_sync?" --experimental-sync":"")>=0;
}
static int writer(void *opaque)
{
    IQRecorder *r=opaque;
    SDL_LockMutex(r->mutex);
    while(!r->shutdown) {
        while(r->status.state!=IQ_REC_PREPARING && !r->shutdown)
            SDL_CondWait(r->changed,r->mutex);
        if(r->shutdown) break;
        IQRecordSettings settings=r->settings;
        size_t capacity=r->status.capacity;
        SDL_UnlockMutex(r->mutex);
        FILE *raw=NULL,*json=NULL;
        char path[192]="",metadata[208]="",error[160]="";
        IQ *samples=NULL;
        if(!create_files(&settings.c,path,metadata,&raw,&json))
            snprintf(error,sizeof(error),"Cannot create IQ recording: %s",strerror(errno));
        else {
            samples=malloc(capacity*sizeof(*samples));
            if(!samples) snprintf(error,sizeof(error),"Cannot allocate IQ capture buffer");
            else memset(samples,0,capacity*sizeof(*samples));
        }
        SDL_LockMutex(r->mutex);
        r->samples=samples;
        snprintf(r->status.path,sizeof(r->status.path),"%s",path);
        if(samples && !r->finish && !r->shutdown) {
            r->status.state=IQ_REC_CAPTURING;
            while(!r->finish && !r->shutdown) SDL_CondWait(r->changed,r->mutex);
        }
        IQRecordStatus done=r->status;
        r->status.state=IQ_REC_SAVING;
        r->samples=NULL;
        SDL_UnlockMutex(r->mutex);
        if(done.count) {
            if(fwrite(samples,sizeof(*samples),done.count,raw)!=done.count)
                snprintf(error,sizeof(error),"Cannot write IQ recording: %s",strerror(errno));
            if(fclose(raw)) snprintf(error,sizeof(error),"Cannot close IQ recording: %s",strerror(errno));
            raw=NULL;
            if(!*error && !save_metadata(json,&settings,&done))
                snprintf(error,sizeof(error),"Cannot write IQ metadata: %s",strerror(errno));
        }
        if(raw && fclose(raw)) snprintf(error,sizeof(error),"Cannot close IQ recording: %s",strerror(errno));
        if(json && fclose(json)) snprintf(error,sizeof(error),"Cannot close IQ metadata: %s",strerror(errno));
        if(!done.count) { if(raw) remove(path); if(json) remove(metadata); }
        else if(*error) remove(metadata); /* Never describe a truncated IQ file as complete. */
        free(samples);
        if(*error) fprintf(stderr,"IQ recording: %s\n",error);
        else if(done.count) fprintf(stderr,"IQ recording saved: %s (%zu samples, %.3f s; %s); settings: %s\n",
            path,done.count,done.count/settings.c.rate,done.reason,metadata);
        SDL_LockMutex(r->mutex);
        r->status.updated=SDL_GetTicks();
        r->status.state=*error?IQ_REC_ERROR:done.count?IQ_REC_SAVED:IQ_REC_IDLE;
        snprintf(r->status.error,sizeof(r->status.error),"%s",error);
    }
    SDL_UnlockMutex(r->mutex);
    return 0;
}
IQRecorder *iq_recorder_create(void)
{
    IQRecorder *r=calloc(1,sizeof(*r));
    if(!r) return NULL;
    r->mutex=SDL_CreateMutex(); r->changed=SDL_CreateCond();
    if(r->mutex && r->changed) r->thread=SDL_CreateThread(writer,"IQ-save",r);
    if(!r->thread) {
        if(r->changed) SDL_DestroyCond(r->changed);
        if(r->mutex) SDL_DestroyMutex(r->mutex);
        free(r); return NULL;
    }
    return r;
}
void iq_recorder_toggle(IQRecorder *r,const IQRecordSettings *settings)
{
    if(!r) return;
    SDL_LockMutex(r->mutex);
    if(r->status.state==IQ_REC_PREPARING || r->status.state==IQ_REC_CAPTURING) {
        r->finish=1; snprintf(r->status.reason,sizeof(r->status.reason),"user_stop");
        if(r->status.state==IQ_REC_CAPTURING) r->status.state=IQ_REC_SAVING;
    } else if(r->status.state!=IQ_REC_SAVING) {
        memset(&r->status,0,sizeof(r->status));
        double samples=settings->c.rate*settings->c.record_seconds;
        if(!isfinite(samples) || settings->c.rate<=0 || settings->c.record_seconds<.001 ||
           settings->c.record_seconds>15 || samples*sizeof(IQ)>2147483648.0) {
            r->status.state=IQ_REC_ERROR;
            snprintf(r->status.error,sizeof(r->status.error),"Capture limit: 15 seconds / 2 GiB; lower --record-seconds");
        } else {
            r->settings=*settings; r->finish=0;
            r->status.capacity=(size_t)llround(samples); r->status.rate=settings->c.rate;
            r->status.state=IQ_REC_PREPARING;
            snprintf(r->status.reason,sizeof(r->status.reason),"complete");
        }
        r->status.updated=SDL_GetTicks();
    }
    SDL_CondSignal(r->changed); SDL_UnlockMutex(r->mutex);
}
void iq_recorder_push(IQRecorder *r,const IQ *iq,size_t count)
{
    if(!r) return;
    SDL_LockMutex(r->mutex);
    if(r->status.state==IQ_REC_CAPTURING) {
        size_t take=r->status.capacity-r->status.count;
        if(take>count) take=count;
        memcpy(r->samples+r->status.count,iq,take*sizeof(*iq)); r->status.count+=take;
        if(r->status.count==r->status.capacity) {
            r->finish=1; r->status.state=IQ_REC_SAVING; SDL_CondSignal(r->changed);
        }
    }
    SDL_UnlockMutex(r->mutex);
}
void iq_recorder_finish(IQRecorder *r,const char *reason)
{
    if(!r) return;
    SDL_LockMutex(r->mutex);
    if(r->status.state==IQ_REC_PREPARING || r->status.state==IQ_REC_CAPTURING) {
        r->finish=1;
        if(r->status.state==IQ_REC_CAPTURING) r->status.state=IQ_REC_SAVING;
        snprintf(r->status.reason,sizeof(r->status.reason),"%s",reason);
        SDL_CondSignal(r->changed);
    }
    SDL_UnlockMutex(r->mutex);
}
void iq_recorder_status(IQRecorder *r,IQRecordStatus *status)
{
    memset(status,0,sizeof(*status));
    if(!r) return;
    SDL_LockMutex(r->mutex); *status=r->status; SDL_UnlockMutex(r->mutex);
}
void iq_recorder_destroy(IQRecorder *r)
{
    if(!r) return;
    iq_recorder_finish(r,"shutdown");
    SDL_LockMutex(r->mutex); r->shutdown=1; SDL_CondSignal(r->changed); SDL_UnlockMutex(r->mutex);
    SDL_WaitThread(r->thread,NULL);
    SDL_DestroyCond(r->changed); SDL_DestroyMutex(r->mutex); free(r);
}
