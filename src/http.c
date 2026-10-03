#define _POSIX_C_SOURCE 200809L

#include "http.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "arena.h"

/* RFC 9110 token characters (header names). */
static int is_tchar(char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return 1;
    return c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int http_url_decode(char *s)
{
    char *out = s;
    for (char *in = s; *in != '\0'; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '%') {
            int hi = hex_value(in[1]);
            int lo = hi < 0 ? -1 : hex_value(in[2]);
            if (lo < 0)
                return -1;
            c = (unsigned char)(hi * 16 + lo);
            in += 2;
        }
        if (c < 0x20 || c == 0x7f)
            return -1;
        *out++ = (char)c;
    }
    *out = '\0';
    return 0;
}

/* Splits off one CRLF-terminated line starting at *p; NULL if no CRLF. */
static char *next_line(char **p, char *end)
{
    char *line = *p;
    for (char *q = line; q + 1 < end; q++) {
        if (q[0] == '\r' && q[1] == '\n') {
            *q = '\0';
            *p = q + 2;
            return line;
        }
    }
    return NULL;
}

static int parse_request_line(char *line, struct request *req)
{
    char *sp1 = strchr(line, ' ');
    if (sp1 == NULL)
        return 400;
    *sp1 = '\0';
    char *target = sp1 + 1;
    char *sp2 = strchr(target, ' ');
    if (sp2 == NULL)
        return 400;
    *sp2 = '\0';
    char *version = sp2 + 1;

    if (strcmp(version, "HTTP/1.1") != 0 && strcmp(version, "HTTP/1.0") != 0)
        return strncmp(version, "HTTP/", 5) == 0 ? 505 : 400;

    if (strcmp(line, "GET") != 0 && strcmp(line, "POST") != 0 &&
        strcmp(line, "PUT") != 0 && strcmp(line, "DELETE") != 0) {
        for (char *m = line; *m != '\0'; m++)
            if (!is_tchar(*m))
                return 400;
        return 501;
    }
    req->method = line;

    if (target[0] != '/')
        return 400;
    if (strlen(target) > HTTP_MAX_PATH)
        return 414;

    char *q = strchr(target, '?');
    if (q != NULL) {
        *q = '\0';
        req->query = q + 1;
    }
    if (http_url_decode(target) != 0)
        return 400;
    req->path = target;
    return 0;
}

static int parse_content_length(const char *v, size_t *out)
{
    size_t n = 0;
    if (*v == '\0')
        return 400;
    for (; *v != '\0'; v++) {
        if (*v < '0' || *v > '9')
            return 400;
        n = n * 10 + (size_t)(*v - '0');
        if (n > HTTP_MAX_BODY)
            return 413;
    }
    *out = n;
    return 0;
}

int http_parse_head(char *buf, size_t len, struct request *req)
{
    memset(req, 0, sizeof *req);
    char *p = buf;
    char *end = buf + len;

    char *line = next_line(&p, end);
    if (line == NULL)
        return 400;
    int status = parse_request_line(line, req);
    if (status != 0)
        return status;

    int have_length = 0;
    while ((line = next_line(&p, end)) != NULL && line[0] != '\0') {
        char *colon = strchr(line, ':');
        if (colon == NULL || colon == line)
            return 400;
        for (char *n = line; n < colon; n++)
            if (!is_tchar(*n))
                return 400; /* also rejects obsolete line folding */
        *colon = '\0';

        char *value = colon + 1;
        while (*value == ' ' || *value == '\t')
            value++;
        char *vend = value + strlen(value);
        while (vend > value && (vend[-1] == ' ' || vend[-1] == '\t'))
            *--vend = '\0';
        for (char *v = value; v < vend; v++)
            if (((unsigned char)*v < 0x20 && *v != '\t') || *v == 0x7f)
                return 400;

        if (req->nheaders == HTTP_MAX_HEADERS)
            return 431;
        req->headers[req->nheaders].name = line;
        req->headers[req->nheaders].value = value;
        req->nheaders++;

        if (strcasecmp(line, "Transfer-Encoding") == 0)
            return 501; /* no chunked bodies */
        if (strcasecmp(line, "Content-Length") == 0) {
            size_t n;
            status = parse_content_length(value, &n);
            if (status != 0)
                return status;
            if (have_length && n != req->content_length)
                return 400;
            req->content_length = n;
            have_length = 1;
        }
    }
    if (line == NULL)
        return 400; /* head did not end with an empty line */
    return 0;
}

/* Index just past "\r\n\r\n" in buf[0..len), or 0 if not present. */
static size_t find_head_end(const char *buf, size_t len)
{
    for (size_t i = 3; i < len; i++)
        if (buf[i - 3] == '\r' && buf[i - 2] == '\n' && buf[i - 1] == '\r' && buf[i] == '\n')
            return i + 1;
    return 0;
}

