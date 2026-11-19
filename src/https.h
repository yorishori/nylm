#ifndef HTTPS_H
#define HTTPS_H

#include <stddef.h>

/*
 * A small HTTP client: one request per connection (Connection: close).
 *   https_request()       HTTPS (OpenSSL libssl) for the services that talk
 *                         to the internet (nylm-qobuz, nylm-musicbrainz): the
 *                         server's certificate checked against the system's
 *                         CAs and the host name, 30 s timeouts;
 *   http_local_request()  plain HTTP to a server on this machine (an address
 *                         in 127.0.0.0/8), 10 s timeouts: Navidrome
 *                         (src/navidrome.h), from the server and the music
 *                         services.
 */

#define HTTPS_MAX_HOST 253
#define HTTPS_MAX_HEAD (64 * 1024) /* bytes in a response's status line and headers */

/* Splits "https://host[:port]/path?query" (the path may be missing: "/").
 * 0, or -1 if it is not one (another scheme, a bad host or port, too long). */
int https_split(const char *url, char host[HTTPS_MAX_HOST + 1], int *port, const char **path);

/* Splits "http://127.x.x.x[:port]/path?query" (no port: 80; no path: "/").
 * 0, or -1 if it is not one (another scheme or host, a bad port, too long). */
int http_local_split(const char *url, char host[HTTPS_MAX_HOST + 1], int *port,
                     const char **path);

/*
 * Decodes a "Transfer-Encoding: chunked" body in place. The decoded length,
 * or -1 if it is not a complete, valid chunked body.
 */
long https_dechunk(char *buf, size_t len);

struct https_response {
    int status;
    char *body; /* malloc()ed, NUL-terminated (free it); NULL when sent to a file */
    size_t len; /* bytes of the body */
};

/*
 * Sends method url with the extra header lines (each "Name: value", the
 * list ends with NULL) and body (NULL: none; else body_len bytes with
 * Content-Type type), and reads the response. Its body goes into memory, or
 * into the file fd if fd >= 0; more than max bytes fails. 0, or -1 with
 * what went wrong in err (nothing to free then).
 */
int https_request(const char *method, const char *url, const char *const *headers,
                  const char *type, const char *body, size_t body_len, int fd, size_t max,
                  struct https_response *out, char *err, size_t errlen);

/* GET-style request (no body) to url, which http_local_split() accepts,
 * with the extra header lines (NULL-terminated list, or NULL); the body
 * goes into memory (more than max bytes fails). As https_request(). */
int http_local_request(const char *method, const char *url, const char *const *headers,
                       size_t max, struct https_response *out, char *err, size_t errlen);

#endif
