#ifndef PAL_H
#define PAL_H
#include <stdint.h>
/* PAL 4.43 burst-referenced decoder. State belongs to the video DSP thread. */
typedef struct {
    double rate, step_c, step_s, osc_c, osc_s;
    uint64_t tick, line, last_lock;
    int initialized, gate_done, count, have_burst, good, locked;
    unsigned burst_updates;
    double burst_c[2], burst_s[2], burst_power[2];
    unsigned burst_count[2], parity;
    double burst_frequency, tracked_phase;
    uint64_t tracked_tick;
    double sum_c, sum_s, sum_cc, sum_cs, sum_v, sum_vc, sum_vs, sum_vv;
    float axis_c, axis_s, polarity;
    float out_y, out_u, out_v;
    float y_alpha, y_delay[32];
    unsigned delay_samples, delay_pos;
    float alpha, u1, u2, v1, v2, y1, y2;
    float notch_b, notch_a, notch_gain, x1, x2, z1, z2;
} PalDecoder;
void pal_init(PalDecoder *p, double rate);
void pal_sample(PalDecoder *p, float composite, uint64_t tick, uint64_t line, int have_line);
uint32_t pal_rgb(const PalDecoder *p);
#endif
