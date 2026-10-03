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
#include "music.h"
#include "server.h"

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
        /* Never read a password from a terminal that would echo it. */
        if (tcgetattr(STDIN_FILENO, &old) != 0) {
            perror("tcgetattr");
            return -1;
        }
        quiet = old;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet) != 0) {
            perror("tcsetattr");
            return -1;
        }
        fprintf(stderr, "%s", prompt);
    }
    char *line = fgets(buf, (int)size, stdin);
    if (tty) {
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &old) != 0)
            perror("tcsetattr: terminal echo may still be off");
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

/*
 * Splits a space-separated env var and parses each item with subnet_parse().
 * For NYLM_LISTEN only bare addresses are accepted.
 */
static int parse_list(const char *name, const char *value, struct subnet *out, int max,
                      int addresses_only)
{
    char copy[512];
    if (snprintf(copy, sizeof copy, "%s", value) >= (int)sizeof copy) {
        fprintf(stderr, "%s is too long\n", name);
        return -1;
    }
    int n = 0;
    char *save = NULL;
    for (char *item = strtok_r(copy, " ", &save); item != NULL;
         item = strtok_r(NULL, " ", &save)) {
        if (n == max) {
            fprintf(stderr, "%s: at most %d entries\n", name, max);
            return -1;
        }
        if ((addresses_only && strchr(item, '/') != NULL) || subnet_parse(item, &out[n]) != 0) {
            fprintf(stderr, "%s: invalid entry '%s'\n", name, item);
            return -1;
        }
        n++;
    }
    if (n == 0) {
        fprintf(stderr, "%s is empty\n", name);
        return -1;
    }
    return n;
}

static int cmd_serve(void)
{
    struct server_config cfg = {
        .public_dir = env_or("NYLM_PUBLIC", "public"),
        .port = parse_port(getenv("NYLM_PORT"), 8080),
    };
    if (cfg.port < 0) {
        fprintf(stderr, "invalid NYLM_PORT\n");
        return 1;
    }

    struct subnet listen[SERVER_MAX_LISTEN];
    cfg.nlisten = parse_list("NYLM_LISTEN", env_or("NYLM_LISTEN", "127.0.0.1"), listen,
                             SERVER_MAX_LISTEN, 1);
    cfg.nallow = parse_list("NYLM_ALLOW", env_or("NYLM_ALLOW", "127.0.0.0/8"), cfg.allow,
                            SERVER_MAX_ALLOW, 0);
    if (cfg.nlisten < 0 || cfg.nallow < 0)
        return 1;
    for (int i = 0; i < cfg.nlisten; i++)
        cfg.listen[i] = listen[i].addr;

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
            "       nylm music-scan      read new and changed music files into the cache\n"
            "\n"
            "environment (defaults in brackets):\n"
            "  NYLM_DATA    (required)     data folder; must exist. Each app gets its own\n"
            "                              subfolder and database in it\n"
            "  NYLM_MUSIC   (none)         music library folder; unset: no music app\n"
            "  NYLM_PUBLIC  [public]       static files directory\n"
            "  NYLM_PORT    [8080]         port to listen on\n"
            "  NYLM_LISTEN  [127.0.0.1]    addresses to listen on, e.g. \"10.0.0.1 192.168.1.5\"\n"
            "  NYLM_ALLOW   [127.0.0.0/8]  client subnets accepted, e.g. \"10.0.0.0/24 192.168.1.0/24\"\n");
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage();
        return 0;
    }
    const char *cmd = argc == 2 ? argv[1] : "";
    if (argc > 2 || (argc == 2 && strcmp(cmd, "set-password") != 0 &&
                     strcmp(cmd, "music-scan") != 0)) {
        usage();
        return 2;
    }

    if (db_open_all(getenv("NYLM_DATA")) != 0)
        return 1;
    if (music_configure(getenv("NYLM_MUSIC"), getenv("NYLM_DATA")) != 0) {
        db_close_all();
        return 1;
    }

    int rc;
    if (strcmp(cmd, "set-password") == 0)
        rc = cmd_set_password();
    else if (strcmp(cmd, "music-scan") == 0)
        rc = arena_init(ARENA_SIZE) == 0 ? music_scan() : 1;
    else
        rc = cmd_serve();
    db_close_all();
    return rc;
}
