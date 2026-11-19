/*
 * Navidrome through its Subsonic API (src/navidrome.h).
 */
#define _POSIX_C_SOURCE 200809L

#include "navidrome.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "arena.h"
#include "db.h"
#include "https.h"
#include "json.h"
#include "music.h"

#define API_VERSION "1.16.1"
#define MAX_CALL_URL 2048

static char base_url[NAVIDROME_MAX_URL + 1];
static char nav_music[MUSIC_MAX_ROOT + 1];

/* 1 if p (len bytes) is a path of folder names: A-Z a-z 0-9 _ . - between
 * single slashes, none "." or "..". */
static int prefix_valid(const char *p, size_t len)
{
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || p[i] == '/') {
            size_t n = i - start;
            if (n == 0 || (n == 1 && p[start] == '.') ||
                (n == 2 && p[start] == '.' && p[start + 1] == '.'))
                return 0;
            start = i + 1;
            continue;
        }
        char c = p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '.' || c == '-'))
            return 0;
    }
    return 1;
}

int navidrome_url_valid(const char *url)
{
    char host[HTTPS_MAX_HOST + 1];
    int port;
    const char *path;
    if (url == NULL || strlen(url) > NAVIDROME_MAX_URL || strchr(url, '?') != NULL ||
        http_local_split(url, host, &port, &path) != 0)
        return 0;
    size_t len = strlen(path);
    if (len > 1 && path[len - 1] == '/')
        len--;
    return len == 1 || prefix_valid(path + 1, len - 1);
}

int navidrome_configure(const char *url, const char *music)
{
    base_url[0] = '\0';
    nav_music[0] = '\0';
    if (url == NULL || *url == '\0')
        return 0;
    if (!navidrome_url_valid(url)) {
        fprintf(stderr, "NYLM_NAVIDROME must be Navidrome's address on this machine, like "
                        "http://127.0.0.1:4533 (an address in 127.0.0.0/8, an optional port and "
                        "path of A-Z a-z 0-9 / _ . -)\n");
        return -1;
    }
    if (music_root() == NULL) {
        fprintf(stderr, "NYLM_NAVIDROME needs NYLM_MUSIC (the music folder Navidrome reads)\n");
        return -1;
    }
    size_t len = strlen(url);
    while (len > 0 && url[len - 1] == '/')
        len--;
    memcpy(base_url, url, len);
    base_url[len] = '\0';
    if (music == NULL || *music == '\0')
        return 0;
    len = strlen(music);
    while (len > 1 && music[len - 1] == '/')
        len--;
    if (music[0] != '/' || len < 2 || len > MUSIC_MAX_ROOT || !text_valid(music, 0) ||
        !prefix_valid(music + 1, len - 1)) {
        fprintf(stderr, "NYLM_NAVIDROME_MUSIC must be the absolute path of the music folder "
                        "inside Navidrome (A-Z a-z 0-9 / _ . -, not /), like /music\n");
        base_url[0] = '\0';
        return -1;
    }
    memcpy(nav_music, music, len);
    nav_music[len] = '\0';
    return 0;
}

const char *navidrome_url(void)
{
    return base_url[0] != '\0' ? base_url : NULL;
}

const char *navidrome_music(void)
{
    return nav_music[0] != '\0' ? nav_music : music_root();
}

int navidrome_user_valid(const char *s)
{
    size_t n = s != NULL ? strlen(s) : 0;
    return n >= 1 && n <= NAVIDROME_MAX_USER && text_valid(s, 0);
}

int navidrome_password_valid(const char *s)
{
    size_t n = s != NULL ? strlen(s) : 0;
    return n >= 1 && n <= NAVIDROME_MAX_PASSWORD && text_valid(s, 0);
}

int navidrome_id_valid(const char *s)
{
    size_t n = 0;
    for (; s != NULL && s[n] != '\0'; n++) {
        char c = s[n];
        if (n >= NAVIDROME_MAX_ID ||
            !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_'))
            return 0;
    }
    return n > 0;
}

/* s with every byte but A-Z a-z 0-9 - . _ ~ as %XX, into out. 0, or -1 if
 * it does not fit. */
static int escape(const char *s, char *out, size_t outlen)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        int plain = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                    (*p >= '0' && *p <= '9') || *p == '-' || *p == '.' || *p == '_' || *p == '~';
        if (o + (plain ? 1 : 3) >= outlen)
            return -1;
        if (plain) {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 15];
        }
    }
    out[o] = '\0';
    return 0;
}

