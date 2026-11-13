#ifndef LIGHT_STACK_H
#define LIGHT_STACK_H
#include "atv.h"
#define LIGHT_STACK_FRAMES 6
typedef struct {
    uint32_t history[LIGHT_STACK_FRAMES][WIDTH*HEIGHT];
    uint16_t sum[WIDTH*HEIGHT][3];
    unsigned count,next;
} LightStack;
/* Reset counters only; stale pixels are never included after a reset. */
void light_stack_reset(LightStack *s);
/* Exact rolling RGB average, without registration, sync gating or allocation. */
void light_stack_add(LightStack *s,const uint32_t *pixels,uint32_t *output);
#endif
