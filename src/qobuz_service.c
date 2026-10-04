/*
 * nylm-qobuz: the Qobuz service (only in that binary, with src/https.c).
 *
 * One run: read the web player's bundle (app id, OAuth key, signing
 * secrets), finish a login the web app queued, then download each queued
 * album. An album's tracks are downloaded into a hidden folder in the
 * library (scans skip dot folders), tagged through TagLib from Qobuz's
 * data, then, holding the library lock, moved where the naming rule puts
 * them (src/move.h), never over a file; then they are scanned.
 *
 * Ported from beets-webapp's qobuz_client.py and qobuz_service.py (which
 * follow qobuz-dl and qobuz-dl-go): the same endpoints, OAuth exchange and
 * request signing.
 */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "arena.h"
#include "art.h"
#include "db.h"
#include "https.h"
#include "image.h"
#include "move.h"
#include "music.h"
#include "qobuz.h"
#include "tags.h"

#define PLAYER       "https://play.qobuz.com"
#define API          "https://www.qobuz.com/api.json/0.2/"
#define USER_AGENT   "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:83.0) " \
                     "Gecko/20100101 Firefox/83.0"
#define TEST_TRACK   "5966783" /* a track that always exists: tests a secret */
#define FORMAT_BEST  27        /* the best FLAC there is (Qobuz falls back) */
#define FORMAT_TEST  5
#define MAX_PAGE     (1024 * 1024)
#define MAX_BUNDLE   (32 * 1024 * 1024)
#define MAX_JSON     (8 * 1024 * 1024)
#define MAX_COVER    (20 * 1024 * 1024)
#define MAX_TRACK    (4LL * 1024 * 1024 * 1024)
#define MAX_TRACKS   500 /* tracks of one album */
#define STAGING      ".nylm-qobuz"
#define ERR_LEN      512

struct qobuz {
    char app_id[QOBUZ_APP_ID_LEN + 1];
    char private_key[QOBUZ_MAX_TOKEN + 1];
    char secrets[QOBUZ_MAX_SECRETS][QOBUZ_MAX_SECRET + 1];
    int nsecrets;
    const char *secret; /* the one that signs downloads */
    char token[QOBUZ_MAX_TOKEN + 1];
    char user_id[QOBUZ_MAX_TOKEN + 1];
    char label[128];
};

/* ---- requests ----------------------------------------------------------- */

/*
 * Calls endpoint of the API: GET with params in the query, or POST with
 * them as the body (as the web player sends them). params: name, value,
 * ..., NULL. Its JSON (in the arena) into *out; the HTTP status into
 * *status. 0, or -1 with err set (an HTTP error status too).
 */
static int api(const struct qobuz *q, int post, const char *endpoint, const char *const *params,
               cJSON **out, int *status, char *err, size_t errlen)
{
    char query[4096];
    size_t n = 0;
    *status = 0;
    for (size_t i = 0; params != NULL && params[i] != NULL; i += 2) {
        char value[QOBUZ_MAX_TOKEN * 3 + 1];
        if (qobuz_urlencode(params[i + 1], value, sizeof value) != 0)
            goto too_long;
        int k = snprintf(query + n, sizeof query - n, "%s%s=%s", n > 0 ? "&" : "", params[i], value);
        if (k < 0 || (size_t)k >= sizeof query - n)
            goto too_long;
        n += (size_t)k;
    }
    query[n] = '\0';
    char url[4096 + 128];
    int u = snprintf(url, sizeof url, "%s%s%s%s", API, endpoint, post || n == 0 ? "" : "?",
                     post ? "" : query);
    if (u < 0 || (size_t)u >= sizeof url)
        goto too_long;
    char app[64], user[QOBUZ_MAX_TOKEN + 32];
    snprintf(app, sizeof app, "X-App-Id: %s", q->app_id);
    snprintf(user, sizeof user, "X-User-Auth-Token: %s", q->token);
    const char *headers[] = { USER_AGENT, app, q->token[0] != '\0' ? user : NULL, NULL };
    struct https_response r;
    if (https_request(post ? "POST" : "GET", url, headers, "text/plain;charset=UTF-8",
                      post ? query : NULL, post ? n : 0, -1, MAX_JSON, &r, err, errlen) != 0)
        return -1;
    *status = r.status;
    if (r.status >= 400) {
        snprintf(err, errlen, "Qobuz answered %d to %s", r.status, endpoint);
        free(r.body);
        return -1;
    }
    *out = cJSON_ParseWithLength(r.body, r.len);
    free(r.body);
    if (*out == NULL) {
        snprintf(err, errlen, "Qobuz's answer to %s is not JSON", endpoint);
        return -1;
    }
    return 0;
too_long:
    snprintf(err, errlen, "a request to %s is too long", endpoint);
    return -1;
}

