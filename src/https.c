/*
 * A small HTTPS client for the Qobuz and MusicBrainz services (src/https.h).
 */
#define _POSIX_C_SOURCE 200809L

#include "https.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#define TIMEOUT_SECONDS 30
#define MAX_URL         4096

int https_split(const char *url, char host[HTTPS_MAX_HOST + 1], int *port, const char **path)
{
    static const char scheme[] = "https://";
    if (url == NULL || strncmp(url, scheme, sizeof scheme - 1) != 0 || strlen(url) > MAX_URL)
        return -1;
    const char *h = url + sizeof scheme - 1;
    size_t n = strcspn(h, ":/?#");
    if (n == 0 || n > HTTPS_MAX_HOST)
        return -1;
    for (size_t i = 0; i < n; i++) {
        char c = h[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-'))
            return -1;
    }
    memcpy(host, h, n);
    host[n] = '\0';
    const char *p = h + n;
    *port = 443;
    if (*p == ':') {
        char *end;
        errno = 0;
        long v = strtol(p + 1, &end, 10);
        if (errno != 0 || end == p + 1 || v < 1 || v > 65535 || (*end != '/' && *end != '\0'))
            return -1;
        *port = (int)v;
        p = end;
    }
    if (*p == '#' || *p == '?')
        return -1;
    *path = *p == '\0' ? "/" : p;
    for (const char *c = *path; *c != '\0'; c++)
        if ((unsigned char)*c <= ' ' || (unsigned char)*c >= 0x7f || *c == '#')
            return -1;
    return 0;
}

/* The value of a hex digit, or -1. */
static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                : -1;
}

long https_dechunk(char *buf, size_t len)
{
    size_t in = 0, out = 0;
    for (;;) {
        size_t size = 0;
        int digits = 0;
        while (in < len && hexval(buf[in]) >= 0) {
            if (++digits > 15)
                return -1;
            size = size * 16 + (size_t)hexval(buf[in++]);
        }
        if (digits == 0)
            return -1;
        /* Chunk extensions: skipped up to the line end. */
        while (in < len && buf[in] != '\r')
            in++;
        if (len - in < 2 || buf[in + 1] != '\n')
            return -1;
        in += 2;
        if (size == 0)
            break;
        if (len - in < size + 2 || buf[in + size] != '\r' || buf[in + size + 1] != '\n')
            return -1;
        memmove(buf + out, buf + in, size);
        out += size;
        in += size + 2;
    }
    /* Trailer lines, then the empty line. */
    for (;;) {
        const char *eol = in < len ? memchr(buf + in, '\n', len - in) : NULL;
        if (eol == NULL || eol == buf + in || eol[-1] != '\r')
            return -1;
        size_t line = (size_t)(eol - (buf + in)) + 1;
        in += line;
        if (line == 2)
            break;
    }
    if (in != len)
        return -1;
    buf[out] = '\0';
    return (long)out;
}

/* ---- the connection ----------------------------------------------------- */

static SSL_CTX *ctx;

/* The TLS settings, made once. NULL on error (err set). */
static SSL_CTX *tls(char *err, size_t errlen)
{
    if (ctx != NULL)
        return ctx;
    SSL_CTX *c = SSL_CTX_new(TLS_client_method());
    if (c == NULL || SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_default_verify_paths(c) != 1) {
        snprintf(err, errlen, "TLS setup failed: %s", ERR_reason_error_string(ERR_get_error()));
        SSL_CTX_free(c);
        return NULL;
    }
    SSL_CTX_set_verify(c, SSL_VERIFY_PEER, NULL);
    /* A server that closes without close_notify: the body's length says
     * whether it is all there. */
    SSL_CTX_set_options(c, SSL_OP_IGNORE_UNEXPECTED_EOF);
    ctx = c;
    return ctx;
}