int http_read_request(struct conn *c, struct request *req)
{
    char *buf = arena_alloc(HTTP_MAX_HEAD);
    if (buf == NULL)
        return 500;

    size_t len = 0, head_len = 0;
    while (head_len == 0) {
        if (len == HTTP_MAX_HEAD)
            return 431;
        ssize_t n = conn_read(c, buf + len, HTTP_MAX_HEAD - len);
        if (n <= 0)
            return len == 0 ? -1 : 408;
        len += (size_t)n;
        head_len = find_head_end(buf, len);
    }

    int status = http_parse_head(buf, head_len, req);
    if (status != 0)
        return status;
    if (req->content_length == 0)
        return 0;

    req->body = arena_alloc(req->content_length + 1);
    if (req->body == NULL)
        return 500;
    size_t have = len - head_len;
    if (have > req->content_length)
        have = req->content_length; /* ignore anything after the body */
    memcpy(req->body, buf + head_len, have);
    while (have < req->content_length) {
        ssize_t n = conn_read(c, req->body + have, req->content_length - have);
        if (n <= 0)
            return 408;
        have += (size_t)n;
    }
    req->body[have] = '\0';
    req->body_len = have;
    return 0;
}

int http_query(const struct request *req, const char *name, const char **value)
{
    size_t name_len = strlen(name);
    for (const char *p = req->query; p != NULL && *p != '\0';) {
        size_t len = strcspn(p, "&");
        if (len > name_len && strncmp(p, name, name_len) == 0 && p[name_len] == '=') {
            char *v = arena_strndup(p + name_len + 1, len - name_len - 1);
            if (v == NULL || http_url_decode(v) != 0)
                return -1;
            *value = v;
            return 0;
        }
        p += len;
        if (*p == '&')
            p++;
    }
    return 1;
}

const char *http_header(const struct request *req, const char *name)
{
    for (size_t i = 0; i < req->nheaders; i++)
        if (strcasecmp(req->headers[i].name, name) == 0)
            return req->headers[i].value;
    return NULL;
}

const char *http_status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 403: return "Forbidden";
    case 422: return "Unprocessable Content";
    case 503: return "Service Unavailable";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 408: return "Request Timeout";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

void http_response_init(struct response *res)
{
    memset(res, 0, sizeof *res);
    res->status = 200;
    res->file_fd = -1;
}

int http_add_header(struct response *res, const char *name, const char *value)
{
    if (res->nextra == HTTP_MAX_EXTRA) {
        fprintf(stderr, "http: too many response headers, dropped %s\n", name);
        return -1;
    }
    res->extra[res->nextra].name = name;
    res->extra[res->nextra].value = value;
    res->nextra++;
    return 0;
}

void http_text(struct response *res, int status, const char *text)
{
    res->status = status;
    res->content_type = "text/plain; charset=utf-8";
    res->body = text;
    res->body_len = strlen(text);
}

long http_send(struct conn *c, const struct response *res)
{
    size_t body_len = res->file_fd >= 0 ? res->file_size : res->body_len;

    char head[2048];
    int n = snprintf(head, sizeof head,
        "HTTP/1.1 %d %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Cache-Control: %s\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "Content-Security-Policy: default-src 'self'; frame-ancestors 'none'; "
        "base-uri 'none'; form-action 'self'\r\n",
        res->status, http_status_text(res->status), body_len,
        res->cache_control ? res->cache_control : "no-store");
    if (res->content_type != NULL && n > 0 && (size_t)n < sizeof head)
        n += snprintf(head + n, sizeof head - (size_t)n, "Content-Type: %s\r\n",
                      res->content_type);
    for (size_t i = 0; i < res->nextra && n > 0 && (size_t)n < sizeof head; i++)
        n += snprintf(head + n, sizeof head - (size_t)n, "%s: %s\r\n",
                      res->extra[i].name, res->extra[i].value);
    if (n < 0 || (size_t)n + 2 >= sizeof head)
        return -1; /* header block too large: a bug, not client input */
    memcpy(head + n, "\r\n", 2);
    n += 2;

    if (conn_write(c, head, (size_t)n) != 0)
        return -1;

    if (res->file_fd < 0) {
        if (body_len > 0 && conn_write(c, res->body, body_len) != 0)
            return -1;
        return (long)body_len;
    }

    char chunk[16384];
    size_t sent = 0;
    while (sent < body_len) {
        ssize_t r = read(res->file_fd, chunk, sizeof chunk);
        if (r <= 0 || conn_write(c, chunk, (size_t)r) != 0)
            return -1;
        sent += (size_t)r;
    }
    return (long)sent;
}
