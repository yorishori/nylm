#ifndef CONN_H
#define CONN_H

#include <stddef.h>
#include <sys/types.h>

/* One client connection. All socket I/O goes through here. */
struct conn {
    int fd;
    char ip[46];     /* client address, for the log */
    double deadline; /* monotonic time after which reads/writes fail */
};

double now_seconds(void); /* monotonic clock */

/* Sets per-call socket timeouts and the whole-request deadline. */
void conn_init(struct conn *c, int fd, const char *ip);

/* Like read(2); returns -1 on error, timeout or passed deadline. */
ssize_t conn_read(struct conn *c, void *buf, size_t len);

/* Writes everything or returns -1. */
int conn_write(struct conn *c, const void *buf, size_t len);

/* Closes the socket. */
void conn_close(struct conn *c);

#endif
