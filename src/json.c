#define _POSIX_C_SOURCE 200809L

#include "json.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "arena.h"

static void no_free(void *p)
{
    (void)p; /* freed with the arena at the end of the request */
}

void json_init(void)
{
    cJSON_Hooks hooks = { .malloc_fn = arena_alloc, .free_fn = no_free };
    cJSON_InitHooks(&hooks);
}

void json_reply(struct response *res, int status, const cJSON *obj)
{
    char *text = obj ? cJSON_PrintUnformatted(obj) : NULL;
    if (text == NULL) {
        http_text(res, 500, "500 Internal Server Error\n");
        return;
    }
    res->status = status;
    res->content_type = "application/json";
    res->body = text;
    res->body_len = strlen(text);
}

void json_error(struct response *res, int status, const char *msg)
{
    cJSON *obj = cJSON_CreateObject();
    if (obj != NULL)
        cJSON_AddStringToObject(obj, "error", msg);
    json_reply(res, status, obj);
}

cJSON *json_body(const struct request *req, struct response *res)
{
    /* Requiring this header also blocks cross-site HTML form posts. */
    const char *type = http_header(req, "Content-Type");
    if (type == NULL || strncasecmp(type, "application/json", 16) != 0 ||
        (type[16] != '\0' && type[16] != ';')) {
        json_error(res, 415, "expected Content-Type: application/json");
        return NULL;
    }
    if (req->body == NULL) {
        json_error(res, 400, "missing body");
        return NULL;
    }
    cJSON *obj = cJSON_ParseWithLength(req->body, req->body_len);
    if (!cJSON_IsObject(obj)) {
        json_error(res, 400, "body must be a JSON object");
        return NULL;
    }
    return obj;
}

const char *json_get_string(const cJSON *obj, const char *key, size_t min, size_t max,
                            const char **out)
{
    static char msg[128];
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item)) {
        snprintf(msg, sizeof msg, "'%s' must be a string", key);
        return msg;
    }
    size_t len = strlen(item->valuestring);
    if (len < min || len > max) {
        snprintf(msg, sizeof msg, "'%s' must be %zu to %zu bytes", key, min, max);
        return msg;
    }
    *out = item->valuestring;
    return NULL;
}
