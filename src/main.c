#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "arena.h"
#include "conn.h"
#include "http.h"

#define DEFAULT_PORT 8080
#define ARENA_SIZE   (16 * 1024 * 1024)

static int parse_port(const char *s, int fallback)
{
    if (s == NULL || *s == '\0')
        return fallback;

    char *end;
    errno = 0;
    long port = strtol(s, &end, 10);
    if (errno != 0 || *end != '\0' || port < 1 || port > 65535)
        return -1;
    return (int)port;
}

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
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 16) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

static void handle(struct request *req, struct response *res)
{
    (void)req;
    http_text(res, 200, "hello\n");
}

static void serve(int client, const char *ip)
{
    double start = now_seconds();
    struct conn c;
    conn_init(&c, client, ip);

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

int main(void)
{
    int port = parse_port(getenv("NYLM_PORT"), DEFAULT_PORT);
    if (port < 0) {
        fprintf(stderr, "invalid NYLM_PORT\n");
        return 1;
    }

    if (arena_init(ARENA_SIZE) != 0) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    /* A client closing early must not kill the server. */
    signal(SIGPIPE, SIG_IGN);

    int server = listen_on(port);
    if (server < 0)
        return 1;

    printf("nylm listening on port %d\n", port);
    fflush(stdout);

    for (;;) {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof addr;
        int client = accept(server, (struct sockaddr *)&addr, &addr_len);
        if (client < 0) {
            if (errno != EINTR)
                perror("accept");
            continue;
        }
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof ip);

        serve(client, ip);
        arena_reset();
    }
}
