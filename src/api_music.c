/*
 * Music library: albums and their tracks' tags (src/music.c, src/tags.c).
 *
 * Reads come from the cache in music_db. Saving an album writes only the
 * files whose tags change: every one is copied, edited and checked first
 * (tags_prepare), and only if all of them pass are they put in place
 * (tags_commit). Album saves and scan starts are recorded in the audit
 * table before anything is done, and their result after.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "action.h"
#include "api.h"
#include "arena.h"
#include "auth.h"
#include "db.h"
#include "json.h"
#include "music.h"
#include "tags.h"

#define ID_MAX     9007199254740991L /* 2^53 - 1: exact in a JSON number */
#define MAX_TRACKS 500               /* tracks in one album (folder) */

/* Every failed password check costs the caller this long, as for login. */
#define PASSWORD_FAILURE_DELAY_SECONDS 1

/* Fields set for the whole album, and per track. */
static const enum tag_field album_fields[] = {
    TAG_ALBUM, TAG_ALBUMARTIST, TAG_GENRE, TAG_DATE, TAG_COMPILATION,
};
static const enum tag_field track_fields[] = {
    TAG_TITLE, TAG_ARTIST, TAG_TRACKNUMBER, TAG_DISCNUMBER,
};
#define NALBUM_FIELDS (sizeof album_fields / sizeof album_fields[0])
#define NTRACK_FIELDS (sizeof track_fields / sizeof track_fields[0])

/* ---- helpers ------------------------------------------------------------ */

/* A message in the arena; a fixed text if out of memory. */
static const char *message(const char *fmt, const char *a, const char *b)
{
    char *msg = arena_alloc(512);
    if (msg == NULL)
        return "invalid request";
    snprintf(msg, 512, fmt, a, b);
    return msg;
}

/* Replies 503 and returns 0 unless the music folder is set and there. */
static int library_ready(struct response *res)
{
    if (music_root() == NULL) {
        json_error(res, 503, "music is not set up: set NYLM_MUSIC in /etc/nylm.conf");
        return 0;
    }
    if (!music_available()) {
        json_error(res, 503, "the music folder is not available (is the drive mounted?)");
        return 0;
    }
    return 1;
}

/* Runs sql and returns its first row's first
 * column as a number; -1 on error (logged). */
static long long single_number(const char *sql)
{
    sqlite3_stmt *st = db_prepare(music_db, sql);
    if (st == NULL)
        return -1;
    long long v = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : -1;
    if (v < 0)
        db_log_error(music_db, sql);
    sqlite3_finalize(st);
    return v;
}

/* Adds key: [names of the fields whose bit is set in bits]. NULL when out
 * of memory. */
static cJSON *add_field_names(cJSON *obj, const char *key, unsigned bits)
{
    cJSON *list = cJSON_AddArrayToObject(obj, key);
    for (int i = 0; list != NULL && i < TAG_FIELDS; i++) {
        if ((bits & (1u << i)) == 0)
            continue;
        cJSON *name = cJSON_CreateString(tags_name[i]);
        if (name == NULL || !cJSON_AddItemToArray(list, name))
            return NULL;
    }
    return list;
}

/* ---- audit -------------------------------------------------------------- */

/* Records that an action starts. Its row id, or -1 (logged): then the
 * action must not happen. */