/* GET url into memory (at most max bytes); the body (free() it) or NULL
 * with err set. */
static char *fetch(const char *url, size_t max, size_t *len, char *err, size_t errlen)
{
    const char *headers[] = { USER_AGENT, NULL };
    struct https_response r;
    if (https_request("GET", url, headers, NULL, NULL, 0, -1, max, &r, err, errlen) != 0)
        return NULL;
    if (r.status != 200) {
        snprintf(err, errlen, "%s answered %d", url, r.status);
        free(r.body);
        return NULL;
    }
    *len = r.len;
    return r.body;
}

/* Reads the web player's bundle: app id, OAuth key, secrets. 0 or -1. */
static int read_bundle(struct qobuz *q, char *err, size_t errlen)
{
    size_t len;
    char *page = fetch(PLAYER "/login", MAX_PAGE, &len, err, errlen);
    char path[256];
    if (page == NULL)
        return -1;
    int found = qobuz_bundle_path(page, path, sizeof path);
    free(page);
    if (found != 0) {
        snprintf(err, errlen, "the Qobuz login page has no bundle.js (Qobuz changed it?)");
        return -1;
    }
    char url[512];
    snprintf(url, sizeof url, "%s%s", PLAYER, path);
    char *bundle = fetch(url, MAX_BUNDLE, &len, err, errlen);
    if (bundle == NULL)
        return -1;
    int ok = qobuz_app_id(bundle, q->app_id) == 0;
    if (ok) {
        qobuz_private_key(bundle, q->private_key);
        q->nsecrets = qobuz_secrets(bundle, q->secrets, QOBUZ_MAX_SECRETS);
    }
    free(bundle);
    if (!ok)
        snprintf(err, errlen, "the Qobuz bundle has no app id (Qobuz changed it?)");
    return ok ? 0 : -1;
}

/* Takes the account's user id, token and subscription from a user/login
 * (or oauth/callback) answer. 0, or -1 with err set. */
static int take_user(struct qobuz *q, const cJSON *info, char *err, size_t errlen)
{
    const cJSON *user = cJSON_GetObjectItemCaseSensitive(info, "user");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(user, "credential"), "parameters");
    if (!cJSON_IsObject(params) || params->child == NULL) {
        snprintf(err, errlen, "this Qobuz account can not download (a free account?)");
        return -1;
    }
    const cJSON *token = cJSON_GetObjectItemCaseSensitive(info, "user_auth_token");
    if (q->token[0] == '\0' && cJSON_IsString(token))
        snprintf(q->token, sizeof q->token, "%s", token->valuestring);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(user, "id");
    if (q->user_id[0] == '\0' && cJSON_IsNumber(id))
        snprintf(q->user_id, sizeof q->user_id, "%.0f", id->valuedouble);
    else if (q->user_id[0] == '\0' && cJSON_IsString(id))
        snprintf(q->user_id, sizeof q->user_id, "%s", id->valuestring);
    const cJSON *label = cJSON_GetObjectItemCaseSensitive(params, "short_label");
    if (!cJSON_IsString(label))
        label = cJSON_GetObjectItemCaseSensitive(params, "label");
    snprintf(q->label, sizeof q->label, "%s", cJSON_IsString(label) ? label->valuestring : "");
    if (q->token[0] == '\0' || q->user_id[0] == '\0') {
        snprintf(err, errlen, "Qobuz's login answer has no token or user id");
        return -1;
    }
    return 0;
}

