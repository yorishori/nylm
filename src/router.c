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
    /* method   path                          handler              public */
    { "GET",    "/api/health",                health,              1 },

    { "POST",   "/api/login",                 session_login,       1 },
    { "POST",   "/api/logout",                session_logout,      1 },
    { "GET",    "/api/session",               session_check,       0 },

    { "GET",    "/api/plants",                plants_list,         0 },
    { "GET",    "/api/plants/due",            plants_due,          0 },
    { "GET",    "/api/plants/plant",          plants_get,          0 },
    { "POST",   "/api/plants/add",            plants_add,          0 },
    { "POST",   "/api/plants/update",         plants_update,       0 },
    { "POST",   "/api/plants/archive",        plants_archive,      0 },
    { "POST",   "/api/plants/types/add",      plants_type_add,     0 },
    { "POST",   "/api/plants/types/update",   plants_type_update,  0 },
    { "POST",   "/api/plants/types/archive",  plants_type_archive, 0 },
    { "POST",   "/api/plants/rules/save",     plants_rule_save,    0 },
    { "POST",   "/api/plants/rules/delete",   plants_rule_delete,  0 },
    { "GET",    "/api/plants/log",            plants_log,          0 },
    { "POST",   "/api/plants/log/add",        plants_log_add,      0 },
    { "POST",   "/api/plants/log/update",     plants_log_update,   0 },
    { "POST",   "/api/plants/log/delete",     plants_log_delete,   0 },

    { "GET",    "/api/music",                 music_overview,      0 },
    { "GET",    "/api/music/albums",          music_albums,        0 },
    { "GET",    "/api/music/album",           music_album,         0 },
    { "GET",    "/api/music/values",          music_values,        0 },
    { "GET",    "/api/music/charts",          music_charts,        0 },
    { "GET",    "/api/music/changes",         music_changes,       0 },
    { "GET",    "/api/music/art",             music_art,           0 },
    { "POST",   "/api/music/queue",           music_queue,         0 },
    { "POST",   "/api/music/cover",           music_cover,         0 },
    { "POST",   "/api/music/discard",         music_discard,       0 },
    { "POST",   "/api/music/scan",            music_scan_start,    0 },
    { "POST",   "/api/music/write",           music_write_start,   0 },
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
    else if (routes[i].public)
        routes[i].handler(req, res);
    else {
        int valid = auth_session_valid(req);
        if (valid < 0)
            json_error(res, 500, "internal error");
        else if (valid == 0)
            json_error(res, 401, "login required");
        else
            routes[i].handler(req, res);
    }
}