static long audit_begin(const struct request *req, const char *action, const cJSON *detail)
{
    char *text = cJSON_PrintUnformatted(detail);
    sqlite3_stmt *st = text != NULL
        ? db_prepare(music_db, "INSERT INTO audit (client, action, detail, result)"
                               " VALUES (?, ?, ?, 'started')")
        : NULL;
    int ok = st != NULL &&
             sqlite3_bind_text(st, 1, req->client ? req->client : "-", -1, SQLITE_STATIC) ==
                 SQLITE_OK &&
             sqlite3_bind_text(st, 2, action, -1, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_bind_text(st, 3, text, -1, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_step(st) == SQLITE_DONE;
    if (!ok)
        fprintf(stderr, "music: can not write the audit log: %s\n", sqlite3_errmsg(music_db));
    sqlite3_finalize(st);
    return ok ? (long)sqlite3_last_insert_rowid(music_db) : -1;
}

/* Records an action's result (logged as well when it failed). */
static void audit_end(long id, const char *result)
{
    sqlite3_stmt *st = db_prepare(music_db, "UPDATE audit SET result = ? WHERE id = ?");
    int ok = st != NULL && sqlite3_bind_text(st, 1, result, -1, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_bind_int64(st, 2, id) == SQLITE_OK && sqlite3_step(st) == SQLITE_DONE;
    if (!ok)
        fprintf(stderr, "music: can not record audit result %ld '%s': %s\n", id, result,
                sqlite3_errmsg(music_db));
    sqlite3_finalize(st);
    if (strncmp(result, "ok", 2) != 0 && strcmp(result, "started") != 0)
        fprintf(stderr, "music: audit %ld: %s\n", id, result);
}

/* ---- reads -------------------------------------------------------------- */

/* GET /api/music: whether music is set up, counts, and the last scan. */
void music_overview(struct request *req, struct response *res)
{
    (void)req;
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL)
        goto fail;
    int configured = music_root() != NULL;
    if (cJSON_AddBoolToObject(obj, "configured", configured) == NULL ||
        cJSON_AddBoolToObject(obj, "available", configured && music_available()) == NULL)
        goto fail;

    long long albums = single_number("SELECT count(*) FROM albums");
    long long tracks = single_number("SELECT count(*) FROM tracks");
    int running = music_scan_running();
    if (albums < 0 || tracks < 0 || running < 0 ||
        cJSON_AddNumberToObject(obj, "albums", (double)albums) == NULL ||
        cJSON_AddNumberToObject(obj, "tracks", (double)tracks) == NULL)
        goto fail;

    sqlite3_stmt *st = db_prepare(music_db,
        "SELECT started, finished, files, parsed, failed, ok FROM scans ORDER BY id DESC LIMIT 1");
    if (st == NULL)
        goto fail;
    int rc = sqlite3_step(st);
    cJSON *scan = rc == SQLITE_ROW ? json_row(st, 6) : cJSON_CreateNull();
    const char *state = running ? "running"
                      : rc != SQLITE_ROW ? "never"
                      : sqlite3_column_type(st, 1) == SQLITE_NULL ? "interrupted"
                      : sqlite3_column_int(st, 5) ? "done" : "failed";
    sqlite3_finalize(st);
    if ((rc != SQLITE_ROW && rc != SQLITE_DONE) || scan == NULL ||
        !cJSON_AddItemToObject(obj, "scan", scan) ||
        cJSON_AddStringToObject(obj, "scan_state", state) == NULL)
        goto fail;
    json_reply(res, 200, obj);
    return;
fail:
    fprintf(stderr, "music: overview failed\n");
    json_error(res, 500, "internal error");
}

/* The value every track of an album shares for column f, else NULL. */
#define SHARED(f) \
    "CASE WHEN min(t." f ") IS max(t." f ") AND count(t." f ") IN (0, count(*))" \
    " THEN min(t." f ") END"
#define MIXED(f, bit) \
    " + (CASE WHEN min(t." f ") IS max(t." f ") AND count(t." f ") IN (0, count(*))" \
    " THEN 0 ELSE " #bit " END)"

/* GET /api/music/albums: every album with the values its tracks share.
 * A field in "mixed" differs between tracks (or is missing on some). */
void music_albums(struct request *req, struct response *res)
{
    (void)req;
    cJSON *list = cJSON_CreateArray();
    sqlite3_stmt *st = list == NULL ? NULL : db_prepare(music_db,
        "SELECT a.id, a.dir, count(*) AS tracks, sum(t.pictures > 0) AS with_art,"
        " " SHARED("album") " AS album, " SHARED("albumartist") " AS albumartist,"
        " " SHARED("date") " AS date, " SHARED("genre") " AS genre,"
        " 0" MIXED("album", 4) MIXED("albumartist", 8) MIXED("genre", 16) MIXED("date", 32)
        " AS mixed"
        " FROM albums a JOIN tracks t ON t.album_id = a.id"
        " GROUP BY a.id"
        " ORDER BY min(t.albumartist) COLLATE NOCASE, min(t.album) COLLATE NOCASE, a.dir");
    if (st == NULL) {
        json_error(res, 500, "internal error");
        return;
    }
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        cJSON *obj = json_row(st, 8);
        if (obj == NULL || !cJSON_AddItemToArray(list, obj) ||
            add_field_names(obj, "mixed", (unsigned)sqlite3_column_int(st, 8)) == NULL) {
            rc = SQLITE_NOMEM;
            break;
        }
    }
    if (rc != SQLITE_DONE)
        db_log_error(music_db, "albums");
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, list);
}

