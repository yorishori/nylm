#ifndef SERVER_H
#define SERVER_H

#include <stdint.h>

#define SERVER_MAX_LISTEN 8
#define SERVER_MAX_ALLOW  8

/* An IPv4 subnet, host byte order. */
struct subnet {
    uint32_t addr;
    uint32_t mask;
};

struct server_config {
    const char *public_dir;                 /* static files */
    int port;
    uint32_t listen[SERVER_MAX_LISTEN];     /* addresses to bind, host order */
    int nlisten;
    struct subnet allow[SERVER_MAX_ALLOW];  /* accepted client subnets */
    int nallow;
};

/*
 * Parses "a.b.c.d" or "a.b.c.d/len" (host bits are masked off).
 * A bare address is a /32. Returns 0 on success, -1 if malformed.
 */
int subnet_parse(const char *s, struct subnet *out);

/* 1 if addr (host order) is inside any of the subnets. */
int subnet_allowed(const struct subnet *list, int n, uint32_t addr);

/* Serves until SIGTERM/SIGINT (returns 0); -1 on startup failure. */
int server_run(const struct server_config *cfg);

#endif