/* Logs in with what the user pasted: a token, or an OAuth code exchanged
 * for one (Qobuz has used GET and POST, "code" and "code_autorisation":
 * each is tried). 0, or -1 with err set. */
static int login(struct qobuz *q, const char *pasted, char *err, size_t errlen)
{
    char code[QOBUZ_MAX_TOKEN + 1], token[QOBUZ_MAX_TOKEN + 1], user_id[QOBUZ_MAX_TOKEN + 1];
    if (qobuz_redirect_parse(pasted, code, token, user_id) != 0) {
        snprintf(err, errlen, "no login code or token in what was pasted");
        return -1;
    }
    q->token[0] = q->user_id[0] = '\0';
    cJSON *info;
    int status;
    const char *partner[] = { "extra", "partner", NULL };
    if (token[0] != '\0') {
        snprintf(q->token, sizeof q->token, "%s", token);
        snprintf(q->user_id, sizeof q->user_id, "%s", user_id);
        if (api(q, 1, "user/login", partner, &info, &status, err, errlen) != 0)
            return -1;
        return take_user(q, info, err, errlen);
    }
    static const char *const names[] = { "code", "code", "code_autorisation", "code_autorisation" };
    for (int i = 0; i < 4; i++) {
        const char *params[] = { names[i], code, "app_id", q->app_id,
                                 q->private_key[0] != '\0' ? "private_key" : NULL,
                                 q->private_key, NULL };
        if (api(q, i % 2, "oauth/callback", params, &info, &status, err, errlen) != 0)
            continue;
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(info, "token");
        if (cJSON_IsString(t) && t->valuestring[0] != '\0') {
            snprintf(q->token, sizeof q->token, "%s", t->valuestring);
            if (api(q, 1, "user/login", partner, &info, &status, err, errlen) != 0)
                return -1;
            return take_user(q, info, err, errlen);
        }
        if (cJSON_GetObjectItemCaseSensitive(info, "user") != NULL)
            return take_user(q, info, err, errlen);
        snprintf(err, errlen, "Qobuz's OAuth answer has no token");
    }
    return -1;
}

/* Checks the stored login; 0, or -1 with err set. */
static int check_login(struct qobuz *q, char *err, size_t errlen)
{
    cJSON *info;
    int status;
    const char *params[] = { "user_id", q->user_id, "user_auth_token", q->token,
                             "app_id", q->app_id, NULL };
    if (api(q, 0, "user/login", params, &info, &status, err, errlen) != 0) {
        if (status == 401)
            snprintf(err, errlen, "the Qobuz login has expired: connect again");
        return -1;
    }
    return take_user(q, info, err, errlen);
}

/* track/getFileUrl, signed with secret. 0, or -1 with err set. */
static int file_url(const struct qobuz *q, const char *track, int format, const char *secret,
                    cJSON **out, char *err, size_t errlen)
{
    long long ts = (long long)time(NULL);
    char sig[33], fmt[8], when[24];
    if (qobuz_signature(track, format, ts, secret, sig) != 0) {
        snprintf(err, errlen, "can not sign the request");
        return -1;
    }
    snprintf(fmt, sizeof fmt, "%d", format);
    snprintf(when, sizeof when, "%lld", ts);
    const char *params[] = { "request_ts", when, "request_sig", sig, "track_id", track,
                             "format_id", fmt, "intent", "stream", NULL };
    int status;
    return api(q, 0, "track/getFileUrl", params, out, &status, err, errlen);
}

