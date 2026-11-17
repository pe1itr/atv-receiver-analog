#ifndef ATV_IQ_DC_BLOCK_H
#define ATV_IQ_DC_BLOCK_H
#include "atv.h"
#include <math.h>
#include <string.h>

/* 0 = automatic (HackRF), 1 = enabled, 2 = disabled. */
static inline int iq_dc_block_enabled(const Config *c)
{
    return c->iq_dc_block==1 || (!c->iq_dc_block && !strcmp(c->mode,"hackrf"));
}

typedef struct {
    double i, q, alpha;
    int initialized;
} IQDCBlock;

static inline void iq_dc_block_reset(IQDCBlock *d,double rate)
{
    *d=(IQDCBlock){.alpha=-expm1(-2*PI*10/rate)};
}

/* Remove only a very narrow region around zero Hz. Bootstrap from a block
 * mean so a large initial offset does not mute the first several fields.
 * State persists across blocks; double precision avoids stalled estimates
 * at the small coefficients required by SDR sample rates. */
static inline void iq_dc_block_process(IQDCBlock *d,IQ *iq,size_t n)
{
    if(!n) return;
    if(!d->initialized) {
        for(size_t j=0;j<n;++j) { d->i+=iq[j].i; d->q+=iq[j].q; }
        d->i/=n; d->q/=n; d->initialized=1;
    }
    for(size_t j=0;j<n;++j) {
        double i=iq[j].i-d->i,q=iq[j].q-d->q;
        d->i+=d->alpha*i; d->q+=d->alpha*q;
        iq[j]=(IQ){(float)i,(float)q};
    }
}
#endif
