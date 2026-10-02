#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "arena.h"
#include "auth.h"
#include "db.h"
#include "json.h"
#include "server.h"
#include "tls.h"

#define ARENA_SIZE (16 * 1024 * 1024)

/* Port from an env var value; fallback if unset, -1 if invalid. */
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

static int cmd_serve(int tls)
{
    struct server_config cfg = {
        .public_dir = env_or("NYLM_PUBLIC", "public"),
        .tls = tls,
        .http_port = parse_port(getenv("NYLM_HTTP_PORT"), 8080),
        .https_port = parse_port(getenv("NYLM_HTTPS_PORT"), 8443),
        .acme_dir = env_or("NYLM_ACME_DIR", "acme"),
        .public_https_port = parse_port(getenv("NYLM_PUBLIC_HTTPS_PORT"), 443),
    };
    if (cfg.http_port < 0 || cfg.https_port < 0 || cfg.public_https_port < 0) {
        fprintf(stderr, "invalid port in NYLM_*_PORT\n");
        return 1;
    }

    if (tls && tls_init(env_or("NYLM_CERT", "certs/cert.pem"),
                        env_or("NYLM_KEY", "certs/key.pem")) != 0) {
        fprintf(stderr, "TLS setup failed (set NYLM_CERT/NYLM_KEY, or NYLM_TLS=off)\n");
        return 1;
    }

    if (arena_init(ARENA_SIZE) != 0) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    json_init();

    /* A client closing early must not kill the server. */
    signal(SIGPIPE, SIG_IGN);

    return server_run(&cfg) == 0 ? 0 : 1;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: nylm                 run the server\n"
            "       nylm set-password    set the login password (reads stdin)\n"
            "\n"
            "environment (defaults in brackets):\n"
            "  NYLM_DB [nylm.db]  NYLM_PUBLIC [public]  NYLM_TLS [on] | off\n"
            "  NYLM_HTTP_PORT [8080]   app when TLS is off, else redirect + ACME\n"
            "  NYLM_HTTPS_PORT [8443]  app when TLS is on\n"
            "  NYLM_CERT [certs/cert.pem]  NYLM_KEY [certs/key.pem]\n"
            "  NYLM_ACME_DIR [acme]  NYLM_PUBLIC_HTTPS_PORT [443]\n");
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage();
        return 0;
    }
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "set-password") != 0)) {
        usage();
        return 2;
    }

    int tls = strcmp(env_or("NYLM_TLS", "on"), "off") != 0;
    auth_set_secure_cookie(tls);

    if (db_open(env_or("NYLM_DB", "nylm.db")) != 0)
        return 1;

    int rc = argc == 2 ? cmd_set_password() : cmd_serve(tls);
    db_close();
    return rc;
}
