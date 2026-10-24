#ifndef SOURCE_PLUTO_H
#define SOURCE_PLUTO_H
#include "atv.h"
typedef struct PlutoSource PlutoSource;
PlutoSource *pluto_source_open(Config *c);
int pluto_source_tune(PlutoSource *s, double *hz, double *gain);
int pluto_source_read(PlutoSource *s, IQ *iq, size_t n);
void pluto_source_stop(PlutoSource *s);
void pluto_source_close(PlutoSource *s);
#endif