/* Reads a positive id from the query string; replies 400 if invalid. */
static int query_id(const struct request *req, struct response *res, long *out)
{
    const char *s = NULL;
    if (http_query(req, "id", &s) == 0 && s[0] >= '1' && s[0] <= '9') {
        char *end;
        errno = 0;
        long v = strtol(s, &end, 10);
        if (errno == 0 && *end == '\0' && v <= ID_MAX) {
            *out = v;
            return 0;
        }
    }
    json_error(res, 400, "query parameter 'id' must be a positive whole number");
    return -1;
}

/* The album's folder, copied into the arena; NULL if it does not exist
 * (*error set on a database error). */
static const char *album_dir(long id, int *error)
{
    sqlite3_stmt *st = db_prepare(music_db, "SELECT dir FROM albums WHERE id = ?");
    *error = st == NULL || sqlite3_bind_int64(st, 1, id) != SQLITE_OK;
    const char *dir = NULL;
    if (!*error) {
        int rc = sqlite3_step(st);
        if (rc == SQLITE_ROW)
            dir = arena_strndup((const char *)sqlite3_column_text(st, 0),
                                (size_t)sqlite3_column_bytes(st, 0));
        *error = (rc != SQLITE_ROW && rc != SQLITE_DONE) || (rc == SQLITE_ROW && dir == NULL);
    }
    if (*error)
        db_log_error(music_db, "album dir");
    sqlite3_finalize(st);
    return dir;
}

/* The columns a track is read with; json_row() takes the first 13. */
#define TRACK_COLUMNS                                                          \
    "id, format, seconds, pictures, title, artist, album, albumartist, genre," \
    " date, tracknumber, discnumber, compilation, path, multi, size, mtime"
#define TRACK_JSON_COLUMNS 13
#define TRACK_ORDER \
    " ORDER BY CAST(discnumber AS INTEGER), CAST(tracknumber AS INTEGER), path"

/* GET /api/music/album?id=N: the album's folder and its tracks' tags. A
 * field in a track's "locked" has several values and can not be changed. */
void music_album(struct request *req, struct response *res)
{
    long id;
    if (query_id(req, res, &id) != 0)
        return;
    int error;
    const char *dir = album_dir(id, &error);
    if (dir == NULL) {
        json_error(res, error ? 500 : 404, error ? "internal error" : "album not found");
        return;
    }
    cJSON *obj = cJSON_CreateObject();
    cJSON *tracks = obj != NULL ? cJSON_AddArrayToObject(obj, "tracks") : NULL;
    sqlite3_stmt *st = tracks != NULL
        ? db_prepare(music_db, "SELECT " TRACK_COLUMNS " FROM tracks WHERE album_id = ?"
                               TRACK_ORDER)
        : NULL;
    int rc = st != NULL && sqlite3_bind_int64(st, 1, id) == SQLITE_OK &&
                     cJSON_AddNumberToObject(obj, "id", (double)id) != NULL &&
                     cJSON_AddStringToObject(obj, "dir", dir) != NULL
                 ? SQLITE_ROW
                 : SQLITE_ERROR;
    while (rc == SQLITE_ROW && (rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(st, 13);
        const char *slash = strrchr(path, '/');
        cJSON *t = json_row(st, TRACK_JSON_COLUMNS);
        if (t == NULL || !cJSON_AddItemToArray(tracks, t) ||
            cJSON_AddStringToObject(t, "file", slash != NULL ? slash + 1 : path) == NULL ||
            add_field_names(t, "locked", (unsigned)sqlite3_column_int(st, 14)) == NULL)
            rc = SQLITE_NOMEM;
    }
    if (rc != SQLITE_DONE) {
        db_log_error(music_db, "album tracks");
        sqlite3_finalize(st);
        json_error(res, 500, "internal error");
        return;
    }
    sqlite3_finalize(st);
    json_reply(res, 200, obj);
}

/* ---- saving an album ---------------------------------------------------- */

struct track {
    long id;
    const char *path; /* relative to the music folder */
    const char *value[TAG_FIELDS];
    unsigned multi;
    int requested;    /* listed in the request */
    int changes;      /* fields to set */
    struct tags_edit edit;
};

/* Loads the album's tracks into the arena. Their count, or -1 (500). */
static int load_tracks(long album_id, struct track **out)
{
    sqlite3_stmt *st = db_prepare(music_db, "SELECT " TRACK_COLUMNS
                                            " FROM tracks WHERE album_id = ?" TRACK_ORDER);
    struct track *list = arena_alloc(MAX_TRACKS * sizeof *list);
    if (st == NULL || list == NULL || sqlite3_bind_int64(st, 1, album_id) != SQLITE_OK) {
        sqlite3_finalize(st);
        return -1;
    }
    int n = 0, rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (n == MAX_TRACKS) {
            fprintf(stderr, "music: album %ld has more than %d tracks\n", album_id, MAX_TRACKS);
            rc = SQLITE_FULL;
            break;
        }
        struct track *t = &list[n++];
        memset(t, 0, sizeof *t);
        t->id = (long)sqlite3_column_int64(st, 0);
        t->edit.format = strcmp((const char *)sqlite3_column_text(st, 1), "mp3") == 0
                             ? TAGS_MP3 : TAGS_FLAC;
        for (int i = 0; i < TAG_FIELDS; i++) {
            const char *v = (const char *)sqlite3_column_text(st, 4 + i);
            if (v != NULL &&
                (t->value[i] = arena_strndup(v, (size_t)sqlite3_column_bytes(st, 4 + i))) == NULL)
                rc = SQLITE_NOMEM;
        }
        t->path = arena_strndup((const char *)sqlite3_column_text(st, 13),
                                (size_t)sqlite3_column_bytes(st, 13));
        t->multi = (unsigned)sqlite3_column_int(st, 14);
        t->edit.size = sqlite3_column_int64(st, 15);
        t->edit.mtime = sqlite3_column_int64(st, 16);
        if (t->path == NULL || rc == SQLITE_NOMEM) {
            rc = SQLITE_NOMEM;
            break;
        }
    }
    if (rc != SQLITE_DONE)
        db_log_error(music_db, "load album tracks");
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        return -1;
    *out = list;
    return n;
}

