#include "core.h"
static _Thread_local NicamCoreIO *core_io;
/* Keep the proven decoder in its own translation unit and namespace. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#define main nicam_original_main
#include "nicam_cli.c"
#undef main
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
int nicam_core_run(NicamCoreIO *io)
{
    core_io=io;
    char *args[]={"nicam", "--iq-format", "s16", "--adaptive-fixed", "--matched-filter"};
    /* Live playback inserts silence for damaged frames; never build up a
     * deferred interpolation backlog after an extended fade. */
    Config cfg;
    if(parse_args(5,args,&cfg)) return -1;
    cfg.conceal_mode=2;
    /* The subcarrier is already translated close to DC. Narrower carrier
     * and timing loops reduce noise-driven phase/timing jitter without
     * relaxing frame validation. Verified with noisy recorded ATV IQ. */
    cfg.adaptive_costas_alpha=0.04;
    cfg.adaptive_gain_mu=0.05;
    init_scramble();
    FrontendFilter frontend;
    MatchedFilter matched;
    if(init_frontend_filter(&frontend,&cfg)) return -1;
    if(init_matched_filter(&matched,&cfg)) { free_frontend_filter(&frontend); return -1; }
    int result=run_adaptive_quality(&cfg,&frontend,&matched);
    free_frontend_filter(&frontend); free_matched_filter(&matched);
    core_io=NULL;
    return result;
}
