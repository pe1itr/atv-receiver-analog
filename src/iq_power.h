#ifndef ATV_IQ_POWER_H
#define ATV_IQ_POWER_H
#include "atv.h"
#include <math.h>

/* Complex AC power: exclude the measured block's constant I/Q component.
 * Two passes avoid cancellation when a large DC offset hides a weak signal.
 * Inspect the samples without changing the demodulation/recording input. */
static inline double iq_ac_power_dbfs(const IQ *iq,size_t n)
{
    if(!n) return NAN;
    double mean_i=0,mean_q=0,power=0;
    for(size_t j=0;j<n;++j) { mean_i+=iq[j].i; mean_q+=iq[j].q; }
    mean_i/=n; mean_q/=n;
    for(size_t j=0;j<n;++j) {
        double i=iq[j].i-mean_i,q=iq[j].q-mean_q;
        power+=i*i+q*q;
    }
    return 10*log10(power/n+1e-16);
}
#endif