/*
 * Reads obj[tags_name[f]] into *out: absent or null keeps the tag (NULL);
 * a string is the new value ("" removes it); compilation is true / false.
 * NULL, or an error message.
 */
static const char *get_tag(const cJSON *obj, enum tag_field f, const char **out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, tags_name[f]);
    *out = NULL;
    if (item == NULL || cJSON_IsNull(item))
        return NULL;
    if (f == TAG_COMPILATION) {
        if (!cJSON_IsBool(item))
            return "'compilation' must be true, false or null";
        *out = cJSON_IsTrue(item) ? "1" : "";
        return NULL;
    }
    if (!cJSON_IsString(item))
        return message("'%s' must be a string or null%s", tags_name[f], "");
    const char *why = tags_check_value(f, item->valuestring);
    if (why != NULL)
        return message("'%s' %s", tags_name[f], why);
    *out = item->valuestring;
    return NULL;
}

/* Rejects keys of obj other than "id" (if allow_id) and the n fields. */
static const char *only_fields(const cJSON *obj, const enum tag_field *fields, size_t n,
                               int allow_id)
{
    for (const cJSON *item = obj->child; item != NULL; item = item->next) {
        int known = allow_id && strcmp(item->string, "id") == 0;
        for (size_t i = 0; i < n && !known; i++)
            known = strcmp(item->string, tags_name[fields[i]]) == 0;
        if (!known)
            return message("unknown field '%.100s'%s", item->string, "");
    }
    return NULL;
}

/*
 * Applies the request's values to the tracks' edits: album fields to every
 * track, then each listed track's own. NULL, or an error message.
 */
static const char *read_changes(const cJSON *body, struct track *tracks, int n)
{
    const cJSON *album = cJSON_GetObjectItemCaseSensitive(body, "album");
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(body, "tracks");
    if (!cJSON_IsObject(album))
        return "'album' must be an object";
    if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) > MAX_TRACKS)
        return "'tracks' must be a list of at most 500 tracks";
    const char *err = only_fields(album, album_fields, NALBUM_FIELDS, 0);
    for (size_t i = 0; i < NALBUM_FIELDS && err == NULL; i++) {
        const char *v;
        err = get_tag(album, album_fields[i], &v);
        for (int k = 0; k < n && err == NULL; k++)
            tracks[k].edit.set[album_fields[i]] = v;
    }

    const cJSON *item;
    cJSON_ArrayForEach(item, list) {
        if (err != NULL)
            break;
        long id;
        if (!cJSON_IsObject(item))
            return "each track must be an object";
        if ((err = only_fields(item, track_fields, NTRACK_FIELDS, 1)) != NULL ||
            (err = json_get_int(item, "id", 1, ID_MAX, &id)) != NULL)
            break;
        struct track *t = NULL;
        for (int k = 0; k < n && t == NULL; k++)
            if (tracks[k].id == id)
                t = &tracks[k];
        if (t == NULL)
            return "a track in 'tracks' is not in this album";
        if (t->requested)
            return "a track is listed twice in 'tracks'";
        t->requested = 1;
        for (size_t i = 0; i < NTRACK_FIELDS && err == NULL; i++)
            err = get_tag(item, track_fields[i], &t->edit.set[track_fields[i]]);
    }
    return err;
}

