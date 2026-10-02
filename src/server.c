#define _POSIX_C_SOURCE 200809L

#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "arena.h"
#include "conn.h"
#include "http.h"
#include "router.h"
#include "static.h"

static const struct server_config *cfg;
static volatile sig_atomic_t stopping;

int subnet_parse(const char *s, struct subnet *out)
{
    char addr_text[INET_ADDRSTRLEN];
    const char *slash = strchr(s, '/');
    size_t addr_len = slash ? (size_t)(slash - s) : strlen(s);
    if (addr_len == 0 || addr_len >= sizeof addr_text)
        return -1;
    memcpy(addr_text, s, addr_len);
    addr_text[addr_len] = '\0';

    struct in_addr in;
    if (inet_pton(AF_INET, addr_text, &in) != 1)
        return -1;

    long bits = 32;
    if (slash != NULL) {
        char *end;
        bits = strtol(slash + 1, &end, 10);
        if (slash[1] == '\0' || *end != '\0' || bits < 0 || bits > 32)
            return -1;
    }
    out->mask = bits == 0 ? 0 : 0xffffffffu << (32 - bits);
    out->addr = ntohl(in.s_addr) & out->mask;
    return 0;
}

int subnet_allowed(const struct subnet *list, int n, uint32_t addr)
{
    for (int i = 0; i < n; i++)
        if ((addr & list[i].mask) == list[i].addr)
            return 1;
    return 0;
}

static int listen_on(uint32_t addr, int port)
{
    char text[INET_ADDRSTRLEN];
    struct in_addr in = { .s_addr = htonl(addr) };
    inet_ntop(AF_INET, &in, text, sizeof text);

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

    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr = in,
    };
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        fprintf(stderr, "bind %s:%d: %s\n", text, port, strerror(errno));
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
    printf("nylm: listening on http://%s:%d\n", text, port);
    return fd;
}

static void handle(struct request *req, struct response *res)
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

static void serve(int client, const char *ip)
{
    double start = now_seconds();
    struct conn c;
    conn_init(&c, client);

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
    } else {
        handle(&req, &res);
    }

    long sent = http_send(&c, &res);
    if (res.file_fd >= 0)
        close(res.file_fd);
    conn_close(&c);

    printf("%s %s %s %d %ld %.1fms\n", ip, req.method, req.path, res.status, sent,
           (now_seconds() - start) * 1000.0);
    fflush(stdout);
}

static void accept_one(int listen_fd)
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

    /* Second line of defence after binding only to LAN/VPN addresses. */
    if (!subnet_allowed(cfg->allow, cfg->nallow, ntohl(addr.sin_addr.s_addr))) {
        close(client);
        printf("%s rejected: not in NYLM_ALLOW\n", ip);
        fflush(stdout);
        return;
    }

    serve(client, ip);
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

    /* SIGTERM (systemctl stop) / SIGINT: finish the current request, then
     * return. No SA_RESTART, so a blocked poll() wakes up with EINTR. */
    struct sigaction sa = { .sa_handler = on_stop_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    struct pollfd fds[SERVER_MAX_LISTEN];
    for (int i = 0; i < cfg->nlisten; i++) {
        fds[i].fd = listen_on(cfg->listen[i], cfg->port);
        fds[i].events = POLLIN;
        if (fds[i].fd < 0)
            return -1;
    }
    fflush(stdout);

    while (!stopping) {
        if (poll(fds, (nfds_t)cfg->nlisten, -1) < 0) {
            if (errno != EINTR)
                perror("poll");
            continue;
        }
        for (int i = 0; i < cfg->nlisten; i++)
            if (fds[i].revents & POLLIN)
                accept_one(fds[i].fd);
    }

    printf("nylm: shutting down\n");
    fflush(stdout);
    for (int i = 0; i < cfg->nlisten; i++)
        close(fds[i].fd);
    return 0;
}
