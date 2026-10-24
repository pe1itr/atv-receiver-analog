#ifndef NICAM_CORE_H
#define NICAM_CORE_H
#include <stddef.h>
/* Blocking input, native int16 complex at 1.456 MS/s; stereo int16 32 kHz output. */
typedef struct {
    int (*read)(void *, void *, size_t);
    int (*pcm)(void *, const void *, size_t);
    void (*quality)(void *, int);
    void *opaque;
    void (*discard_bits)(void *, size_t); /* Optional sync realignment diagnostic. */
} NicamCoreIO;
int nicam_core_run(NicamCoreIO *io);
#endif
