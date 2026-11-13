#ifndef SOUND_H
#define SOUND_H
#include <stddef.h>
#include <stdint.h>
/* Mono FM sound at 6 MHz in the composite baseband. Output is 48 kHz float. */
typedef void (*SoundOutput)(void *, const float *, size_t);
typedef struct Sound Sound;
int sound_enabled(double rate, double bandwidth);
Sound *sound_create(double rate, SoundOutput output, void *opaque);
void sound_reset(Sound *s);
void sound_process(void *opaque, const float *video, size_t n);
double sound_power(const Sound *s);
int sound_locked(const Sound *s); /* Read only while processing is idle. */
void sound_destroy(Sound *s);
/* Dual FM: interleaved 48 kHz stereo, n counts frames. No automatic squelch. */
typedef struct StereoSound StereoSound;
int stereo_sound_enabled(double rate, double bandwidth);
StereoSound *stereo_sound_create(double rate, SoundOutput output, void *opaque);
/* upper_pair: 0 = 7.02/7.22 MHz, 1 = 7.38/7.56 MHz. Resets all channels. */
void stereo_sound_reset(StereoSound *s, int upper_pair, int swap);
void stereo_sound_process(StereoSound *s, const float *samples, size_t n);
/* Scan all carriers for 160 ms every 2 s; optionally play the selected pair
 * continuously. Detection holds the last completed scan until the next one. */
void stereo_sound_monitor(StereoSound *s, const float *samples, size_t n, int playback);
/* Bits 0..3: 7.02, 7.22, 7.38, 7.56 MHz. Read while processing is idle. */
unsigned stereo_sound_detected(const StereoSound *s);
int stereo_sound_locked(const StereoSound *s);
void stereo_sound_destroy(StereoSound *s);
#endif
