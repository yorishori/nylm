#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include "arena.h"
#include "auth.h"
#include "conn.h"
#include "db.h"
#include "http.h"
#include "json.h"
#include "router.h"
#include "static.h"

#define DEFAULT_PORT 8080
#define ARENA_SIZE   (16 * 1024 * 1024)

static const char *public_dir;

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
    if (strncmp(req->path, "/api/", 5) == 0) {
        router_dispatch(req, res);
    } else if (strcmp(req->method, "GET") == 0) {
        static_serve(public_dir, req->path, res);
    } else {
        http_text(res, 405, "405 Method Not Allowed\n");
        http_add_header(res, "Allow", "GET");
    }
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

static const char *env_or(const char *name, const char *fallback)
{
    const char *v = getenv(name);
    return v != NULL && *v != '\0' ? v : fallback;
}

/* Reads one line into buf without echo when stdin is a terminal. */
static int read_password(const char *prompt, char *buf, size_t size)
{
    int tty = isatty(STDIN_FILENO);
    struct termios old, quiet;
    if (tty) {
        fprintf(stderr, "%s", prompt);
        tcgetattr(STDIN_FILENO, &old);
        quiet = old;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
    }
    char *line = fgets(buf, (int)size, stdin);
    if (tty) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
        fprintf(stderr, "\n");
    }
    if (line == NULL)
        return -1;
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n')
        buf[--len] = '\0';
    else if (len == size - 1)
        return -1; /* too long */
    return 0;
}

static int cmd_set_password(void)
{
    char pw[AUTH_MAX_PASSWORD + 2], again[AUTH_MAX_PASSWORD + 2];
    if (read_password("new password: ", pw, sizeof pw) != 0) {
        fprintf(stderr, "could not read password (max %d bytes)\n", AUTH_MAX_PASSWORD);
        return 1;
    }
    if (strlen(pw) < AUTH_MIN_PASSWORD) {
        fprintf(stderr, "password must be at least %d bytes\n", AUTH_MIN_PASSWORD);
        return 1;
    }
    if (isatty(STDIN_FILENO)) {
        if (read_password("again: ", again, sizeof again) != 0 || strcmp(pw, again) != 0) {
            fprintf(stderr, "passwords do not match\n");
            return 1;
        }
    }
    if (auth_set_password(pw) != 0) {
        fprintf(stderr, "failed to set password\n");
        return 1;
    }
    fprintf(stderr, "password set; all sessions logged out\n");
    return 0;
}

static int cmd_serve(void)
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
    json_init();

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

static void usage(void)
{
    fprintf(stderr,
            "usage: nylm                 run the server\n"
            "       nylm set-password    set the login password (reads stdin)\n");
}

int main(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "set-password") != 0)) {
        usage();
        return 2;
    }

    public_dir = env_or("NYLM_PUBLIC", "public");
    int tls = strcmp(env_or("NYLM_TLS", "on"), "off") != 0;
    auth_set_secure_cookie(tls);

    if (db_open(env_or("NYLM_DB", "nylm.db")) != 0)
        return 1;

    int rc = argc == 2 ? cmd_set_password() : cmd_serve();
    db_close();
    return rc;
}
