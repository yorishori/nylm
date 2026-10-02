#include "router.h"

#include <string.h>

#include "api.h"
#include "json.h"

static void health(struct request *req, struct response *res)
{
    (void)req;
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "status", "ok");
    json_reply(res, 200, obj);
}

static const struct route routes[] = {
    { "GET",    "/api/health",   health },

    { "GET",    "/api/notes",    notes_list },
    { "POST",   "/api/notes",    notes_create },
    { "GET",    "/api/notes/:",  notes_get },
    { "PUT",    "/api/notes/:",  notes_update },
    { "DELETE", "/api/notes/:",  notes_delete },
};

/*
 * Exact match, or a pattern ending in "/:" which matches one more path
 * segment and stores it in *param ("/api/notes/:" matches "/api/notes/42").
 */
static int path_matches(const char *pattern, const char *path, const char **param)
{
    size_t len = strlen(pattern);
    if (len >= 2 && strcmp(pattern + len - 2, "/:") == 0) {
        size_t prefix = len - 1; /* keep the slash */
        if (strncmp(pattern, path, prefix) != 0)
            return 0;
        const char *rest = path + prefix;
        if (*rest == '\0' || strchr(rest, '/') != NULL)
            return 0;
        *param = rest;
        return 1;
    }
    return strcmp(pattern, path) == 0;
}

int router_find(const struct route *table, size_t n, const char *method,
                const char *path, const char **param)
{
    int result = ROUTE_NOT_FOUND;
    for (size_t i = 0; i < n; i++) {
        const char *p = NULL;
        if (!path_matches(table[i].path, path, &p))
            continue;
        if (strcmp(table[i].method, method) == 0) {
            *param = p;
            return (int)i;
        }
        result = ROUTE_METHOD_NOT_ALLOWED;
    }
    return result;
}

void router_dispatch(struct request *req, struct response *res)
{
    int i = router_find(routes, sizeof routes / sizeof routes[0], req->method, req->path,
                        &req->param);
    if (i == ROUTE_NOT_FOUND)
        json_error(res, 404, "not found");
    else if (i == ROUTE_METHOD_NOT_ALLOWED)
        json_error(res, 405, "method not allowed");
    else
        routes[i].handler(req, res);
}