/* Finds a secret that signs downloads. 0, or -1 with err set. */
static int find_secret(struct qobuz *q, char *err, size_t errlen)
{
    for (int i = 0; i < q->nsecrets; i++) {
        cJSON *r;
        char ignored[ERR_LEN];
        if (file_url(q, TEST_TRACK, FORMAT_TEST, q->secrets[i], &r, ignored, sizeof ignored) == 0) {
            q->secret = q->secrets[i];
            return 0;
        }
    }
    snprintf(err, errlen, "no secret in the Qobuz bundle signs downloads (Qobuz changed "
                          "something; see qobuz-dl-go)");
    return -1;
}

/* ---- the database ------------------------------------------------------- */

/* Runs sql with text parameters (NULL binds NULL), nparams of them. 0 or
 * -1 (logged). */
static int run(const char *sql, int nparams, ...)
{
    sqlite3_stmt *st = db_prepare(music_db, sql);
    va_list ap;
    va_start(ap, nparams);
    int ok = st != NULL;
    for (int i = 0; ok && i < nparams; i++) {
        const char *v = va_arg(ap, const char *);
        ok = (v != NULL ? sqlite3_bind_text(st, i + 1, v, -1, SQLITE_TRANSIENT)
                        : sqlite3_bind_null(st, i + 1)) == SQLITE_OK;
    }
    va_end(ap);
    ok = ok && sqlite3_step(st) == SQLITE_DONE;
    if (!ok)
        db_log_error(music_db, sql);
    sqlite3_finalize(st);
    return ok ? 0 : -1;
}

/* Records an account error ("" clears it). */
static void account_error(const char *msg)
{
    fprintf(stderr, "qobuz: %s\n", msg[0] != '\0' ? msg : "ok");
    run("UPDATE qobuz_account SET error = ?, updated = unixepoch()", 1, msg);
}

/* Ends download id: state, how many tracks were saved, and a note. */
static void finish(long long id, const char *state, int saved, const char *note)
{
    char num[24], saved_text[16];
    snprintf(num, sizeof num, "%lld", id);
    snprintf(saved_text, sizeof saved_text, "%d", saved);
    fprintf(stderr, "qobuz: download %lld %s: %s\n", id, state, note);
    run("UPDATE qobuz_downloads SET state = ?, saved = ?, note = ?, finished = unixepoch()"
        " WHERE id = ?", 4, state, saved_text, note, num);
}

/* ---- downloading -------------------------------------------------------- */

/* Appends text to note (with "; " between), cut at a character boundary
 * when the note is full. */
static void note_add(char *note, size_t size, const char *text)
{
    size_t n = strlen(note);
    if (n > 0 && n + 2 < size) {
        memcpy(note + n, "; ", 2);
        n += 2;
    }
    size_t len = strlen(text);
    if (len > size - 1 - n) {
        len = size - 1 - n;
        while (len > 0 && ((unsigned char)text[len] & 0xc0) == 0x80)
            len--;
    }
    memcpy(note + n, text, len);
    note[n + len] = '\0';
}

/* Removes the files in folder dir, then dir (a staging folder: only files). */
static void remove_folder(const char *dir)
{
    DIR *d = opendir(dir);
    if (d == NULL)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char path[TAGS_MAX_PATH];
        if (e->d_name[0] == '.' && (e->d_name[1] == '\0' || strcmp(e->d_name, "..") == 0))
            continue;
        if (snprintf(path, sizeof path, "%s/%s", dir, e->d_name) < (int)sizeof path)
            unlink(path);
    }
    closedir(d);
    rmdir(dir);
}

/* A track on its way: the file in the staging folder and its path in the
 * library (relative to the music folder). */
struct staged {
    char file[TAGS_MAX_PATH];
    char rel[TAGS_MAX_PATH];
};

/* The album's cover (image.large), checked to decode; NULL if none. *mime
 * set. malloc()ed. */
