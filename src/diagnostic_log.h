#ifndef DIAGNOSTIC_LOG_H
#define DIAGNOSTIC_LOG_H
#include <SDL.h>
#include <stddef.h>
#define DIAGNOSTIC_SECONDS 30
typedef struct DiagnosticLog DiagnosticLog;
typedef int (*DiagnosticSnapshot)(void *,char *,size_t);
enum { DIAG_IDLE, DIAG_RECORDING, DIAG_SAVING, DIAG_SAVED, DIAG_ERROR };
DiagnosticLog *diagnostic_log_create(DiagnosticSnapshot snapshot,void *opaque);
void diagnostic_log_toggle(DiagnosticLog *log);
void diagnostic_log_poll(DiagnosticLog *log);
/* Attach the separately saved initial screenshot while recording. */
void diagnostic_log_screenshot(DiagnosticLog *log,const char *path,int saved);
int diagnostic_log_state(DiagnosticLog *log);
unsigned diagnostic_log_remaining(DiagnosticLog *log);
/* Valid when SAVED or ERROR; UTF-8 absolute path / error description. */
const char *diagnostic_log_path(DiagnosticLog *log);
const char *diagnostic_log_error(DiagnosticLog *log);
void diagnostic_log_destroy(DiagnosticLog *log);
#endif
