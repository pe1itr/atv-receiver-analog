#ifndef IQ_RECORDER_H
#define IQ_RECORDER_H
#include "atv.h"

typedef struct IQRecorder IQRecorder;
typedef enum { IQ_REC_IDLE, IQ_REC_PREPARING, IQ_REC_CAPTURING,
    IQ_REC_SAVING, IQ_REC_SAVED, IQ_REC_ERROR } IQRecordState;
typedef struct {
    Config c; /* Actual SDR frequency; LO is added for the displayed RF. */
    int audio_source, vertical_shift;
} IQRecordSettings;
typedef struct {
    IQRecordState state;
    size_t count, capacity;
    double rate;
    uint32_t updated;
    char path[192], error[160], reason[40];
} IQRecordStatus;

IQRecorder *iq_recorder_create(void);
/* Starts a bounded capture, or stops an active one. Never allocates or writes
 * files on the caller. A request during saving is ignored. */
void iq_recorder_toggle(IQRecorder *r,const IQRecordSettings *settings);
/* Only a bounded memory copy under a short lock; no allocation or disk I/O. */
void iq_recorder_push(IQRecorder *r,const IQ *iq,size_t count);
/* Save the contiguous prefix before settings changes, a pause or an RX gap. */
void iq_recorder_finish(IQRecorder *r,const char *reason);
void iq_recorder_status(IQRecorder *r,IQRecordStatus *status);
/* Save any partial capture and join the disk writer. Call after RX stops. */
void iq_recorder_destroy(IQRecorder *r);
#endif