/*
 * Drops values a track already has, so only real changes are written.
 * Returns the first track and field that has several values and would be
 * changed (*field set), else NULL.
 */
static struct track *drop_unchanged(struct track *tracks, int n, int *field)
{
    for (int k = 0; k < n; k++) {
        struct track *t = &tracks[k];
        for (int i = 0; i < TAG_FIELDS; i++) {
            const char *v = t->edit.set[i];
            if (v == NULL)
                continue;
            if (t->multi & (1u << i)) {
                *field = i;
                return t;
            }
            if (t->value[i] != NULL ? strcmp(t->value[i], v) == 0 : v[0] == '\0')
                t->edit.set[i] = NULL;
            else
                t->changes++;
        }
    }
    return NULL;
}

/* {"album": dir, "files": [{"path", "set": {field: [old, new]}}]}; NULL
 * when out of memory. */
static cJSON *change_list(const char *dir, const struct track *tracks, int n)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON *files = obj != NULL && cJSON_AddStringToObject(obj, "album", dir) != NULL
                       ? cJSON_AddArrayToObject(obj, "files") : NULL;
    if (files == NULL)
        return NULL;
    for (int k = 0; k < n; k++) {
        if (tracks[k].changes == 0)
            continue;
        cJSON *f = cJSON_CreateObject();
        cJSON *set = f != NULL && cJSON_AddItemToArray(files, f) &&
                             cJSON_AddStringToObject(f, "path", tracks[k].path) != NULL
                         ? cJSON_AddObjectToObject(f, "set") : NULL;
        if (set == NULL)
            return NULL;
        for (int i = 0; i < TAG_FIELDS; i++) {
            if (tracks[k].edit.set[i] == NULL)
                continue;
            cJSON *pair = cJSON_AddArrayToObject(set, tags_name[i]);
            cJSON *old = tracks[k].value[i] != NULL ? cJSON_CreateString(tracks[k].value[i])
                                                    : cJSON_CreateNull();
            cJSON *new = cJSON_CreateString(tracks[k].edit.set[i]);
            if (pair == NULL || old == NULL || new == NULL || !cJSON_AddItemToArray(pair, old) ||
                !cJSON_AddItemToArray(pair, new))
                return NULL;
        }
    }
    return obj;
}

/* Prepares every changed file. 0, or the HTTP status to reply with (msg set);
 * then no copy is left. */
static int prepare_all(struct track *tracks, int n, const char **msg)
{
    char err[256];
    for (int k = 0; k < n; k++) {
        struct track *t = &tracks[k];
        if (t->changes == 0)
            continue;
        char *path = arena_alloc(TAGS_MAX_PATH);
        int len = path != NULL ? snprintf(path, TAGS_MAX_PATH, "%s/%s", music_root(), t->path)
                               : -1;
        int rc = -1;
        snprintf(err, sizeof err, "path too long");
        if (len >= 0 && len < TAGS_MAX_PATH) {
            t->edit.path = path;
            rc = tags_prepare(&t->edit, err, sizeof err);
        }
        if (rc != 0) {
            for (int j = 0; j < k; j++)
                tags_discard(&tracks[j].edit);
            *msg = message(rc == TAGS_STALE ? "%s: %s; scan the library again" : "%s: %s",
                           t->path, err);
            return rc == TAGS_STALE || rc == TAGS_LOCKED ? 409 : 500;
        }
    }
    return 0;
}

/* POST /api/music/album/save {id, album: {fields}, tracks: [{id, fields}]}
 * Fields absent or null are kept. -> 200 {written: number of files} */
