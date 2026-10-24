#ifndef FM_MATH_H
#define FM_MATH_H
#include <math.h>

/* MinGW's atan2f uses x87 FPATAN, which is costly at SDR sample rates.
 * Reduce to |z| <= tan(pi/8), then evaluate atan's alternating series.
 * The first omitted term is <= tan(pi/8)^19 / 19 < 3e-9 radians;
 * float rounding dominates. Keep libm's special-value semantics. */
#if defined(_WIN32) || defined(ATV_FAST_PHASE)
static inline float fm_atan2f(float y, float x)
{
    float ax=fabsf(x), ay=fabsf(y);
    if(!isfinite(ax) || !isfinite(ay) || (ax==0 && ay==0)) return atan2f(y,x);
    int swap=ay>ax;
    float z=swap?ax/ay:ay/ax;
    int reduce=z>0.41421356237309505f;
    if(reduce) z=(z-1)/(z+1);
    float zz=z*z;
    float p=1.0f/17;
    p=-1.0f/15+zz*p;
    p=1.0f/13+zz*p;
    p=-1.0f/11+zz*p;
    p=1.0f/9+zz*p;
    p=-1.0f/7+zz*p;
    p=1.0f/5+zz*p;
    p=-1.0f/3+zz*p;
    float angle=z+z*zz*p;
    if(reduce) angle+=0.7853981633974483f;
    if(swap) angle=1.5707963267948966f-angle;
    if(signbit(x)) angle=3.1415926535897932f-angle;
    return copysignf(angle,y);
}
#else
#define fm_atan2f atan2f
#endif
#endif