/* A TCP connection to host:port with timeouts; the fd, or -1 (err set). */
static int dial(const char *host, int port, char *err, size_t errlen)
{
    char service[8];
    snprintf(service, sizeof service, "%d", port);
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res;
    int rc = getaddrinfo(host, service, &hints, &res);
    if (rc != 0) {
        snprintf(err, errlen, "%s: %s", host, gai_strerror(rc));
        return -1;
    }
    int fd = -1;
    const struct timeval tv = { TIMEOUT_SECONDS, 0 };
    for (struct addrinfo *a = res; a != NULL && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
        if (fd < 0)
            continue;
        /* On Linux the send timeout also bounds connect(). */
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0 ||
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) != 0 ||
            connect(fd, a->ai_addr, a->ai_addrlen) != 0) {
            snprintf(err, errlen, "%s: %s", host, strerror(errno));
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
    return fd;
}

/* Writes all of data. 0, or -1. */
static int send_all(SSL *ssl, const char *data, size_t len)
{
    while (len > 0) {
        int chunk = len > 1 << 20 ? 1 << 20 : (int)len;
        int n = SSL_write(ssl, data, chunk);
        if (n <= 0)
            return -1;
        data += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Reads up to len bytes: the number, 0 at the end, -1 on error. */
static long receive(SSL *ssl, char *buf, size_t len)
{
    int n = SSL_read(ssl, buf, len > 1 << 20 ? 1 << 20 : (int)len);
    if (n > 0)
        return n;
    return SSL_get_error(ssl, n) == SSL_ERROR_ZERO_RETURN ? 0 : -1;
}

/* The value of header name in the header block head (NUL-terminated), or
 * NULL. */
static const char *header(const char *head, const char *name)
{
    size_t n = strlen(name);
    for (const char *line = strstr(head, "\r\n"); line != NULL; line = strstr(line, "\r\n")) {
        line += 2;
        if (strncasecmp(line, name, n) == 0 && line[n] == ':') {
            const char *v = line + n + 1;
            while (*v == ' ' || *v == '\t')
                v++;
            return v;
        }
    }
    return NULL;
}

int https_request(const char *method, const char *url, const char *const *headers,
                  const char *type, const char *body, size_t body_len, int fd, size_t max,
                  struct https_response *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof *out);
    char host[HTTPS_MAX_HOST + 1];
    int port;
    const char *path;
    if (https_split(url, host, &port, &path) != 0) {
        snprintf(err, errlen, "not an https URL nylm can use");
        return -1;
    }
    SSL_CTX *c = tls(err, errlen);
    int sock = c != NULL ? dial(host, port, err, errlen) : -1;
    if (sock < 0)
        return -1;
    SSL *ssl = SSL_new(c);
    char *buf = NULL;
    size_t cap = 0, len = 0;
    int ok = 0;
    if (ssl == NULL || SSL_set_fd(ssl, sock) != 1 || SSL_set_tlsext_host_name(ssl, host) != 1 ||
        SSL_set1_host(ssl, host) != 1 || SSL_connect(ssl) != 1) {
        long v = ssl != NULL ? SSL_get_verify_result(ssl) : X509_V_OK;
        snprintf(err, errlen, "%s: TLS failed: %s", host,
                 v != X509_V_OK ? X509_verify_cert_error_string(v)
                                : ERR_reason_error_string(ERR_get_error()));
        goto done;
    }

    char head[8192];
    int n = snprintf(head, sizeof head,
                     "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
                     "Accept-Encoding: identity\r\n", method, path, host);
    for (size_t i = 0; headers != NULL && headers[i] != NULL && n > 0 && (size_t)n < sizeof head; i++)
        n += snprintf(head + n, sizeof head - (size_t)n, "%s\r\n", headers[i]);
    if (body != NULL && n > 0 && (size_t)n < sizeof head)
        n += snprintf(head + n, sizeof head - (size_t)n, "Content-Type: %s\r\nContent-Length: %zu\r\n",
                      type, body_len);
    if (n > 0 && (size_t)n < sizeof head)
        n += snprintf(head + n, sizeof head - (size_t)n, "\r\n");
    if (n < 0 || (size_t)n >= sizeof head) {
        snprintf(err, errlen, "request too long");
        goto done;
    }
    if (send_all(ssl, head, (size_t)n) != 0 || (body != NULL && send_all(ssl, body, body_len) != 0)) {
        snprintf(err, errlen, "%s: sending failed", host);
        goto done;
    }

    /* The status line and headers. */
    cap = HTTPS_MAX_HEAD + 1;
    buf = malloc(cap);
    char *end = NULL;
    while (buf != NULL && end == NULL) {
        long r = len < HTTPS_MAX_HEAD ? receive(ssl, buf + len, HTTPS_MAX_HEAD - len) : -1;
        if (r <= 0) {
            snprintf(err, errlen, "%s: %s", host, r == 0 ? "the answer ended early"
                                                         : "no valid answer");
            goto done;
        }
        len += (size_t)r;
        buf[len] = '\0';
        end = strstr(buf, "\r\n\r\n");
    }
    if (buf == NULL) {
        snprintf(err, errlen, "out of memory");
        goto done;
    }
    *end = '\0';
    if (sscanf(buf, "HTTP/1.%*1[01] %3d", &out->status) != 1 || out->status < 100) {
        snprintf(err, errlen, "%s: not an HTTP answer", host);
        goto done;
    }
    const char *te = header(buf, "Transfer-Encoding");
    const char *cl = header(buf, "Content-Length");
    int chunked = te != NULL && strncasecmp(te, "chunked", 7) == 0;
    long long length = -1;
    if (cl != NULL) {
        char *e;
        errno = 0;
        length = strtoll(cl, &e, 10);
        if (errno != 0 || e == cl || length < 0) {
            snprintf(err, errlen, "%s: bad Content-Length", host);
            goto done;
        }
    }
    if ((length >= 0 && (unsigned long long)length > max) || (chunked && fd >= 0)) {
        snprintf(err, errlen, "%s: %s", host, chunked ? "a chunked download" : "the answer is too big");
        goto done;
    }
    size_t start = (size_t)(end + 4 - buf);
    size_t got = len - start;

    if (fd >= 0) {
        /* Into the file: what came with the headers, then the rest. */
        size_t total = 0;
        for (;;) {
            if (got > 0) {
                total += got;
                if (total > max) {
                    snprintf(err, errlen, "%s: the answer is too big", host);
                    goto done;
                }
                for (size_t w = 0; w < got;) {
                    ssize_t k = write(fd, buf + start + w, got - w);
                    if (k < 0) {
                        snprintf(err, errlen, "writing the file: %s", strerror(errno));
                        goto done;
                    }
                    w += (size_t)k;
                }
            }
            long r = receive(ssl, buf, HTTPS_MAX_HEAD);
            if (r < 0) {
                snprintf(err, errlen, "%s: the download broke off", host);
                goto done;
            }
            if (r == 0)
                break;
            start = 0;
            got = (size_t)r;
        }
        if (length >= 0 && (unsigned long long)total != (unsigned long long)length) {
            snprintf(err, errlen, "%s: the download is incomplete", host);
            goto done;
        }
        out->len = total;
        ok = 1;
        goto done;
    }

    /* Into memory, until the server closes. */
    memmove(buf, buf + start, got);
    len = got;
    for (;;) {
        if (len == cap - 1) {
            size_t next = cap * 2;
            char *grown = cap - 1 < max + 64 ? realloc(buf, next) : NULL;
            if (grown == NULL) {
                snprintf(err, errlen, "%s: %s", host,
                         cap - 1 < max + 64 ? "out of memory" : "the answer is too big");
                goto done;
            }
            buf = grown;
            cap = next;
        }
        long r = receive(ssl, buf + len, cap - 1 - len);
        if (r < 0) {
            snprintf(err, errlen, "%s: the answer broke off", host);
            goto done;
        }
        if (r == 0)
            break;
        len += (size_t)r;
    }
    buf[len] = '\0';
    if (chunked) {
        long d = https_dechunk(buf, len);
        if (d < 0) {
            snprintf(err, errlen, "%s: bad chunked answer", host);
            goto done;
        }
        len = (size_t)d;
    }
    if ((length >= 0 && !chunked && (unsigned long long)len != (unsigned long long)length) ||
        len > max) {
        snprintf(err, errlen, "%s: %s", host, len > max ? "the answer is too big"
                                                        : "the answer is incomplete");
        goto done;
    }
    out->body = buf;
    out->len = len;
    buf = NULL;
    ok = 1;
done:
    free(buf);
    if (ssl != NULL) {
        if (ok)
            SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    close(sock);
    ERR_clear_error();
    return ok ? 0 : -1;
}
