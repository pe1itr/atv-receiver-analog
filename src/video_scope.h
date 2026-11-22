#ifndef VIDEO_SCOPE_H
#define VIDEO_SCOPE_H
#include <stddef.h>
#include <stdint.h>

#define VIDEO_SCOPE_POINTS 640
/* Bounded acquisition memory at all timebases and sample rates. */
#define VIDEO_SCOPE_CAPACITY 16384
#define VIDEO_SCOPE_SCALES 9
extern const unsigned video_scope_us_per_div[VIDEO_SCOPE_SCALES];
typedef struct {
    float low[VIDEO_SCOPE_POINTS], high[VIDEO_SCOPE_POINTS];
    float minimum, maximum;
    double start_us, duration_us;
    unsigned sequence;
    int triggered;
} VideoScopeReading;
typedef struct {
    float samples[VIDEO_SCOPE_CAPACITY];
    size_t count, wanted, skip, stride, partial;
    double partial_sum;
    unsigned timebase;
    double rate, line_hz;
    VideoScopeReading reading;
} VideoScope;

void video_scope_init(VideoScope *s,double rate,double line_hz);
/* Discard an incomplete trace after tuning, gaps, or mode changes. */
void video_scope_reset(VideoScope *s,double line_hz);
/* Bounded short acquisitions, about 25/s. Returns 1 on a new reading.
 * Input is the same post-FM/de-emphasis video that feeds the TV decoder. */
int video_scope_process(VideoScope *s,const float *video,size_t count);
#endif
