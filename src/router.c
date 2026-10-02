#include "router.h"

#include <string.h>

#include "api.h"
#include "auth.h"
#include "json.h"

static void health(struct request *req, struct response *res)
{
    (void)req;
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "status", "ok");
    json_reply(res, 200, obj);
}

static const struct route routes[] = {
    /* method   path             handler         public */
    { "GET",    "/api/health",   health,         1 },

    { "POST",   "/api/login",    session_login,  1 },
    { "POST",   "/api/logout",   session_logout, 1 },
    { "GET",    "/api/session",  session_check,  0 },
};

int router_find(const struct route *table, size_t n, const char *method, const char *path)
{
    int result = ROUTE_NOT_FOUND;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(table[i].path, path) != 0)
            continue;
        if (strcmp(table[i].method, method) == 0)
            return (int)i;
        result = ROUTE_METHOD_NOT_ALLOWED;
    }
    return result;
}

void router_dispatch(struct request *req, struct response *res)
{
    int i = router_find(routes, sizeof routes / sizeof routes[0], req->method, req->path);
    if (i == ROUTE_NOT_FOUND)
        json_error(res, 404, "not found");
    else if (i == ROUTE_METHOD_NOT_ALLOWED)
        json_error(res, 405, "method not allowed");
    else if (!routes[i].public && !auth_session_valid(req))
        json_error(res, 401, "login required");
    else
        routes[i].handler(req, res);
}
