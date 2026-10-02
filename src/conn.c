#define _POSIX_C_SOURCE 200809L

#include "conn.h"

#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* A single read or write may block at most this long... */
#define IO_TIMEOUT_SECONDS 5
/* ...and a whole request (read + handle + write) at most this long. */
#define REQUEST_SECONDS 15

double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int conn_init(struct conn *c, int fd)
{
    c->fd = fd;
    c->deadline = now_seconds() + REQUEST_SECONDS;

    /* Without these a silent client could block the server forever. */
    struct timeval tv = { .tv_sec = IO_TIMEOUT_SECONDS };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) != 0) {
        perror("conn: setsockopt timeout");
        return -1;
    }
    return 0;
}

ssize_t conn_read(struct conn *c, void *buf, size_t len)
{
    for (;;) {
        if (now_seconds() > c->deadline)
            return -1;
        ssize_t n = read(c->fd, buf, len);
        if (n < 0 && errno == EINTR)
            continue;
        return n;
    }
}

int conn_write(struct conn *c, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        if (now_seconds() > c->deadline)
            return -1;
        ssize_t n = write(c->fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

void conn_close(struct conn *c)
{
    close(c->fd);
    c->fd = -1;
}
