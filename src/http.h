#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

#include "conn.h"

#define HTTP_MAX_HEAD    8192          /* request line + headers */
#define HTTP_MAX_BODY    (1024 * 1024) /* 1 MiB */
#define HTTP_MAX_HEADERS 64
#define HTTP_MAX_PATH    2048
#define HTTP_MAX_EXTRA   8             /* extra response headers */

struct header {
    const char *name;
    const char *value;
};

struct request {
    const char *method;
    const char *path;  /* URL-decoded, no query string */
    const char *query; /* raw text after '?', NULL if none */
    struct header headers[HTTP_MAX_HEADERS];
    size_t nheaders;
    size_t content_length;
    char *body;        /* NUL-terminated, NULL if no body */
    size_t body_len;
};

struct response {
    int status;
    const char *content_type;
    const char *cache_control; /* NULL means "no-store" */
    const char *body;
    size_t body_len;
    int file_fd;       /* >= 0: send file_size bytes from this fd as body */
    size_t file_size;
    struct header extra[HTTP_MAX_EXTRA];
    size_t nextra;
};

/*
 * Parses the request head in buf (len bytes, ending in "\r\n\r\n") into req.
 * Modifies buf in place. Returns 0 on success or the HTTP status to reply with.
 */
int http_parse_head(char *buf, size_t len, struct request *req);

/*
 * Reads a full request from c. Returns 0 on success, an HTTP status to reply
 * with, or -1 if the connection is dead (nothing should be sent).
 */
int http_read_request(struct conn *c, struct request *req);

/* Decodes %XX escapes in place. Rejects bad escapes and control characters. */
int http_url_decode(char *s);

/*
 * Finds the first name=value pair in the query string and URL-decodes the
 * value into the arena. Returns 0 (found), 1 (absent) or -1 (badly escaped,
 * or out of memory).
 */
int http_query(const struct request *req, const char *name, const char **value);

/* Case-insensitive header lookup; NULL if absent. */
const char *http_header(const struct request *req, const char *name);

const char *http_status_text(int status);

void http_response_init(struct response *res);
/* Adds a response header; -1 if all HTTP_MAX_EXTRA slots are used. */
int http_add_header(struct response *res, const char *name, const char *value);
void http_text(struct response *res, int status, const char *text);

/* Writes the response; returns bytes of body sent, or -1 on error. */
long http_send(struct conn *c, const struct response *res);

#endif
