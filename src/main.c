#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_PORT 8080

static const char RESPONSE[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 6\r\n"
    "Connection: close\r\n"
    "\r\n"
    "hello\n";

static int parse_port(const char *s)
{
    if (s == NULL || *s == '\0')
        return DEFAULT_PORT;

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

static void write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        buf += n;
        len -= (size_t)n;
    }
}

static void handle(int client)
{
    /* Request is read and ignored for now; parsing comes in milestone 2. */
    char buf[4096];
    ssize_t n = read(client, buf, sizeof buf);
    if (n <= 0)
        return;

    write_all(client, RESPONSE, sizeof RESPONSE - 1);
}

int main(void)
{
    int port = parse_port(getenv("NYLM_PORT"));
    if (port < 0) {
        fprintf(stderr, "invalid NYLM_PORT\n");
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
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            if (errno != EINTR)
                perror("accept");
            continue;
        }
        handle(client);
        close(client);
    }
}
