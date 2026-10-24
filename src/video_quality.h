#ifndef VIDEO_QUALITY_H
#define VIDEO_QUALITY_H
#include <stddef.h>
#include <stdint.h>
/* Unweighted PAL diagnostic, measured at decoder input after selected video
 * filtering/de-emphasis. Noise gate: 9..10 us; sync reference: 1.5..3.5 us. */
typedef struct {
    double video_snr_db, noise_rms, sync_depth, sync_depth_error;
    unsigned lines;
    int valid;
    enum { VQ_COLLECTING, VQ_OK, VQ_NO_SYNC, VQ_FEW_LINES, VQ_NO_NOISE, VQ_BAD_DEPTH, VQ_UNCERTAIN } status;
    unsigned sequence;
} VideoQualityReading;
typedef struct {
    double rate, elapsed, unsynced;
    double line_start;
    double means[2], m2[2];
    unsigned counts[2];
    double noise_m2, porch_mean, depth_mean, depth_m2;
    uint64_t samples;
    unsigned lines;
    int have_line, window_sync;
    VideoQualityReading reading;
} VideoQuality;
void video_quality_init(VideoQuality *q, double rate);
/* Clear current measurements after retuning or a stream discontinuity. */
void video_quality_reset(VideoQuality *q);
const char *video_quality_status(const VideoQualityReading *r);
void video_quality_process(VideoQuality *q, const float *video, size_t n,
                           uint64_t first_tick, int sync_valid,
                           double line_origin, double line_period,
                           double field_origin, double field_period);
#endif