void music_album_save(struct request *req, struct response *res)
{
    cJSON *body = json_body(req, res);
    long id;
    if (body == NULL)
        return;
    const char *err = json_get_int(body, "id", 1, ID_MAX, &id);
    if (err != NULL) {
        json_error(res, 400, err);
        return;
    }
    if (!library_ready(res))
        return;
    int error;
    const char *dir = album_dir(id, &error);
    if (dir == NULL) {
        json_error(res, error ? 500 : 404, error ? "internal error" : "album not found");
        return;
    }
    struct track *tracks;
    int n = load_tracks(id, &tracks);
    if (n < 0) {
        json_error(res, 500, "internal error");
        return;
    }
    if ((err = read_changes(body, tracks, n)) != NULL) {
        json_error(res, 400, err);
        return;
    }
    int field;
    struct track *locked = drop_unchanged(tracks, n, &field);
    if (locked != NULL) {
        json_error(res, 409, message("%s: %s has several values; nylm does not change those",
                                     locked->path, tags_name[field]));
        return;
    }
    int changed = 0;
    for (int k = 0; k < n; k++)
        changed += tracks[k].changes > 0;
    cJSON *out = cJSON_CreateObject();
    if (out == NULL || cJSON_AddNumberToObject(out, "written", changed) == NULL) {
        json_error(res, 500, "internal error");
        return;
    }
    if (changed == 0) {
        json_reply(res, 200, out);
        return;
    }

    cJSON *detail = change_list(dir, tracks, n);
    long audit = detail != NULL ? audit_begin(req, "tags", detail) : -1;
    if (audit < 0) {
        json_error(res, 500, "can not write the audit log; nothing was changed");
        return;
    }
    const char *msg = NULL;
    int status = prepare_all(tracks, n, &msg);
    if (status != 0) {
        audit_end(audit, message("failed, nothing written: %s%s", msg, ""));
        json_error(res, status, msg);
        return;
    }

    int written = 0;
    char why[256];
    for (int k = 0; k < n; k++) {
        if (tracks[k].changes == 0)
            continue;
        int rc = tags_commit(&tracks[k].edit, why, sizeof why);
        if (rc != 0) {
            for (int j = k + 1; j < n; j++)
                tags_discard(&tracks[j].edit);
            char counts[64];
            snprintf(counts, sizeof counts, "%d of %d files written", written, changed);
            msg = message("%s; stopped at %s", counts, tracks[k].path);
            audit_end(audit, message("failed: %s: %s", msg, why));
            json_error(res, rc == TAGS_STALE ? 409 : 500, message("%s: %s", msg, why));
            for (int j = 0; j < k; j++)
                if (tracks[j].changes > 0)
                    music_reread(tracks[j].id);
            return;
        }
        written++;
    }
    int stale = 0;
    for (int k = 0; k < n; k++)
        if (tracks[k].changes > 0 && music_reread(tracks[k].id) != 0)
            stale++;
    char result[96];
    snprintf(result, sizeof result, "ok: %d files written%s", written,
             stale ? "; cache not refreshed for some, scan again" : "");
    audit_end(audit, result);
    json_reply(res, 200, out);
}

/* ---- scan --------------------------------------------------------------- */

/* POST /api/music/scan {password}: starts nylm-music-scan.service through
 * the root action music-scan. -> 202 */
void music_scan_start(struct request *req, struct response *res)
{
    cJSON *body = json_body(req, res);
    const char *password;
    if (body == NULL)
        return;
    if (json_get_string(body, "password", 1, AUTH_MAX_PASSWORD, &password) != NULL) {
        json_error(res, 400, "'password' is required");
        return;
    }
    if (!library_ready(res))
        return;
    int ok = auth_check_password(password);
    if (ok < 0) {
        json_error(res, 500, "internal error");
        return;
    }
    if (ok == 0) {
        sleep(PASSWORD_FAILURE_DELAY_SECONDS);
        json_error(res, 403, "wrong password");
        return;
    }
    int running = music_scan_running();
    if (running != 0) {
        json_error(res, running > 0 ? 409 : 500,
                   running > 0 ? "a scan is already running" : "internal error");
        return;
    }
    cJSON *detail = cJSON_CreateObject();
    long audit = detail != NULL ? audit_begin(req, "scan", detail) : -1;
    if (audit < 0) {
        json_error(res, 500, "can not write the audit log; the scan was not started");
        return;
    }
    if (action_run("music-scan") != 0) {
        audit_end(audit, "failed: the action music-scan did not start the scan");
        json_error(res, 500, "could not start the scan; see the server log");
        return;
    }
    audit_end(audit, "ok: scan started");
    cJSON *out = cJSON_CreateObject();
    json_reply(res, 202, out);
}
