#ifndef STATUS_SERVER_H
#define STATUS_SERVER_H
#include <stddef.h>
typedef struct StatusServer StatusServer;
typedef int (*StatusJSON)(void *opaque, char *buffer, size_t capacity);
/* Numeric IPv4 bind address. All I/O is nonblocking and bounded. */
StatusServer *status_server_open(const char *ip, int port);
void status_server_poll(StatusServer *server, StatusJSON json, void *opaque);
void status_server_close(StatusServer *server);
#endif
