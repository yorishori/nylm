#define _POSIX_C_SOURCE 200809L

#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "arena.h"
#include "conn.h"
#include "http.h"
#include "router.h"
#include "static.h"
#include "tls.h"

#define ACME_PREFIX "/.well-known/acme-challenge/"

enum listener { APP, REDIRECT };

static const struct server_config *cfg;
static volatile sig_atomic_t stopping;
static char acme_challenge_dir[1024];

static int listen_on(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        fprintf(stderr, "bind port %d: %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }

    /* Non-blocking, so accept() after poll() can't hang if the client already
     * left. Accepted sockets do not inherit this on Linux. */
    if (listen(fd, 16) < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

static void handle_app(struct request *req, struct response *res)
{
    if (strncmp(req->path, "/api/", 5) == 0) {
        router_dispatch(req, res);
    } else if (strcmp(req->method, "GET") == 0) {
        static_serve(cfg->public_dir, req->path, res);
    } else {
        http_text(res, 405, "405 Method Not Allowed\n");
        http_add_header(res, "Allow", "GET");
    }
}

/* Appends s to out, %-encoding anything outside the allowed set. */
static size_t append_encoded(char *out, size_t pos, size_t cap, const char *s,
                             const char *allowed)
{
    static const char hex[] = "0123456789ABCDEF";
    for (; *s != '\0' && pos + 4 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            strchr(allowed, c) != NULL) {
            out[pos++] = (char)c;
        } else {
            out[pos++] = '%';
            out[pos++] = hex[c >> 4];
            out[pos++] = hex[c & 15];
        }
    }
    out[pos] = '\0';
    return pos;
}

char *server_redirect_location(const char *host, const char *path, const char *query,
                               int https_port)
{
    if (host == NULL)
        return NULL;
    size_t host_len = strcspn(host, ":"); /* drop any port */
    if (host_len == 0 || host_len > 253)
        return NULL;
    for (size_t i = 0; i < host_len; i++) {
        char c = host[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-'))
            return NULL;
    }

    size_t cap = 512 + 3 * (strlen(path) + strlen(query));
    char *out = arena_alloc(cap);
    if (out == NULL)
        return NULL;
    int n = https_port == 443
                ? snprintf(out, cap, "https://%.*s", (int)host_len, host)
                : snprintf(out, cap, "https://%.*s:%d", (int)host_len, host, https_port);
    size_t pos = append_encoded(out, (size_t)n, cap, path, "-._~/!$&'()*+,;=:@");
    if (*query != '\0') {
        out[pos++] = '?';
        append_encoded(out, pos, cap, query, "-._~/!$&'()*+,;=:@?%");
    }
    return out;
}

/* Port 80 when TLS is on: ACME challenges, everything else goes to HTTPS. */
static void handle_redirect(struct request *req, struct response *res)
{
    if (strncmp(req->path, ACME_PREFIX, strlen(ACME_PREFIX)) == 0) {
        const char *token = req->path + strlen(ACME_PREFIX);
        char rel[256];
        if (strcmp(req->method, "GET") != 0 || strlen(token) + 2 > sizeof rel ||
            strspn(token, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
                strlen(token)) {
            http_text(res, 404, "404 Not Found\n");
            return;
        }
        snprintf(rel, sizeof rel, "/%s", token);
        static_serve(acme_challenge_dir, rel, res);
        res->content_type = "text/plain";
        return;
    }

    char *location = server_redirect_location(http_header(req, "Host"), req->path,
                                              req->query, cfg->public_https_port);
    if (location == NULL) {
        http_text(res, 400, "400 Bad Request\n");
        return;
    }
    http_text(res, 301, "Moved to HTTPS\n");
    http_add_header(res, "Location", location);
}

static void serve(int client, const char *ip, enum listener which)
{
    double start = now_seconds();
    struct conn c;
    conn_init(&c, client, ip);

    if (which == APP && cfg->tls && tls_accept(&c) != 0) {
        conn_close(&c);
        return;
    }

    struct request req;
    struct response res;
    http_response_init(&res);

    int status = http_read_request(&c, &req);
    if (status < 0) {
        conn_close(&c);
        return;
    }
    if (status > 0) {
        char *text = arena_alloc(64);
        if (text != NULL)
            snprintf(text, 64, "%d %s\n", status, http_status_text(status));
        http_text(&res, status, text != NULL ? text : "error\n");
        req.method = "-";
        req.path = "-";
    } else if (which == APP) {
        handle_app(&req, &res);
    } else {
        handle_redirect(&req, &res);
    }

    long sent = http_send(&c, &res);
    if (res.file_fd >= 0)
        close(res.file_fd);
    conn_close(&c);

    printf("%s %s %s %s %d %ld %.1fms\n", ip, which == APP ? "app" : "redirect", req.method,
           req.path, res.status, sent, (now_seconds() - start) * 1000.0);
    fflush(stdout);
}

static void accept_one(int listen_fd, enum listener which)
{
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof addr;
    int client = accept(listen_fd, (struct sockaddr *)&addr, &addr_len);
    if (client < 0) {
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            perror("accept");
        return;
    }
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof ip);

    serve(client, ip, which);
    arena_reset();
}

static void on_stop_signal(int sig)
{
    (void)sig;
    stopping = 1;
}

int server_run(const struct server_config *config)
{
    cfg = config;

    /* SIGTERM (docker stop) / SIGINT: finish the current request, then return.
     * No SA_RESTART, so a blocked poll() wakes up with EINTR. */
    struct sigaction sa = { .sa_handler = on_stop_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    http_set_hsts(cfg->tls);
    snprintf(acme_challenge_dir, sizeof acme_challenge_dir, "%s%.*s", cfg->acme_dir,
             (int)strlen(ACME_PREFIX) - 1, ACME_PREFIX);

    /* fds[0] serves the app; fds[1] (TLS only) is the port-80 redirector. */
    struct pollfd fds[2] = { { .fd = -1 }, { .fd = -1 } };
    nfds_t nfds = 1;
    if (cfg->tls) {
        fds[0].fd = listen_on(cfg->https_port);
        fds[1].fd = listen_on(cfg->http_port);
        nfds = 2;
        if (fds[0].fd < 0 || fds[1].fd < 0)
            return -1;
        printf("nylm: https on %d, http->https redirect on %d\n", cfg->https_port,
               cfg->http_port);
    } else {
        fds[0].fd = listen_on(cfg->http_port);
        if (fds[0].fd < 0)
            return -1;
        printf("nylm: plain http on %d (NYLM_TLS=off)\n", cfg->http_port);
    }
    fflush(stdout);

    fds[0].events = fds[1].events = POLLIN;
    while (!stopping) {
        if (poll(fds, nfds, -1) < 0) {
            if (errno != EINTR)
                perror("poll");
            continue;
        }
        if (fds[0].revents & POLLIN)
            accept_one(fds[0].fd, APP);
        if (nfds == 2 && (fds[1].revents & POLLIN))
            accept_one(fds[1].fd, REDIRECT);
    }
    printf("nylm: shutting down\n");
    fflush(stdout);
    close(fds[0].fd);
    if (fds[1].fd >= 0)
        close(fds[1].fd);
    return 0;
}
