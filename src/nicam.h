#ifndef ATV_NICAM_H
#define ATV_NICAM_H
#include <stddef.h>
#include <stdint.h>
typedef struct Nicam Nicam;
/* Cumulative diagnostics since creation; select/reset do not clear them. */
typedef struct {
    unsigned good_frames, bad_frames, muted_frames, sync_bits_discarded;
} NicamStats;
void nicam_stats(Nicam *n, NicamStats *stats);
typedef void (*NicamOutput)(void *,const float *,size_t); /* 48 kHz stereo frames */
Nicam *nicam_create(double rate,NicamOutput output,void *opaque);
/* Sole producer, called while the audio worker is idle for select/reset. */
int nicam_select(Nicam *n,int enabled);
void nicam_reset(Nicam *n);
void nicam_process(Nicam *n,const float *video,size_t count,double fm_power);
int nicam_detected(const Nicam *n);
int nicam_locked(Nicam *n);
unsigned nicam_overflows(Nicam *n);
int nicam_active(const Nicam *n);
/* Test/offline hook: already filtered complex int16 at 1.456 MS/s. */
int nicam_push_iq(Nicam *n,const int16_t *iq,size_t count);
void nicam_finish(Nicam *n); /* Drain and join; used at EOF, never for live backpressure. */
void nicam_destroy(Nicam *n);
#endif
