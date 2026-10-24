#ifndef TEST_SIGNAL_H
#define TEST_SIGNAL_H
#include "atv.h"
typedef struct {
    double rate, deviation, amplitude, cnr_db, offset;
    double line_offset_hz, field_offset_hz;
    int noise, invert, color, no_burst;
    double color_phase, color_offset_hz;
    uint64_t seed;
} TestSignalConfig;
typedef struct {
    TestSignalConfig config;
    uint64_t sample, random;
    double phase, noise_sigma, noise_energy;
    uint8_t picture[WIDTH*HEIGHT];
} TestSignal;
void test_signal_init(TestSignal *s, TestSignalConfig config);
void test_signal_generate(TestSignal *s, IQ *out, size_t count);
#endif
