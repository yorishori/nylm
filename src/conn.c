#define _POSIX_C_SOURCE 200809L

#include "conn.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
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

void conn_init(struct conn *c, int fd, const char *ip)
{
    c->fd = fd;
    c->ssl = NULL;
    snprintf(c->ip, sizeof c->ip, "%s", ip);
    c->deadline = now_seconds() + REQUEST_SECONDS;

    struct timeval tv = { .tv_sec = IO_TIMEOUT_SECONDS };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

ssize_t conn_read(struct conn *c, void *buf, size_t len)
{
    for (;;) {
        if (now_seconds() > c->deadline)
            return -1;
        if (c->ssl != NULL) {
            if (len > INT_MAX)
                len = INT_MAX;
            int n = SSL_read(c->ssl, buf, (int)len);
            return n > 0 ? n : (SSL_get_error(c->ssl, n) == SSL_ERROR_ZERO_RETURN ? 0 : -1);
        }
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
        ssize_t n;
        if (c->ssl != NULL) {
            int chunk = len > INT_MAX ? INT_MAX : (int)len;
            int w = SSL_write(c->ssl, p, chunk);
            if (w <= 0)
                return -1;
            n = w;
        } else {
            n = write(c->fd, p, len);
        }
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
    if (c->ssl != NULL) {
        SSL_shutdown(c->ssl); /* send close_notify; don't wait for the reply */
        SSL_free(c->ssl);
        c->ssl = NULL;
    }
    close(c->fd);
    c->fd = -1;
}
