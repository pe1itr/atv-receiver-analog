#ifndef AFC_H
#define AFC_H
#include <stddef.h>
#include <stdint.h>
#define AFC_RANGE_HZ 500000.0
/* Fold two independent 40 ms windows on the PAL line period. The estimator
 * uses blanking, not picture brightness or the largest spectral peak. */
typedef struct {
    double sum[2][256], phase, rate, line_hz;
    unsigned count[2][256];
    uint64_t tick, settle;
} AFC;
void afc_reset(AFC *a, double rate, double line_hz);
/* 0: collecting, -1: no reliable PAL reference, 1: error in RF Hz available.
 * Input is demodulated video (black=0), with configured polarity applied. */
int afc_measure(AFC *a, const float *video, size_t n, double deviation, int invert, double *error);
/* Bound the target to the capture window and step to 100 kHz. */
double afc_target(double anchor, double current, double error, int *limited);
#endif
