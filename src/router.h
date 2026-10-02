#ifndef ROUTER_H
#define ROUTER_H

#include <stddef.h>

#include "http.h"

typedef void (*handler_fn)(struct request *req, struct response *res);

struct route {
    const char *method;
    const char *path; /* exact, or "/prefix/:" - see path_matches() */
    handler_fn handler;
    int public; /* 1: no login needed */
};

#define ROUTE_NOT_FOUND          (-1)
#define ROUTE_METHOD_NOT_ALLOWED (-2)

/* Index of the matching route, or one of the ROUTE_* codes. */
int router_find(const struct route *routes, size_t n, const char *method,
                const char *path, const char **param);

/*
 * Handles an /api/ request with the application's route table. Routes not
 * marked public answer 401 unless the request has a valid session.
 */
void router_dispatch(struct request *req, struct response *res);

#endif
