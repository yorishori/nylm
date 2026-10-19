#define _POSIX_C_SOURCE 200809L

#include "conn.h"

#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

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

/* The reason of OpenSSL's last error, for a log line ("" if none). */
static const char *tls_error(void)
{
    unsigned long e = ERR_get_error();
    const char *reason = e != 0 ? ERR_reason_error_string(e) : NULL;
    ERR_clear_error();
    return reason != NULL ? reason : "";
}

SSL_CTX *conn_tls_init(const char *cert_file, const char *key_file)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (ctx == NULL) {
        fprintf(stderr, "tls: SSL_CTX_new: %s\n", tls_error());
        return NULL;
    }
    /* No renegotiation: a client could make the server work for nothing. */
    SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION);
    if (SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) != 1) {
        fprintf(stderr, "tls: can not require TLS 1.2: %s\n", tls_error());
    } else if (SSL_CTX_use_certificate_chain_file(ctx, cert_file) != 1) {
        fprintf(stderr, "tls: certificate %s: %s\n", cert_file, tls_error());
    } else if (SSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM) != 1) {
        fprintf(stderr, "tls: private key %s: %s\n", key_file, tls_error());
    } else if (SSL_CTX_check_private_key(ctx) != 1) {
        fprintf(stderr, "tls: %s is not the key of %s: %s\n", key_file, cert_file, tls_error());
    } else {
        return ctx;
    }
    SSL_CTX_free(ctx);
    return NULL;
}

int conn_init(struct conn *c, int fd, SSL_CTX *tls, const char *ip)
{
    c->fd = fd;
    c->deadline = now_seconds() + REQUEST_SECONDS;
    c->ssl = NULL;

    /* Without these a silent client could block the server forever. */
    struct timeval tv = { .tv_sec = IO_TIMEOUT_SECONDS };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) != 0) {
        perror("conn: setsockopt timeout");
        return -1;
    }
    if (tls == NULL)
        return 0;

    c->ssl = SSL_new(tls);
    if (c->ssl == NULL || SSL_set_fd(c->ssl, fd) != 1) {
        fprintf(stderr, "tls: SSL_new: %s\n", tls_error());
        return -1;
    }
    /* Each read and write of the handshake has the socket timeouts. */
    if (SSL_accept(c->ssl) != 1 || now_seconds() > c->deadline) {
        printf("%s tls handshake failed %s\n", ip, tls_error());
        fflush(stdout);
        return -1;
    }
    return 0;
}

ssize_t conn_read(struct conn *c, void *buf, size_t len)
{
    for (;;) {
        if (now_seconds() > c->deadline)
            return -1;
        if (c->ssl != NULL) {
            /* Timeouts and errors alike end the request; a clean close is 0. */
            size_t got = 0;
            int ok = SSL_read_ex(c->ssl, buf, len, &got);
            int closed = !ok && SSL_get_error(c->ssl, 0) == SSL_ERROR_ZERO_RETURN;
            ERR_clear_error();
            return ok ? (ssize_t)got : closed ? 0 : -1;
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
        if (c->ssl != NULL) {
            size_t done = 0;
            if (SSL_write_ex(c->ssl, p, len, &done) != 1) {
                ERR_clear_error();
                return -1;
            }
            p += done;
            len -= done;
            continue;
        }
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
    if (c->ssl != NULL) {
        SSL_shutdown(c->ssl); /* tells the client we are done; its answer is not awaited */
        SSL_free(c->ssl);
        ERR_clear_error();
        c->ssl = NULL;
    }
    close(c->fd);
    c->fd = -1;
}
