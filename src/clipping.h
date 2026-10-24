#ifndef ATV_CLIPPING_H
#define ATV_CLIPPING_H
#include "atv.h"
#include <string.h>

/* Rail hits in delivered, unfiltered IQ, not proof of analogue overload.
 * SoapyHackRF readbuf converts signed int8 to CF32 by dividing by 127:
 * https://github.com/pothosware/SoapyHackRF/blob/master/HackRF_Streaming.cpp
 * Other streams are checked against normalized digital full scale; upstream
 * ADC clipping hidden by hardware filtering/scaling cannot be detected here. */
typedef struct {
    uint64_t samples, clipped_samples;
    uint32_t last_clip_tick;
    float low, high;
} Clipping;

static inline void clipping_init(Clipping *c, const char *mode)
{
    memset(c,0,sizeof(*c));
    c->low=!strcmp(mode,"hackrf")?-128.0f/127.0f:-1.0f;
    c->high=1.0f;
}

static inline void clipping_process(Clipping *c, const IQ *iq, size_t n, uint32_t tick)
{
    uint64_t clipped=0;
    for(size_t j=0;j<n;++j)
        clipped+=(iq[j].i<=c->low) | (iq[j].i>=c->high) |
                 (iq[j].q<=c->low) | (iq[j].q>=c->high);
    c->samples+=n;
    c->clipped_samples+=clipped;
    if(clipped) c->last_clip_tick=tick;
}

static inline int clipping_active(const Clipping *c, uint32_t tick)
{
    return c->clipped_samples && (uint32_t)(tick-c->last_clip_tick)<2000;
}
#endif
