#ifndef RX_QUEUE_H
#define RX_QUEUE_H
#include "atv.h"

typedef struct RxQueue RxQueue;
typedef int (*RxReadFn)(void *, IQ *, size_t);
typedef int (*RxControlFn)(void *);
/* One producer owns read/serialized control calls; one consumer reads or
 * sends controls (or executes a live control on its own thread).
 * read: samples, 0 timeout, -2 discontinuity, other negative fatal error. */
RxQueue *rx_queue_create(size_t block, size_t slots, RxReadFn read, void *opaque);
int rx_queue_read(RxQueue *q, IQ *iq, size_t capacity);
int rx_queue_control(RxQueue *q, RxControlFn fn, void *opaque);
/* Runs control on the sole consumer's thread while the producer keeps
 * reading and discards transition samples. Only for drivers supporting
 * concurrent read/settings calls (UHD). Stop/destroy after it returns. */
int rx_queue_control_live(RxQueue *q, RxControlFn fn, void *opaque);
void rx_queue_stats(RxQueue *q, SourceStats *stats);
void rx_queue_stop(RxQueue *q);
void rx_queue_destroy(RxQueue *q);
#endif