static unsigned char *cover(const cJSON *album, size_t *size, const char **mime, char *note,
                            size_t notelen)
{
    const cJSON *large = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(album, "image"), "large");
    if (!cJSON_IsString(large))
        return NULL;
    char err[ERR_LEN];
    unsigned char *data = (unsigned char *)fetch(large->valuestring, MAX_COVER, size, err,
                                                 sizeof err);
    unsigned char *thumb = NULL;
    size_t thumb_size;
    unsigned w, h;
    *mime = data != NULL ? art_mime(data, *size) : NULL;
    if (data != NULL && *mime != NULL &&
        (strcmp(*mime, "image/jpeg") == 0 || strcmp(*mime, "image/png") == 0) &&
        image_thumbnail(data, *size, ART_THUMB_SIZE, &thumb, &thumb_size, &w, &h, err,
                        sizeof err) == 0) {
        free(thumb);
        return data;
    }
    free(data);
    note_add(note, notelen, "no cover (it could not be read)");
    return NULL;
}

/*
 * Downloads and tags one track into the staging folder dir (as file NN.ext)
 * and sets where it goes in the library. 0, or -1 with err set.
 */
static int stage_track(const struct qobuz *q, const cJSON *album, const cJSON *track,
                       int index, const char *dir, const struct tag_picture *pic,
                       struct staged *out, char *err, size_t errlen)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(track, "id");
    char track_id[32];
    if (!cJSON_IsNumber(id) || id->valuedouble < 1 || id->valuedouble > 1e15) {
        snprintf(err, errlen, "a track without an id");
        return -1;
    }
    snprintf(track_id, sizeof track_id, "%.0f", id->valuedouble);
    const cJSON *streamable = cJSON_GetObjectItemCaseSensitive(track, "streamable");
    if (cJSON_IsFalse(streamable)) {
        snprintf(err, errlen, "track %s can not be streamed", track_id);
        return -1;
    }
    cJSON *info;
    if (file_url(q, track_id, FORMAT_BEST, q->secret, &info, err, errlen) != 0)
        return -1;
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(info, "url");
    const cJSON *mime = cJSON_GetObjectItemCaseSensitive(info, "mime_type");
    if (cJSON_GetObjectItemCaseSensitive(info, "sample") != NULL ||
        !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(info, "sampling_rate")) ||
        !cJSON_IsString(url)) {
        snprintf(err, errlen, "track %s: Qobuz offers only a sample", track_id);
        return -1;
    }
    const char *ext = cJSON_IsString(mime) && strcmp(mime->valuestring, "audio/mpeg") == 0
                          ? "mp3" : "flac";
    int k = snprintf(out->file, sizeof out->file, "%s/%02d.%s", dir, index, ext);
    if (k < 0 || (size_t)k >= sizeof out->file) {
        snprintf(err, errlen, "the staging path is too long");
        return -1;
    }
    int fd = open(out->file, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) {
        snprintf(err, errlen, "%.300s: %s", out->file, strerror(errno));
        return -1;
    }
    const char *headers[] = { USER_AGENT, NULL };
    struct https_response r;
    int rc = https_request("GET", url->valuestring, headers, NULL, NULL, 0, fd,
                           (size_t)MAX_TRACK, &r, err, errlen);
    if (rc == 0 && r.status != 200) {
        snprintf(err, errlen, "track %s: the download answered %d", track_id, r.status);
        rc = -1;
    }
    if (rc == 0 && fsync(fd) != 0) {
        snprintf(err, errlen, "%.300s: %s", out->file, strerror(errno));
        rc = -1;
    }
    if (close(fd) != 0 && rc == 0) {
        snprintf(err, errlen, "%.300s: %s", out->file, strerror(errno));
        rc = -1;
    }
    if (rc != 0) {
        unlink(out->file);
        return -1;
    }

    if (qobuz_tag_file(out->file, ext, album, track, pic, out->rel, sizeof out->rel, err,
                       errlen) != 0) {
        char why[ERR_LEN];
        snprintf(why, sizeof why, "track %s: %.400s", track_id, err);
        snprintf(err, errlen, "%s", why);
        return -1;
    }
    return 0;
}

