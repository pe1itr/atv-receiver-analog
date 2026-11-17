#include "diagnostic_log.h"
#include "version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "platform.h"

typedef struct {
    double wall_seconds,process_seconds;
    uint64_t system_total,system_idle;
    int process_valid,system_valid;
} CpuSample;

static CpuSample read_cpu_sample(void)
{
    CpuSample sample={0};
    sample.wall_seconds=(double)SDL_GetPerformanceCounter()/SDL_GetPerformanceFrequency();
#ifdef _WIN32
    FILETIME created,exited,kernel,user,idle;
    if(GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)) {
        uint64_t k=((uint64_t)kernel.dwHighDateTime<<32)|kernel.dwLowDateTime;
        uint64_t u=((uint64_t)user.dwHighDateTime<<32)|user.dwLowDateTime;
        sample.process_seconds=(k+u)*1e-7; sample.process_valid=1;
    }
    if(GetSystemTimes(&idle,&kernel,&user)) {
        uint64_t k=((uint64_t)kernel.dwHighDateTime<<32)|kernel.dwLowDateTime;
        uint64_t u=((uint64_t)user.dwHighDateTime<<32)|user.dwLowDateTime;
        sample.system_total=k+u; /* Windows kernel time includes idle time. */
        sample.system_idle=((uint64_t)idle.dwHighDateTime<<32)|idle.dwLowDateTime;
        sample.system_valid=1;
    }
#else
    struct timespec process;
    if(!clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&process)) {
        sample.process_seconds=process.tv_sec+process.tv_nsec*1e-9;
        sample.process_valid=1;
    }
#ifdef __linux__
    FILE *fp=fopen("/proc/stat","r");
    if(fp) {
        char line[512];
        unsigned long long user,nice,system,idle,iowait,irq,softirq,steal;
        if(fgets(line,sizeof(line),fp) &&
           sscanf(line,"cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &user,&nice,&system,&idle,&iowait,&irq,&softirq,&steal)==8) {
            /* Guest counters repeat time already included in user/nice. */
            sample.system_total=user+nice+system+idle+iowait+irq+softirq+steal;
            sample.system_idle=idle+iowait; sample.system_valid=1;
        }
        fclose(fp);
    }
#endif
#endif
    return sample;
}

static int format_cpu_usage(char *out,size_t capacity,const CpuSample *previous,const CpuSample *current,int cpus)
{
    char process[160]="unavailable (first sample or CPU timer unavailable)";
    char system[96]="unavailable (first sample or system counters unavailable)";
    double interval=previous?current->wall_seconds-previous->wall_seconds:0;
    if(previous && interval>0 && previous->process_valid && current->process_valid &&
       current->process_seconds>=previous->process_seconds && cpus>0) {
        double cores=(current->process_seconds-previous->process_seconds)/interval;
        snprintf(process,sizeof(process),"%.3f cores; %.2f%% of one logical CPU; %.2f%% of %d logical CPUs",
            cores,100*cores,100*cores/cpus,cpus);
    }
    if(previous && previous->system_valid && current->system_valid &&
       current->system_total>previous->system_total && current->system_idle>=previous->system_idle) {
        uint64_t total=current->system_total-previous->system_total;
        uint64_t idle=current->system_idle-previous->system_idle;
        if(idle<=total) snprintf(system,sizeof(system),"%.2f%%",100.0*(total-idle)/total);
    }
    return snprintf(out,capacity,"CPU: logical CPUs=%d; interval=%.3f s\n"
        "Receiver process CPU (all threads): %s\nSystem CPU busy (includes other programs): %s\n",
        cpus,interval>0?interval:0,process,system);
}

/* The UI copies at most 31 scalar snapshots. Disk I/O only runs on the
 * writer thread, without holding any receiver mutex or accessing the SDR. */
