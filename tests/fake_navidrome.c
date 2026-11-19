/*
 * A stand-in for Navidrome in tests/smoke.sh: `fake_navidrome PORT DIR`
 * answers GET .../rest/METHOD?... on 127.0.0.1:PORT, one request at a
 * time, until it is killed.
 *   - the token must be the MD5 of DIR/password (if that file exists) and
 *     the salt s, else it answers Navidrome's "wrong username or password";
 *   - the answer is DIR/METHOD.N.json for the Nth call of METHOD if that
 *     file exists, else DIR/METHOD.json, else 404; bodies over 512 bytes
 *     are sent chunked, as Navidrome does;
 *   - each request's "METHOD QUERY" is appended to DIR/requests.log.
 */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/evp.h>

#define MAX_METHODS 64

static const char *dir;
static char methods[MAX_METHODS][64];
static int calls[MAX_METHODS];

/* The number of this call of method (1 for the first). */
static int count_call(const char *method)
{
    for (int i = 0; i < MAX_METHODS; i++) {
        if (methods[i][0] == '\0')
            snprintf(methods[i], sizeof methods[i], "%s", method);
        if (strcmp(methods[i], method) == 0)
            return ++calls[i];
    }
    return 0;
}

/* The contents of DIR/name in a malloc()ed buffer, or NULL. */
static char *slurp(const char *name, size_t *len)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    char *buf = malloc(1 << 22);
    *len = buf != NULL ? fread(buf, 1, (1 << 22) - 1, f) : 0;
    fclose(f);
    if (buf != NULL)
        buf[*len] = '\0';
    return buf;
}

/* The value of name in the query q into out ("" if absent). */
static void param(const char *q, const char *name, char *out, size_t outlen)
{
    size_t n = strlen(name);
    out[0] = '\0';
    for (const char *p = q; p != NULL && *p != '\0'; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (strncmp(p, name, n) == 0 && p[n] == '=') {
            size_t len = strcspn(p + n + 1, "& ");
            snprintf(out, outlen, "%.*s", (int)len, p + n + 1);
            return;
        }
    }
}

/* 1 if the query's token t is the MD5 of DIR/password and its salt s. */
static int token_ok(const char *q)
{
    size_t len;
    char *password = slurp("password", &len);
    if (password == NULL)
        return 1;
    while (len > 0 && password[len - 1] == '\n')
        password[--len] = '\0';
    char salt[64], token[64], joined[512], want[33];
    param(q, "s", salt, sizeof salt);
    param(q, "t", token, sizeof token);
    snprintf(joined, sizeof joined, "%s%s", password, salt);
    free(password);
    unsigned char md5[EVP_MAX_MD_SIZE];
    unsigned n = 0;
    EVP_Digest(joined, strlen(joined), md5, &n, EVP_md5(), NULL);
    for (unsigned i = 0; i < 16; i++)
        snprintf(want + 2 * i, 3, "%02x", md5[i]);
    return salt[0] != '\0' && strcmp(token, want) == 0;
}

static void send_all(int fd, const char *s, size_t len)
{
    while (len > 0) {
        ssize_t n = send(fd, s, len, MSG_NOSIGNAL);
        if (n <= 0)
            return;
        s += n;
        len -= (size_t)n;
    }
}

static void answer(int fd, int status, const char *body, size_t len)
{
    char head[256];
    int chunked = len > 512;
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nConnection: close\r\n",
                     status, status == 200 ? "OK" : "Not Found");
    n += chunked ? snprintf(head + n, sizeof head - (size_t)n, "Transfer-Encoding: chunked\r\n\r\n")
                 : snprintf(head + n, sizeof head - (size_t)n, "Content-Length: %zu\r\n\r\n", len);
    send_all(fd, head, (size_t)n);
    if (!chunked) {
        send_all(fd, body, len);
        return;
    }
    /* Two chunks and the end. */
    size_t half = len / 2;
    char size[32];
    n = snprintf(size, sizeof size, "%zx\r\n", half);
    send_all(fd, size, (size_t)n);
    send_all(fd, body, half);
    n = snprintf(size, sizeof size, "\r\n%zx\r\n", len - half);
    send_all(fd, size, (size_t)n);
    send_all(fd, body + half, len - half);
    send_all(fd, "\r\n0\r\n\r\n", 7);
}

static void serve(int fd)
{
    char req[8192] = "";
    size_t got = 0;
    while (got < sizeof req - 1 && strstr(req, "\r\n\r\n") == NULL) {
        ssize_t n = recv(fd, req + got, sizeof req - 1 - got, 0);
        if (n <= 0)
            return;
        got += (size_t)n;
        req[got] = '\0';
    }
    char method[64] = "", query[4096] = "";
    const char *rest = strstr(req, "/rest/");
    const char *eol = strstr(req, "\r\n");
    if (strncmp(req, "GET ", 4) != 0 || rest == NULL || eol == NULL || rest > eol) {
        answer(fd, 404, "not found", 9);
        return;
    }
    size_t m = strcspn(rest + 6, "? ");
    snprintf(method, sizeof method, "%.*s", (int)(m < 63 ? m : 63), rest + 6);
    if (rest[6 + m] == '?')
        snprintf(query, sizeof query, "%.*s", (int)strcspn(rest + 7 + m, " "), rest + 7 + m);
    char log[1024];
    snprintf(log, sizeof log, "%s/requests.log", dir);
    FILE *f = fopen(log, "a");
    if (f != NULL) {
        fprintf(f, "%s %s\n", method, query);
        fclose(f);
    }
    if (!token_ok(query)) {
        const char *wrong = "{\"subsonic-response\":{\"status\":\"failed\",\"version\":\"1.16.1\","
                            "\"error\":{\"code\":40,\"message\":\"Wrong username or password\"}}}";
        answer(fd, 200, wrong, strlen(wrong));
        return;
    }
    char name[128];
    size_t len;
    snprintf(name, sizeof name, "%s.%d.json", method, count_call(method));
    char *body = slurp(name, &len);
    if (body == NULL) {
        snprintf(name, sizeof name, "%s.json", method);
        body = slurp(name, &len);
    }
    if (body == NULL)
        answer(fd, 404, "not found", 9);
    else
        answer(fd, 200, body, len);
    free(body);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: fake_navidrome PORT DIR\n");
        return 2;
    }
    dir = argv[2];
    char *end;
    long port = strtol(argv[1], &end, 10);
    if (*end != '\0' || port < 1 || port > 65535) {
        fprintf(stderr, "fake_navidrome: bad port\n");
        return 2;
    }
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s < 0 || setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0 ||
        bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 8) != 0) {
        perror("fake_navidrome");
        return 1;
    }
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0)
            continue;
        serve(c);
        close(c);
    }
}