int qobuz_tag_file(const char *path, const char *ext, const cJSON *album, const cJSON *track,
                   const struct tag_picture *pic, char *rel, size_t rellen, char *err,
                   size_t errlen)
{
    struct tags now, want;
    char read_err[ERR_LEN], note[TAGS_MAX_NOTE] = "";
    const char *why;
    if (tags_read(path, &now, NULL, NULL, read_err, sizeof read_err) != 0) {
        snprintf(err, errlen, "the file can not be read: %s", read_err);
        return -1;
    }
    if ((why = qobuz_track_tags(album, track, &want)) != NULL) {
        snprintf(err, errlen, "%s", why);
        return -1;
    }
    unsigned changed = 0;
    for (int f = 0; f < TAG_FIELDS; f++)
        changed |= 1u << f;
    if (pic != NULL) {
        want.npictures = 1;
        want.pictures = pic;
    }
    if (tags_prepare(&want, changed, note, sizeof note) != 0) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    if (tags_write(path, &now, &want, read_err, sizeof read_err) != TAGS_WRITTEN) {
        snprintf(err, errlen, "tagging failed: %s", read_err);
        return -1;
    }
    why = move_target(want.value[TAG_ALBUMARTIST].v[0], want.value[TAG_ALBUM].v[0],
                      want.value[TAG_TITLE].v[0], want.value[TAG_TRACKNUMBER].v[0],
                      want.value[TAG_DISCNUMBER].v[0], ext, rel, rellen);
    if (why != NULL) {
        snprintf(err, errlen, "%s", why);
        return -1;
    }
    return 0;
}

/*
 * Moves the staged tracks into the library under the library lock (never
 * over a file) and queues a scan of each folder they went to. The number
 * moved; -1 if the lock or the database failed.
 */
static int place(struct staged *tracks, int n, char *note, size_t notelen)
{
    int lock = music_start_service("qobuz");
    if (lock < 0) {
        note_add(note, notelen, "the library was busy: the files were not saved");
        return -1;
    }
    int moved = 0;
    const char *root = music_root();
    char last[TAGS_MAX_PATH] = "";
    for (int i = 0; i < n; i++) {
        char to[TAGS_MAX_PATH], why[ERR_LEN];
        int k = snprintf(to, sizeof to, "%s/%s", root, tracks[i].rel);
        if (k < 0 || (size_t)k >= sizeof to) {
            snprintf(why, sizeof why, "%.300s: the path is too long", tracks[i].rel);
            note_add(note, notelen, why);
            continue;
        }
        if (move_make_folders(to) != 0 || move_rename_new(tracks[i].file, to) != 0) {
            snprintf(why, sizeof why, "%.300s: %s", tracks[i].rel,
                     errno == EEXIST ? "already in the library" : strerror(errno));
            note_add(note, notelen, why);
            continue;
        }
        moved++;
        *strrchr(to, '/') = '\0';
        if (strcmp(to, last) != 0) {
            snprintf(last, sizeof last, "%s", to);
            if (run("INSERT INTO scans (path) VALUES (?)", 1, to) != 0)
                moved = -1;
        }
        if (moved < 0)
            break;
    }
    music_unlock(lock);
    return moved;
}

/* Downloads album row id (Qobuz album_id). 1 if a track was saved, 0 if
 * not, -1 on a database error. */