#define SNAPSHOT_SIZE 12288
struct DiagnosticLog {
    DiagnosticSnapshot snapshot;
    void *opaque;
    SDL_atomic_t state;
    SDL_Thread *writer;
    Uint32 started,next;
    unsigned count;
    CpuSample previous_cpu;
    const char *reason;
    char directory[4096],path[4352],error[256],screenshot[256];
    char samples[DIAGNOSTIC_SECONDS+1][SNAPSHOT_SIZE];
};
static int save_log(void *opaque)
{
    DiagnosticLog *log=opaque;
    time_t now=time(NULL); struct tm utc; char stamp[40];
    gmtime_r(&now,&utc); strftime(stamp,sizeof(stamp),"%Y%m%d-%H%M%SZ",&utc);
    int fd=-1;
    for(unsigned i=0;i<1000;++i) {
        snprintf(log->path,sizeof(log->path),"%satv-diagnose-%s-%03u.txt",log->directory,stamp,i);
#ifdef _WIN32
        fd=_open(log->path,_O_WRONLY|_O_CREAT|_O_EXCL|_O_BINARY,_S_IREAD|_S_IWRITE);
#else
        fd=open(log->path,O_WRONLY|O_CREAT|O_EXCL,0600);
#endif
        if(fd>=0 || errno!=EEXIST) break;
    }
    if(fd<0) goto fail;
    FILE *fp=fdopen(fd,"w");
    if(!fp) { int err=errno; close(fd); errno=err; goto fail; }
    int ok=fprintf(fp,"FM ATV diagnostic recording\nVersion: %s; commit: %s; build UTC: %s\nPlatform: %s\n"
        "Duration: %.3f seconds; snapshots: %u; end: %s\n"
        "Snapshots approximately once per second; short events may fall between snapshots.\n"
        "IF level is signal plus noise in dBFS, not dBm or C/N.\n"
        "Invalid Video-SNR is unavailable, never zero dB.\n"
        "Throughput is samples consumed by the receiver, not proof of source continuity.\n"
        "CPU usage is averaged between snapshots. One logical CPU fully used = 1.0 cores = 100%% of one CPU.\n"
        "Process total-capacity percentage divides by SDL's logical CPU count; system busy includes other programs.\n"
        "On Linux, system busy excludes idle and I/O-wait time. CPU averages do not identify individual busy threads.\n"
        "AFC SEEK is not a reliable tuning-error estimate.\n"
        "Contains receiver settings and diagnostics; no IQ or decoded audio.\n"
        "Initial screenshot (separate PNG, send together with this TXT): %s\n\n",
        ATV_VERSION,ATV_GIT_COMMIT,ATV_BUILD_UTC,SDL_GetPlatform(),(log->next-log->started)/1000.0,
        log->count,log->reason,log->screenshot)>=0;
    for(unsigned i=0;i<log->count && ok;++i) ok=fputs(log->samples[i],fp)>=0;
    if(fflush(fp)) ok=0;
    int err=errno;
    if(fclose(fp)) { ok=0; err=errno; }
    if(!ok) { errno=err?err:EIO; goto fail; }
    SDL_AtomicSet(&log->state,DIAG_SAVED); return 0;
fail:
    snprintf(log->error,sizeof(log->error),"Cannot save diagnosis: %s",strerror(errno));
    SDL_AtomicSet(&log->state,DIAG_ERROR); return 1;
}
DiagnosticLog *diagnostic_log_create(DiagnosticSnapshot snapshot,void *opaque)
{
    DiagnosticLog *log=calloc(1,sizeof(*log));
    if(log) { log->snapshot=snapshot; log->opaque=opaque; }
    return log;
}
int diagnostic_log_state(DiagnosticLog *log) { return log?SDL_AtomicGet(&log->state):DIAG_ERROR; }
unsigned diagnostic_log_remaining(DiagnosticLog *log)
{
    Uint32 elapsed=SDL_GetTicks()-log->started;
    return elapsed>=DIAGNOSTIC_SECONDS*1000?0:DIAGNOSTIC_SECONDS-elapsed/1000;
}
const char *diagnostic_log_path(DiagnosticLog *log) { return log?log->path:""; }
const char *diagnostic_log_error(DiagnosticLog *log) { return log?log->error:"Cannot allocate diagnosis recorder"; }
void diagnostic_log_screenshot(DiagnosticLog *log,const char *path,int saved)
{
    if(log && diagnostic_log_state(log)==DIAG_RECORDING)
        snprintf(log->screenshot,sizeof(log->screenshot),"%s",saved?path:"FAILED - screenshot could not be saved");
}
static void capture(DiagnosticLog *log)
{
    if(log->count>=DIAGNOSTIC_SECONDS+1) return;
    char *out=log->samples[log->count];
    int n=snprintf(out,SNAPSHOT_SIZE,"--- Elapsed %.3f s ---\n",(SDL_GetTicks()-log->started)/1000.0);
    CpuSample cpu=read_cpu_sample();
    n+=format_cpu_usage(out+n,SNAPSHOT_SIZE-(size_t)n,log->count?&log->previous_cpu:NULL,&cpu,SDL_GetCPUCount());
    log->previous_cpu=cpu;
    int used=log->snapshot(log->opaque,out+n,SNAPSHOT_SIZE-(size_t)n);
    if(used<=0 || (size_t)used>=SNAPSHOT_SIZE-(size_t)n)
        snprintf(out+n,SNAPSHOT_SIZE-(size_t)n,"Snapshot unavailable or too large\n\n");
    ++log->count;
}
static void finish(DiagnosticLog *log,const char *reason)
{
    capture(log); log->reason=reason; log->next=SDL_GetTicks();
    SDL_AtomicSet(&log->state,DIAG_SAVING);
    log->writer=SDL_CreateThread(save_log,"Diagnostic-file",log);
    if(!log->writer) {
        snprintf(log->error,sizeof(log->error),"Cannot start diagnosis writer: %s",SDL_GetError());
        SDL_AtomicSet(&log->state,DIAG_ERROR);
    }
}
void diagnostic_log_poll(DiagnosticLog *log)
{
    if(!log) return;
    int state=diagnostic_log_state(log);
    if(state==DIAG_RECORDING) {
        Uint32 now=SDL_GetTicks();
        if(now-log->started>=DIAGNOSTIC_SECONDS*1000) finish(log,"30 seconds completed");
        else if((Sint32)(now-log->next)>=0) { capture(log); log->next=now+1000; }
    } else if(log->writer && state!=DIAG_SAVING) {
        SDL_WaitThread(log->writer,NULL); log->writer=NULL;
    }
}
void diagnostic_log_toggle(DiagnosticLog *log)
{
    if(!log) return;
    diagnostic_log_poll(log);
    int state=diagnostic_log_state(log);
    if(state==DIAG_RECORDING) { finish(log,"stopped by user"); return; }
    if(state==DIAG_SAVING) return;
    /* Store alongside the running program's working directory; fall back to
     * the application's per-user directory if the path cannot be obtained. */
    char cwd[4096];
    if(getcwd(cwd,sizeof(cwd)) && strlen(cwd)+2<sizeof(log->directory)) {
        size_t length=strlen(cwd);
        memcpy(log->directory,cwd,length);
        memcpy(log->directory+length,"/",2);
    }
    else {
        char *pref=SDL_GetPrefPath("PE1ITR","ATV Receiver");
        if(!pref || strlen(pref)>=sizeof(log->directory)) {
            SDL_free(pref); strcpy(log->error,"Cannot determine diagnosis directory");
            SDL_AtomicSet(&log->state,DIAG_ERROR); return;
        }
        strcpy(log->directory,pref); SDL_free(pref);
    }
    log->count=0; log->path[0]=log->error[0]=0;
    strcpy(log->screenshot,"not captured");
    log->started=log->next=SDL_GetTicks();
    SDL_AtomicSet(&log->state,DIAG_RECORDING);
    diagnostic_log_poll(log);
}
void diagnostic_log_destroy(DiagnosticLog *log)
{
    if(!log) return;
    if(diagnostic_log_state(log)==DIAG_RECORDING) finish(log,"receiver closed or setup reopened");
    if(log->writer) SDL_WaitThread(log->writer,NULL);
    if(diagnostic_log_state(log)==DIAG_SAVED) fprintf(stderr,"Diagnosis saved: %s\n",log->path);
    else if(diagnostic_log_state(log)==DIAG_ERROR) fprintf(stderr,"%s\n",log->error);
    free(log);
}