int navidrome_call_url(const char *base, const char *method, const char *user,
                       const char *password, const char *salt, const char *extra, char *out,
                       size_t outlen)
{
    char joined[NAVIDROME_MAX_PASSWORD + 64];
    unsigned char md5[EVP_MAX_MD_SIZE];
    unsigned md5_len = 0;
    char token[33], name[NAVIDROME_MAX_USER * 3 + 1];
    int j = snprintf(joined, sizeof joined, "%s%s", password, salt);
    if (j < 0 || (size_t)j >= sizeof joined ||
        EVP_Digest(joined, (size_t)j, md5, &md5_len, EVP_md5(), NULL) != 1 || md5_len != 16 ||
        escape(user, name, sizeof name) != 0) {
        OPENSSL_cleanse(joined, sizeof joined);
        return -1;
    }
    OPENSSL_cleanse(joined, sizeof joined);
    for (unsigned i = 0; i < 16; i++)
        snprintf(token + 2 * i, 3, "%02x", md5[i]);
    int n = snprintf(out, outlen,
                     "%s/rest/%s?u=%s&t=%s&s=%s&v=" API_VERSION "&c=nylm&f=json%s", base, method,
                     name, token, salt, extra);
    return n >= 0 && (size_t)n < outlen ? 0 : -1;
}

cJSON *navidrome_answer(const char *body, size_t len, int *refused, char *err, size_t errlen)
{
    *refused = 0;
    cJSON *root = body != NULL ? cJSON_ParseWithLength(body, len) : NULL;
    cJSON *r = cJSON_GetObjectItemCaseSensitive(root, "subsonic-response");
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(r, "status");
    if (!cJSON_IsObject(r) || !cJSON_IsString(status)) {
        snprintf(err, errlen, "Navidrome's answer is not one nylm can read%s",
                 root == NULL && body != NULL ? " (too big, or not JSON)" : "");
        return NULL;
    }
    if (strcmp(status->valuestring, "ok") == 0)
        return r;
    *refused = 1;
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(r, "error");
    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(e, "message");
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(e, "code");
    snprintf(err, errlen, "Navidrome says: %.200s (code %d)",
             cJSON_IsString(msg) && text_valid(msg->valuestring, 0) ? msg->valuestring
                                                                     : "an error",
             cJSON_IsNumber(code) ? code->valueint : 0);
    return NULL;
}

/* The login in music_db into user and password. 1, 0 if none, -1 (logged). */
static int stored_login(char user[NAVIDROME_MAX_USER + 1],
                        char password[NAVIDROME_MAX_PASSWORD + 1])
{
    sqlite3_stmt *st = db_prepare(music_db,
        "SELECT user, password FROM navidrome_account WHERE id = 1");
    int rc = st != NULL ? sqlite3_step(st) : SQLITE_ERROR;
    int found = 0;
    if (rc == SQLITE_ROW) {
        const char *u = (const char *)sqlite3_column_text(st, 0);
        const char *p = (const char *)sqlite3_column_text(st, 1);
        found = navidrome_user_valid(u) && navidrome_password_valid(p) ? 1 : -1;
        if (found == 1) {
            snprintf(user, NAVIDROME_MAX_USER + 1, "%s", u);
            snprintf(password, NAVIDROME_MAX_PASSWORD + 1, "%s", p);
        } else {
            fprintf(stderr, "navidrome: the stored login is not valid\n");
        }
    } else if (rc != SQLITE_DONE) {
        db_log_error(music_db, "navidrome login");
        found = -1;
    }
    sqlite3_finalize(st);
    return found;
}

int navidrome_login(const char **user)
{
    char u[NAVIDROME_MAX_USER + 1], p[NAVIDROME_MAX_PASSWORD + 1];
    int found = stored_login(u, p);
    OPENSSL_cleanse(p, sizeof p);
    if (found == 1 && (*user = arena_strndup(u, strlen(u))) == NULL)
        return -1;
    return found;
}

