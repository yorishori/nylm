#ifndef CONN_H
#define CONN_H

#include <stddef.h>
#include <sys/types.h>

struct ssl_st;
struct ssl_ctx_st;

/* One client connection. All socket I/O goes through here: plain, or
 * through TLS when the server has a certificate (NYLM_TLS_CERT). */
struct conn {
    int fd;
    double deadline;    /* monotonic time after which reads/writes fail */
    struct ssl_st *ssl; /* NULL: plain HTTP */
};

double now_seconds(void); /* monotonic clock */

/*
 * The TLS settings of the server: TLS 1.2 or newer, with the certificate
 * chain and private key in these PEM files. NULL if they can't be used
 * (logged).
 */
struct ssl_ctx_st *conn_tls_init(const char *cert_file, const char *key_file);

/*
 * Sets per-call socket timeouts and the whole-request deadline; with tls
 * (not NULL), also does the TLS handshake. 0 or -1 (a failed handshake is
 * logged, with the client's address ip).
 */
int conn_init(struct conn *c, int fd, struct ssl_ctx_st *tls, const char *ip);

/* Like read(2); returns -1 on error, timeout or passed deadline. */
ssize_t conn_read(struct conn *c, void *buf, size_t len);

/* Writes everything or returns -1. */
int conn_write(struct conn *c, const void *buf, size_t len);

/* Closes the socket. */
void conn_close(struct conn *c);

#endif
