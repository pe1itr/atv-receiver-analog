/* Lifecycle checks use a controlled clock, real background I/O and real files. */
#include <SDL.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
static Uint32 clock_ms=100;
static Uint32 test_ticks(void) { return clock_ms; }
#define SDL_GetTicks test_ticks
#include "../src/diagnostic_log.c"
#undef SDL_GetTicks
static unsigned snapshots;
static int snapshot(void *opaque,char *out,size_t capacity)
{
    (void)opaque; ++snapshots;
    return snprintf(out,capacity,"frequency=1252.125 MHz; Video-SNR unavailable (no_sync)\n\n");
}
static void wait_saved(DiagnosticLog *log)
{
    for(unsigned i=0;i<3000 && diagnostic_log_state(log)==DIAG_SAVING;++i) SDL_Delay(1);
    diagnostic_log_poll(log);
    assert(diagnostic_log_state(log)==DIAG_SAVED);
}
int main(void)
{
    assert(SDL_Init(SDL_INIT_TIMER)==0);
    CpuSample before={.wall_seconds=10,.process_seconds=2,.system_total=100,.system_idle=20,.process_valid=1,.system_valid=1};
    CpuSample after={.wall_seconds=12,.process_seconds=5,.system_total=900,.system_idle=220,.process_valid=1,.system_valid=1};
    char cpu_text[512];
    format_cpu_usage(cpu_text,sizeof(cpu_text),&before,&after,8);
    assert(strstr(cpu_text,"1.500 cores; 150.00% of one logical CPU; 18.75% of 8 logical CPUs"));
    assert(strstr(cpu_text,"System CPU busy (includes other programs): 75.00%"));
    format_cpu_usage(cpu_text,sizeof(cpu_text),NULL,&after,8);
    assert(strstr(cpu_text,"unavailable (first sample"));
    CpuSample native=read_cpu_sample(); assert(native.process_valid && native.system_valid);

    char directory[]="/tmp/atv-diagnostic-test-XXXXXX";
    assert(mkdtemp(directory)); assert(chdir(directory)==0);
    DiagnosticLog *log=diagnostic_log_create(snapshot,NULL); assert(log);
    diagnostic_log_toggle(log);
    assert(diagnostic_log_state(log)==DIAG_RECORDING && snapshots==1);
    diagnostic_log_screenshot(log,"initial-test.png",1);
    for(unsigned i=1;i<=30;++i) { clock_ms+=1000; diagnostic_log_poll(log); }
    wait_saved(log); assert(snapshots==31);
    char first[4352]; strcpy(first,diagnostic_log_path(log));
    FILE *fp=fopen(first,"r"); assert(fp);
    char data[16384]; size_t n=fread(data,1,sizeof(data)-1,fp); data[n]=0; fclose(fp);
    assert(strstr(data,"Duration: 30.000 seconds; snapshots: 31"));
    assert(strstr(data,"Receiver process CPU (all threads):"));
    assert(strstr(data,"System CPU busy (includes other programs):"));
    assert(strstr(data,"Platform: Linux")); assert(strstr(data,"initial-test.png"));
    assert(strstr(data,"30 seconds completed")); assert(strstr(data,"Elapsed 30.000 s"));
    diagnostic_log_toggle(log); clock_ms+=500; diagnostic_log_toggle(log); wait_saved(log);
    assert(strcmp(first,diagnostic_log_path(log))!=0); /* Never overwrite. */
    assert(unlink(first)==0); assert(unlink(diagnostic_log_path(log))==0);
    diagnostic_log_toggle(log); diagnostic_log_destroy(log); /* Save partial on quit. */
    log=diagnostic_log_create(snapshot,NULL); assert(log);
    diagnostic_log_toggle(log);
    /* Directory disappears between capture and save: visible, nonfatal error. */
    strcpy(log->directory,"/this-directory-does-not-exist/");
    diagnostic_log_toggle(log);
    for(unsigned i=0;i<3000 && diagnostic_log_state(log)==DIAG_SAVING;++i) SDL_Delay(1);
    assert(diagnostic_log_state(log)==DIAG_ERROR && *diagnostic_log_error(log));
    diagnostic_log_destroy(log);
    SDL_Quit();
    puts("Diagnosis: 30-second deadline, bounded snapshots, early stop, unique TXT, partial close and I/O error OK");
    return 0;
}
