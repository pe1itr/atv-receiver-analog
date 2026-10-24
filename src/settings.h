#ifndef SETTINGS_H
#define SETTINGS_H
#include "atv.h"
#include <stdio.h>
#include <string.h>
typedef struct {
    Config c; /* Frequency is the displayed frequency, including converter LO. */
    int stacking, stack_frames, rolling, paused, vertical_shift;
    int audio_source, audio_muted, audio_swap;
    char strings[9][1024];
} Settings;
/* Missing optional files are allowed. Malformed files fail with a diagnostic. */
size_t settings_default_rx_block(const char *mode);
static inline double settings_min_rate(const char *mode)
{
    return mode && !strcmp(mode,"pluto")?5e6:8e6;
}
const char *settings_hardware_rate_hint(unsigned index);
int settings_load(Settings *s, const char *path, int required);
int settings_save(const Settings *s, const char *path);
typedef struct {
    const char *title, *current, *details, *error;
    enum { SETTINGS_VALUE, SETTINGS_WELCOME, SETTINGS_HARDWARE } kind;
} SettingsPrompt;
/* Return 1 with a submitted line, or 0 to cancel startup. */
typedef int (*SettingsPromptFn)(void *, const SettingsPrompt *, char *, size_t);
int settings_run_wizard(Settings *s, SettingsPromptFn prompt, void *opaque);
/* Optional playback-device step; names are copied when selected, not indices. */
int settings_run_wizard_audio(Settings *s, SettingsPromptFn prompt, void *opaque,
                              const char *const *devices, int count);
int settings_wizard(Settings *s, FILE *in, FILE *out);
#endif
