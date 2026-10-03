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

int text_valid(const char *s, int multiline)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p != '\0') {
        unsigned char c = *p;
        if (c < 0x80) {
            if ((c < 0x20 && !(multiline && c == '\n')) || c == 0x7f)
                return 0;
            p++;
            continue;
        }
        /* Multi-byte sequence: lead byte, then 1-3 continuation bytes.
         * Overlong forms, surrogates and code points past U+10FFFF are
         * rejected through the allowed range of the second byte. */
        int more;
        unsigned char lo = 0x80, hi = 0xbf;
        if (c >= 0xc2 && c <= 0xdf)
            more = 1;
        else if (c >= 0xe0 && c <= 0xef) {
            more = 2;
            if (c == 0xe0)
                lo = 0xa0;
            else if (c == 0xed)
                hi = 0x9f;
        } else if (c >= 0xf0 && c <= 0xf4) {
            more = 3;
            if (c == 0xf0)
                lo = 0x90;
            else if (c == 0xf4)
                hi = 0x8f;
        } else
            return 0;
        if (p[1] < lo || p[1] > hi)
            return 0;
        for (int i = 2; i <= more; i++)
            if (p[i] < 0x80 || p[i] > 0xbf)
                return 0;
        /* C1 control characters (U+0080..U+009F) */
        if (c == 0xc2 && p[1] <= 0x9f)
            return 0;
        p += more + 1;
    }
    return 1;
}

/* An error message "'key' <what>" in the request arena. */
static const char *field_error(const char *key, const char *what)
{
    char *msg = arena_alloc(128);
    if (msg == NULL)
        return "invalid field";
    snprintf(msg, 128, "'%s' %s", key, what);
    return msg;
}

const char *json_get_text(const cJSON *obj, const char *key, size_t min, size_t max,
                          int multiline, const char **out)
{
    const char *s;
    const char *err = json_get_string(obj, key, min, max, &s);
    if (err != NULL)
        return err;
    if (!text_valid(s, multiline))
        return field_error(key, multiline ? "must be UTF-8 text without control characters"
                                          : "must be one line of UTF-8 text");
    *out = s;
    return NULL;
}

const char *json_get_int(const cJSON *obj, const char *key, long min, long max, long *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        double v = item->valuedouble;
        /* Range first: converting an out-of-range double to long is undefined. */
        if (v >= (double)min && v <= (double)max && v == (double)(long)v) {
            *out = (long)v;
            return NULL;
        }
    }
    char what[96];
    snprintf(what, sizeof what, "must be a whole number from %ld to %ld", min, max);
    return field_error(key, what);
}

cJSON *json_row(sqlite3_stmt *st, int ncols)
{
    cJSON *obj = cJSON_CreateObject();
    for (int i = 0; obj != NULL && i < ncols; i++) {
        const char *key = sqlite3_column_name(st, i);
        cJSON *item;
        switch (sqlite3_column_type(st, i)) {
        case SQLITE_INTEGER:
            /* ids and small numbers: exact in a double */
            item = cJSON_AddNumberToObject(obj, key, (double)sqlite3_column_int64(st, i));
            break;
        case SQLITE_TEXT:
            item = cJSON_AddStringToObject(obj, key, (const char *)sqlite3_column_text(st, i));
            break;
        default:
            item = cJSON_AddNullToObject(obj, key);
        }
        if (item == NULL)
            obj = NULL; /* the arena frees what was built */
    }
    return obj;
}

const char *json_get_bool(const cJSON *obj, const char *key, int *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsBool(item))
        return field_error(key, "must be true or false");
    *out = cJSON_IsTrue(item);
    return NULL;
}