static int download(const struct qobuz *q, long long id, const char *album_id)
{
    char err[ERR_LEN], note[TAGS_MAX_NOTE] = "", num[24], dir[TAGS_MAX_PATH];
    snprintf(num, sizeof num, "%lld", id);
    if (run("UPDATE qobuz_downloads SET state = 'running', started = unixepoch() WHERE id = ?",
            1, num) != 0)
        return -1;
    cJSON *album;
    int status;
    const char *params[] = { "album_id", album_id, NULL };
    if (api(q, 0, "album/get", params, &album, &status, err, sizeof err) != 0) {
        finish(id, "failed", 0, status == 404 ? "Qobuz has no such album" : err);
        return 0;
    }
    const cJSON *tracks = cJSON_GetObjectItemCaseSensitive(album, "tracks");
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(tracks, "items");
    const cJSON *total = cJSON_GetObjectItemCaseSensitive(tracks, "total");
    int count = cJSON_GetArraySize(items);
    const cJSON *title = cJSON_GetObjectItemCaseSensitive(album, "title");
    const cJSON *artist = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(album, "artist"), "name");
    char ntracks[16];
    snprintf(ntracks, sizeof ntracks, "%d", count);
    run("UPDATE qobuz_downloads SET title = ?, artist = ?, tracks = ? WHERE id = ?", 4,
        cJSON_IsString(title) ? title->valuestring : NULL,
        cJSON_IsString(artist) ? artist->valuestring : NULL, ntracks, num);
    if (cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(album, "streamable"))) {
        finish(id, "failed", 0, "the album can not be streamed");
        return 0;
    }
    if (count == 0 || count > MAX_TRACKS ||
        (cJSON_IsNumber(total) && total->valuedouble > count)) {
        finish(id, "failed", 0, count == 0 ? "Qobuz lists no tracks"
                                           : "the album has more tracks than nylm takes (500) "
                                             "or than Qobuz listed");
        return 0;
    }

    snprintf(dir, sizeof dir, "%s/" STAGING "/%lld", music_root(), id);
    char base[TAGS_MAX_PATH];
    snprintf(base, sizeof base, "%s/" STAGING, music_root());
    remove_folder(dir); /* what a cut-off run left */
    if ((mkdir(base, 0755) != 0 && errno != EEXIST) || mkdir(dir, 0755) != 0) {
        snprintf(err, sizeof err, "%.300s: %s", dir, strerror(errno));
        finish(id, "failed", 0, err);
        return 0;
    }
    size_t cover_size = 0;
    const char *mime = NULL;
    unsigned char *cover_data = cover(album, &cover_size, &mime, note, sizeof note);
    struct tag_picture pic = { .type = "Front Cover", .description = "", .data = cover_data,
                               .size = cover_size, .mime = mime };
    if (cover_data != NULL && art_hash(cover_data, cover_size, pic.hash) != 0) {
        free(cover_data);
        cover_data = NULL;
    }
    struct staged *staged = calloc((size_t)count, sizeof *staged);
    int nstaged = 0;
    const cJSON *track;
    int index = 0;
    cJSON_ArrayForEach(track, items) {
        if (staged == NULL)
            break;
        index++;
        if (stage_track(q, album, track, index, dir, cover_data != NULL ? &pic : NULL,
                        &staged[nstaged], err, sizeof err) == 0) {
            nstaged++;
        } else {
            note_add(note, sizeof note, err);
        }
        char done[16];
        snprintf(done, sizeof done, "%d", nstaged);
        run("UPDATE qobuz_downloads SET saved = ? WHERE id = ?", 2, done, num);
    }
    free(cover_data);
    int saved = staged != NULL ? place(staged, nstaged, note, sizeof note) : -1;
    free(staged);
    remove_folder(dir);
    rmdir(base);
    if (saved < 0) {
        finish(id, "failed", 0, note[0] != '\0' ? note : "out of memory");
        return 0;
    }
    finish(id, saved == 0 ? "failed" : saved < count ? "warning" : "done", saved,
           note[0] != '\0' ? note : "");
    return saved > 0;
}

/* ---- the run ------------------------------------------------------------ */

/* The account row's columns into q; *pending: a pasted login waiting. */
static int load_account(struct qobuz *q, char *pending, size_t size)
{
    sqlite3_stmt *st = db_prepare(music_db,
        "SELECT coalesce(user_id, ''), coalesce(token, ''), coalesce(login, '')"
        " FROM qobuz_account WHERE id = 1");
    int ok = st != NULL && sqlite3_step(st) == SQLITE_ROW;
    if (ok) {
        snprintf(q->user_id, sizeof q->user_id, "%s", (const char *)sqlite3_column_text(st, 0));
        snprintf(q->token, sizeof q->token, "%s", (const char *)sqlite3_column_text(st, 1));
        snprintf(pending, size, "%s", (const char *)sqlite3_column_text(st, 2));
    } else {
        db_log_error(music_db, "qobuz: account");
    }
    sqlite3_finalize(st);
    return ok ? 0 : -1;
}

