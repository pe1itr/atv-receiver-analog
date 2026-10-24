#ifndef ALIGNMENT_H
#define ALIGNMENT_H
#include "atv.h"
typedef struct Alignment Alignment;
Alignment *alignment_create(double rate, double bandwidth, double deviation, int invert, size_t capacity);
/* Full-rate video branch; raw_composite bypasses the monochrome low-pass. */
Alignment *alignment_create_video(double rate, double bandwidth, double deviation, int invert, size_t capacity, int raw_composite);
/* Single caller; submit copies IQ. Exactly one outstanding block is allowed.
 * wait returns the held demodulated guide, valid until the next submit/reset. */
int alignment_submit(Alignment *a, const IQ *iq, size_t n);
const float *alignment_wait(Alignment *a);
/* IQ power after the bandwidth filter, before FM; NaN until first measurement.
 * Same single caller as submit/wait; waits for the outstanding block. */
double alignment_if_power(Alignment *a, uint32_t *measured_at);
/* Optional consumer of full-rate demodulated samples, on the video worker.
 * Set only from the submitting thread; waits for any outstanding block. */
void alignment_set_consumer(Alignment *a, void (*consume)(void *, const float *, size_t), void *opaque);
/* Enable linear-phase video correction after the raw audio consumer. */
int alignment_set_deemphasis(Alignment *a);
void alignment_set_fm_mode(Alignment *a, int mode, int invert);
void alignment_reset(Alignment *a, int invert);
void alignment_destroy(Alignment *a);
#endif