cJSON *navidrome_call(const char *method, const char *extra, const char *user,
                      const char *password, int *refused, char *err, size_t errlen)
{
    int dummy;
    if (refused == NULL)
        refused = &dummy;
    *refused = 0;
    if (base_url[0] == '\0') {
        snprintf(err, errlen, "Navidrome is not set up (NYLM_NAVIDROME)");
        return NULL;
    }
    char u[NAVIDROME_MAX_USER + 1], p[NAVIDROME_MAX_PASSWORD + 1];
    if (user == NULL) {
        int found = stored_login(u, p);
        if (found != 1) {
            snprintf(err, errlen, found == 0 ? "nylm has no Navidrome login: add it in Music, "
                                               "Navidrome"
                                             : "can not read the Navidrome login (see the log)");
            return NULL;
        }
        user = u;
        password = p;
    }
    unsigned char raw[8];
    char salt[17], url[MAX_CALL_URL];
    int ok = RAND_bytes(raw, sizeof raw) == 1;
    for (unsigned i = 0; ok && i < sizeof raw; i++)
        snprintf(salt + 2 * i, 3, "%02x", raw[i]);
    ok = ok && navidrome_call_url(base_url, method, user, password, salt, extra, url,
                                  sizeof url) == 0;
    OPENSSL_cleanse(p, sizeof p);
    if (!ok) {
        snprintf(err, errlen, "can not make the call to Navidrome");
        return NULL;
    }
    struct https_response r;
    char why[256];
    if (http_local_request("GET", url, NULL, NAVIDROME_MAX_ANSWER, &r, why, sizeof why) != 0) {
        /* The address has the token: say only where it went. */
        snprintf(err, errlen, "Navidrome does not answer at %s: %s", base_url, why);
        return NULL;
    }
    cJSON *answer = NULL;
    if (r.status != 200)
        snprintf(err, errlen, "Navidrome answered HTTP %d (is NYLM_NAVIDROME its address?)",
                 r.status);
    else
        answer = navidrome_answer(r.body, r.len, refused, err, errlen);
    free(r.body);
    return answer;
}

int navidrome_check_login(const char *user, const char *password, int *refused, char *err,
                          size_t errlen)
{
    char name[NAVIDROME_MAX_USER * 3 + 1], extra[NAVIDROME_MAX_USER * 3 + 16];
    if (!navidrome_user_valid(user) || !navidrome_password_valid(password) ||
        escape(user, name, sizeof name) != 0) {
        *refused = 1;
        snprintf(err, errlen, "not a valid user name and password");
        return -1;
    }
    snprintf(extra, sizeof extra, "&username=%s", name);
    if (navidrome_call("ping", "", user, password, refused, err, errlen) == NULL)
        return -1;
    cJSON *r = navidrome_call("getUser", extra, user, password, refused, err, errlen);
    if (r == NULL)
        return -1;
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(r, "user");
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(u, "adminRole"))) {
        *refused = 1;
        snprintf(err, errlen, "%s is not an admin in Navidrome: nylm needs an admin login to "
                              "start Navidrome's scans", user);
        return -1;
    }
    return 0;
}

int navidrome_local_path(const char *nav_root, const char *music_root, const char *path,
                         char *out, size_t outlen)
{
    if (nav_root == NULL || music_root == NULL || path == NULL || !text_valid(path, 0))
        return -1;
    const char *rel = path;
    size_t n = strlen(nav_root);
    if (path[0] == '/') {
        if (strncmp(path, nav_root, n) != 0 || path[n] != '/')
            return -1;
        rel = path + n + 1;
    }
    /* Each part a name: not empty, not "." or "..". */
    for (const char *p = rel;;) {
        size_t len = strcspn(p, "/");
        if (len == 0 || (len == 1 && p[0] == '.') || (len == 2 && p[0] == '.' && p[1] == '.'))
            return -1;
        if (p[len] == '\0')
            break;
        p += len + 1;
    }
    int w = snprintf(out, outlen, "%s/%s", music_root, rel);
    return w >= 0 && (size_t)w < outlen ? 0 : -1;
}