/* The next queued download: its id (0 if none, -1 on error) and album id. */
static long long next_download(char album_id[QOBUZ_MAX_ID + 1])
{
    sqlite3_stmt *st = db_prepare(music_db,
        "SELECT id, album_id FROM qobuz_downloads WHERE state = 'queued' ORDER BY id LIMIT 1");
    int rc = st != NULL ? sqlite3_step(st) : SQLITE_ERROR;
    long long id = rc == SQLITE_ROW ? sqlite3_column_int64(st, 0) : rc == SQLITE_DONE ? 0 : -1;
    if (rc == SQLITE_ROW)
        snprintf(album_id, QOBUZ_MAX_ID + 1, "%s", (const char *)sqlite3_column_text(st, 1));
    if (id < 0)
        db_log_error(music_db, "qobuz: next download");
    sqlite3_finalize(st);
    return id;
}

int qobuz_run(void)
{
    if (music_root() == NULL) {
        fprintf(stderr, "qobuz: NYLM_MUSIC is not set\n");
        return 1;
    }
    int lock = music_qobuz_lock(1);
    if (lock < 0)
        return 1;
    struct qobuz *q = calloc(1, sizeof *q);
    char pending[QOBUZ_MAX_REDIRECT + 1], err[ERR_LEN];
    int rc = q != NULL ? 0 : -1;
    /* Downloads a cut-off run left running: start them again. */
    if (rc == 0)
        rc = run("UPDATE qobuz_downloads SET state = 'queued', saved = 0 WHERE state = 'running'", 0);
    if (rc == 0)
        rc = load_account(q, pending, sizeof pending);
    if (rc == 0 && read_bundle(q, err, sizeof err) != 0) {
        account_error(err);
        if (pending[0] != '\0')
            run("UPDATE qobuz_account SET login = NULL", 0);
        rc = 1;
    }
    if (rc == 0)
        rc = run("UPDATE qobuz_account SET app_id = ?, updated = unixepoch()", 1, q->app_id);
    if (rc == 0 && pending[0] != '\0') {
        if (login(q, pending, err, sizeof err) == 0) {
            rc = run("UPDATE qobuz_account SET user_id = ?, token = ?, label = ?, login = NULL,"
                     " error = '', updated = unixepoch()", 3, q->user_id, q->token, q->label);
            printf("qobuz: logged in (%s)\n", q->label[0] != '\0' ? q->label : "no label");
        } else {
            run("UPDATE qobuz_account SET login = NULL", 0);
            account_error(err);
            q->token[0] = '\0';
        }
    }
    char album_id[QOBUZ_MAX_ID + 1];
    long long next = rc == 0 ? next_download(album_id) : 0;
    if (next > 0 && q->token[0] == '\0') {
        account_error("not connected to Qobuz: connect first");
        next = 0;
    } else if (next > 0 && (check_login(q, err, sizeof err) != 0 ||
                            find_secret(q, err, sizeof err) != 0)) {
        account_error(err);
        next = 0;
    } else if (next > 0) {
        run("UPDATE qobuz_account SET label = ?, error = '', updated = unixepoch()", 1, q->label);
    }
    int albums = 0, scans = 0;
    while (rc == 0 && next > 0) {
        arena_reset();
        int r = download(q, next, album_id);
        if (r < 0)
            rc = -1;
        albums++;
        scans += r > 0;
        if (rc == 0)
            next = next_download(album_id);
        if (next < 0)
            rc = -1;
    }
    free(q);
    music_unlock(lock);
    if (albums > 0)
        printf("qobuz: %d albums downloaded, %d with tracks saved\n", albums, scans);
    /* Reads what was saved into the cache. */
    if (rc == 0 && scans > 0 && music_scan(NULL) != 0)
        rc = 1;
    return rc == 0 ? 0 : 1;
}
