#define _POSIX_C_SOURCE 200809L

#include "json.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "arena.h"

/* 1 if needle occurs anywhere in buf[0..len). */
static int contains(const char *buf, size_t len, const char *needle)
{
    size_t n = strlen(needle);
    for (size_t i = 0; i + n <= len; i++)
        if (memcmp(buf + i, needle, n) == 0)
            return 1;
    return 0;
}

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
    /* cJSON ends strings at a NUL, so "a\u0000b" would silently become "a".
     * Reject NUL in any form rather than store something other than what was
     * sent. (This also rejects the harmless text "\\u0000"; that is fine.) */
    if (memchr(req->body, '\0', req->body_len) != NULL ||
        contains(req->body, req->body_len, "\\u0000")) {
        json_error(res, 400, "body must not contain NUL characters");
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
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    int is_string = cJSON_IsString(item);
    size_t len = is_string ? strlen(item->valuestring) : 0;
    if (is_string && len >= min && len <= max) {
        *out = item->valuestring;
        return NULL;
    }

    char *msg = arena_alloc(128);
    if (msg == NULL)
        return "invalid field";
    if (!is_string)
        snprintf(msg, 128, "'%s' must be a string", key);
    else
        snprintf(msg, 128, "'%s' must be %zu to %zu bytes", key, min, max);
    return msg;
}
