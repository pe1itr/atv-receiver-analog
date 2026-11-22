#ifndef ATV_PAL_PIPELINE_H
#define ATV_PAL_PIPELINE_H
#include "atv.h"

/* A configured block must span multiple chunks for preparation to overlap drawing. */
#define PAL_CHUNK 8192u
typedef struct PalPipeline PalPipeline;
PalPipeline *pal_pipeline_create(void);
void pal_pipeline_destroy(PalPipeline *p);
/* Single receiver thread. Jobs finish before the input buffers are released.
 * A new decoder starts with previous_valid cleared (also after a reset). */
void pal_pipeline_decode(PalPipeline *p, Decoder *d, const float *video, const float *guide, size_t n);
#endif
