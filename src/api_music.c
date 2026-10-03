/*
 * Music library: albums, their tracks' tags, and the changes to them
 * (src/music.c). The server never opens a music file: it reads the cache
 * in music_db, queues tag changes in the changes table, and starts the
 * services that work on the files (scan, write), each after asking for the
 * password again and recording it in the audit table.
 *
 * While a service runs the server writes nothing to music_db: every write
 * here holds the library lock shared, and answers 409 if a service has it.
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

#define ID_MAX       9007199254740991L /* 2^53 - 1: exact in a JSON number */
#define MAX_TRACKS   500               /* tracks in one album (folder) */
#define MAX_CANCEL   1000              /* change ids in one cancel request */

/* Every failed password check costs the caller this long, as for login. */
#define PASSWORD_FAILURE_DELAY_SECONDS 1

#define BUSY_MESSAGE "the library is busy: a scan or write is running; try again when it is done"

/* Fields set for the whole album, and per track. */
static const enum tag_field album_fields[] = {
    TAG_ALBUM, TAG_ALBUMARTIST, TAG_GENRE, TAG_DATE, TAG_COMPILATION,
};
static const enum tag_field track_fields[] = {
    TAG_TITLE, TAG_ARTIST, TAG_TRACKNUMBER, TAG_DISCNUMBER,
};
/* Tags nylm does not change while a file has several values: all but the
 * genre, whose values are replaced by the one new string. */
#define LOCKABLE (~(1u << TAG_GENRE))

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

/*
 * Takes the library lock for a write to music_db. The fd, or -1 after
 * replying 409 (a service runs) or 500.
 */
static int lock_for_write(struct response *res)
{
    int fd = music_lock_shared();
    if (fd == MUSIC_BUSY)
        json_error(res, 409, BUSY_MESSAGE);
    else if (fd < 0)
        json_error(res, 500, "internal error");
    return fd >= 0 ? fd : -1;
}

/* Runs sql and returns its first row's first column as a number; -1 on
 * error (logged). */
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

/* Runs a statement that returns no rows and finalizes it. 0 or -1 (logged). */
static int run_once(sqlite3_stmt *st)
{
    int rc = st != NULL ? sqlite3_step(st) : SQLITE_ERROR;
    if (st != NULL && rc != SQLITE_DONE)
        db_log_error(music_db, sqlite3_sql(st));
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
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

/* Appends one JSON object per row of st (its first ncols columns) to list.
 * 0, or -1 on error (logged). */
static int add_rows(cJSON *list, sqlite3_stmt *st, int ncols)
{
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        cJSON *obj = json_row(st, ncols);
        if (obj == NULL || !cJSON_AddItemToArray(list, obj)) {
            fprintf(stderr, "music: out of memory building a list\n");
            return -1;
        }
    }
    if (rc != SQLITE_DONE)
        db_log_error(music_db, sqlite3_sql(st));
    return rc == SQLITE_DONE ? 0 : -1;
}

/* ---- audit -------------------------------------------------------------- */

/* Records that an action starts (the caller holds the library lock). Its
 * row id, or -1 (logged): then the action must not happen. */
static long audit_begin(const struct request *req, const char *action)
{
    sqlite3_stmt *st = db_prepare(music_db, "INSERT INTO audit (client, action, detail, result)"
                                            " VALUES (?, ?, '{}', 'started')");
    int ok = st != NULL &&
             sqlite3_bind_text(st, 1, req->client ? req->client : "-", -1, SQLITE_STATIC) ==
                 SQLITE_OK &&
             sqlite3_bind_text(st, 2, action, -1, SQLITE_STATIC) == SQLITE_OK &&
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
    if (strncmp(result, "ok", 2) != 0)
        fprintf(stderr, "music: audit %ld: %s\n", id, result);
}

/* ---- reads -------------------------------------------------------------- */

/* GET /api/music: whether music is set up, counts, pending changes, which
 * service runs (busy: "scan", "write" or null) and the last scan. */
void music_overview(struct request *req, struct response *res)
{
    (void)req;
    int error;
    const char *busy = music_busy(&error);
    cJSON *obj = cJSON_CreateObject();
    int configured = music_root() != NULL;
    long long albums = single_number("SELECT count(*) FROM albums");
    long long tracks = single_number("SELECT count(*) FROM tracks");
    long long pending = single_number("SELECT count(*) FROM changes WHERE state = 'pending'");
    if (error || obj == NULL || albums < 0 || tracks < 0 || pending < 0 ||
        cJSON_AddBoolToObject(obj, "configured", configured) == NULL ||
        cJSON_AddBoolToObject(obj, "available", configured && music_available()) == NULL ||
        cJSON_AddNumberToObject(obj, "albums", (double)albums) == NULL ||
        cJSON_AddNumberToObject(obj, "tracks", (double)tracks) == NULL ||
        cJSON_AddNumberToObject(obj, "pending", (double)pending) == NULL ||
        (busy != NULL ? cJSON_AddStringToObject(obj, "busy", busy)
                      : cJSON_AddNullToObject(obj, "busy")) == NULL)
        goto fail;

    sqlite3_stmt *st = db_prepare(music_db,
        "SELECT started, finished, files, parsed, failed, ok FROM scans ORDER BY id DESC LIMIT 1");
    if (st == NULL)
        goto fail;
    int rc = sqlite3_step(st);
    cJSON *scan = rc == SQLITE_ROW ? json_row(st, 6) : cJSON_CreateNull();
    int scanning = busy != NULL && strcmp(busy, "scan") == 0;
    const char *state = scanning ? "running"
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
 * A field in "mixed" differs between tracks (or is missing on some);
 * pending counts the album's changes waiting to be written. */
void music_albums(struct request *req, struct response *res)
{
    (void)req;
    cJSON *list = cJSON_CreateArray();
    sqlite3_stmt *st = list == NULL ? NULL : db_prepare(music_db,
        "SELECT a.id, a.dir, count(*) AS tracks, sum(t.pictures > 0) AS with_art,"
        " " SHARED("album") " AS album, " SHARED("albumartist") " AS albumartist,"
        " " SHARED("date") " AS date, " SHARED("genre") " AS genre,"
        " (SELECT count(*) FROM changes c JOIN tracks p ON p.id = c.track_id"
        "  WHERE p.album_id = a.id AND c.state = 'pending') AS pending,"
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
        cJSON *obj = json_row(st, 9);
        if (obj == NULL || !cJSON_AddItemToArray(list, obj) ||
            add_field_names(obj, "mixed", (unsigned)sqlite3_column_int(st, 9)) == NULL) {
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
    " date, tracknumber, discnumber, compilation, path, multi"
#define TRACK_JSON_COLUMNS 13
#define TRACK_ORDER \
    " ORDER BY CAST(discnumber AS INTEGER), CAST(tracknumber AS INTEGER), path"

/* The track object in list with this id, or NULL. */
static cJSON *track_by_id(const cJSON *list, long id)
{
    cJSON *t;
    cJSON_ArrayForEach(t, list) {
        const cJSON *tid = cJSON_GetObjectItemCaseSensitive(t, "id");
        if (cJSON_IsNumber(tid) && (long)tid->valuedouble == id)
            return t;
    }
    return NULL;
}

/* Adds to each track of the album its pending changes: "pending":
 * {field: new value}. 0 or -1 (logged). */
static int add_pending(cJSON *tracks, long album_id)
{
    sqlite3_stmt *st = db_prepare(music_db,
        "SELECT c.track_id, c.field, c.new FROM changes c JOIN tracks t ON t.id = c.track_id"
        " WHERE t.album_id = ? AND c.state = 'pending' ORDER BY c.id");
    if (st == NULL || sqlite3_bind_int64(st, 1, album_id) != SQLITE_OK) {
        sqlite3_finalize(st);
        return -1;
    }
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        cJSON *t = track_by_id(tracks, (long)sqlite3_column_int64(st, 0));
        cJSON *pending = cJSON_GetObjectItemCaseSensitive(t, "pending");
        if (pending == NULL ||
            cJSON_AddStringToObject(pending, (const char *)sqlite3_column_text(st, 1),
                                    (const char *)sqlite3_column_text(st, 2)) == NULL) {
            rc = SQLITE_NOMEM;
            break;
        }
    }
    if (rc != SQLITE_DONE)
        db_log_error(music_db, "album pending");
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* GET /api/music/album?id=N: the album's folder and its tracks' tags. A
 * field in a track's "locked" has several values and can not be changed;
 * "pending" holds the changes queued for it. */
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
            add_field_names(t, "locked", (unsigned)sqlite3_column_int(st, 14) & LOCKABLE) ==
                NULL ||
            cJSON_AddObjectToObject(t, "pending") == NULL)
            rc = SQLITE_NOMEM;
    }
    if (rc != SQLITE_DONE)
        db_log_error(music_db, "album tracks");
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE || add_pending(tracks, id) != 0) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, obj);
}

/* GET /api/music/changes: the pending changes, and the latest written ones
 * (done, warning, failed) with their notes. album_id is null when a scan
 * removed the track. */
void music_changes(struct request *req, struct response *res)
{
    (void)req;
#define CHANGE_COLUMNS                                                                 \
    "SELECT c.id, t.album_id, c.path, c.field, c.old, c.new, c.state, c.note, c.queued," \
    " c.finished FROM changes c LEFT JOIN tracks t ON t.id = c.track_id"
    cJSON *obj = cJSON_CreateObject();
    cJSON *pending = obj != NULL ? cJSON_AddArrayToObject(obj, "pending") : NULL;
    cJSON *history = obj != NULL ? cJSON_AddArrayToObject(obj, "history") : NULL;
    sqlite3_stmt *a = pending == NULL ? NULL : db_prepare(music_db,
        CHANGE_COLUMNS " WHERE c.state = 'pending' ORDER BY c.path, c.id");
    sqlite3_stmt *b = history == NULL ? NULL : db_prepare(music_db,
        CHANGE_COLUMNS " WHERE c.state <> 'pending' ORDER BY c.finished DESC, c.id DESC"
        " LIMIT 200");
    int ok = a != NULL && b != NULL && add_rows(pending, a, 10) == 0 &&
             add_rows(history, b, 10) == 0;
    sqlite3_finalize(a);
    sqlite3_finalize(b);
    if (!ok) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, obj);
}

/* ---- queueing changes --------------------------------------------------- */

struct track {
    long id;
    const char *path; /* relative to the music folder */
    const char *value[TAG_FIELDS];
    unsigned multi;
    unsigned pending; /* bit (1u << field): a change is pending */
    int requested;    /* listed in the request */
    const char *set[TAG_FIELDS]; /* NULL: not in the request */
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
        for (int i = 0; i < TAG_FIELDS; i++) {
            const char *v = (const char *)sqlite3_column_text(st, 4 + i);
            if (v != NULL &&
                (t->value[i] = arena_strndup(v, (size_t)sqlite3_column_bytes(st, 4 + i))) == NULL)
                rc = SQLITE_NOMEM;
        }
        t->path = arena_strndup((const char *)sqlite3_column_text(st, 13),
                                (size_t)sqlite3_column_bytes(st, 13));
        t->multi = (unsigned)sqlite3_column_int(st, 14);
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
 * Reads obj[tags_name[f]] into *out: absent or null leaves the tag alone
 * (NULL); a string is the new value ("" removes it); compilation is true /
 * false. NULL, or an error message.
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
 * Reads the request's values into the tracks: album fields for every
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
            tracks[k].set[album_fields[i]] = v;
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
            err = get_tag(item, track_fields[i], &t->set[track_fields[i]]);
    }
    return err;
}

/* Marks which fields of the album's tracks have a pending change. 0 or -1
 * (logged). */
static int load_pending(long album_id, struct track *tracks, int n)
{
    sqlite3_stmt *st = db_prepare(music_db,
        "SELECT c.track_id, c.field FROM changes c JOIN tracks t ON t.id = c.track_id"
        " WHERE t.album_id = ? AND c.state = 'pending'");
    if (st == NULL || sqlite3_bind_int64(st, 1, album_id) != SQLITE_OK) {
        sqlite3_finalize(st);
        return -1;
    }
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        long id = (long)sqlite3_column_int64(st, 0);
        int field = tags_field_of((const char *)sqlite3_column_text(st, 1));
        for (int k = 0; k < n && field >= 0; k++)
            if (tracks[k].id == id)
                tracks[k].pending |= 1u << field;
    }
    if (rc != SQLITE_DONE)
        db_log_error(music_db, "album pending fields");
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* The first track and field with several values that the request would
 * change (*field set), else NULL. */
static const struct track *find_locked(const struct track *tracks, int n, int *field)
{
    for (int k = 0; k < n; k++)
        for (int i = 0; i < TAG_FIELDS; i++)
            if (tracks[k].set[i] != NULL && (tracks[k].multi & LOCKABLE & (1u << i))) {
                *field = i;
                return &tracks[k];
            }
    return NULL;
}

/*
 * The first track that would still have no track or disc number (*field
 * set): its file has none, none is pending and the request sets none.
 * (A value set is never empty: those can not be removed.)
 */
static const struct track *find_missing(const struct track *tracks, int n, int *field)
{
    static const enum tag_field required[] = { TAG_TRACKNUMBER, TAG_DISCNUMBER };
    for (int k = 0; k < n; k++)
        for (size_t i = 0; i < sizeof required / sizeof required[0]; i++) {
            const struct track *t = &tracks[k];
            enum tag_field f = required[i];
            if ((t->value[f] == NULL || t->value[f][0] == '\0') &&
                !(t->pending & (1u << f)) && t->set[f] == NULL) {
                *field = f;
                return t;
            }
        }
    return NULL;
}

/*
 * Queues t's field as change (or, if the file already has that value,
 * drops a pending change for it). Adds 1 to *queued or *dropped when it
 * did. 0 or -1 (logged).
 */
static int queue_one(const struct request *req, const struct track *t, int field, int *queued,
                     int *dropped)
{
    const char *v = t->set[field];
    int same = t->value[field] != NULL ? strcmp(t->value[field], v) == 0 : v[0] == '\0';
    sqlite3_stmt *st;
    if (same) {
        st = db_prepare(music_db, "DELETE FROM changes"
                                  " WHERE state = 'pending' AND track_id = ? AND field = ?");
        if (st == NULL || sqlite3_bind_int64(st, 1, t->id) != SQLITE_OK ||
            sqlite3_bind_text(st, 2, tags_name[field], -1, SQLITE_STATIC) != SQLITE_OK) {
            sqlite3_finalize(st);
            return -1;
        }
    } else {
        st = db_prepare(music_db,
            "INSERT INTO changes (track_id, path, field, old, new, client)"
            " VALUES (?, ?, ?, ?, ?, ?)"
            " ON CONFLICT (track_id, field) WHERE state = 'pending' DO UPDATE SET"
            "  old = excluded.old, new = excluded.new, client = excluded.client,"
            "  queued = excluded.queued"
            " WHERE new IS NOT excluded.new");
        int rc = st != NULL ? sqlite3_bind_int64(st, 1, t->id) : SQLITE_ERROR;
        if (rc == SQLITE_OK)
            rc = sqlite3_bind_text(st, 2, t->path, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK)
            rc = sqlite3_bind_text(st, 3, tags_name[field], -1, SQLITE_STATIC);
        if (rc == SQLITE_OK)
            rc = t->value[field] != NULL
                     ? sqlite3_bind_text(st, 4, t->value[field], -1, SQLITE_STATIC)
                     : sqlite3_bind_null(st, 4);
        if (rc == SQLITE_OK)
            rc = sqlite3_bind_text(st, 5, v, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK)
            rc = sqlite3_bind_text(st, 6, req->client ? req->client : "-", -1, SQLITE_STATIC);
        if (rc != SQLITE_OK) {
            sqlite3_finalize(st);
            return -1;
        }
    }
    if (run_once(st) != 0)
        return -1;
    *(same ? dropped : queued) += sqlite3_changes(music_db) > 0;
    return 0;
}

/*
 * POST /api/music/album/save {id, album: {fields}, tracks: [{id, fields}]}
 * Queues the changes; nothing is written to the files until the write
 * service runs. Fields absent or null are left alone; a value the file
 * already has drops its pending change. -> 200 {queued, dropped}
 */
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
    if (music_root() == NULL) {
        json_error(res, 503, "music is not set up: set NYLM_MUSIC in /etc/nylm.conf");
        return;
    }
    int error;
    const char *dir = album_dir(id, &error);
    if (dir == NULL) {
        json_error(res, error ? 500 : 404, error ? "internal error" : "album not found");
        return;
    }
    struct track *tracks;
    int n = load_tracks(id, &tracks);
    if (n < 0 || load_pending(id, tracks, n) != 0) {
        json_error(res, 500, "internal error");
        return;
    }
    if ((err = read_changes(body, tracks, n)) != NULL) {
        json_error(res, 400, err);
        return;
    }
    int field;
    const struct track *locked = find_locked(tracks, n, &field);
    if (locked != NULL) {
        json_error(res, 409, message("%s: %s has several values; nylm does not change those",
                                     locked->path, tags_name[field]));
        return;
    }
    const struct track *missing = find_missing(tracks, n, &field);
    if (missing != NULL) {
        json_error(res, 400, message("%s has no %s: every track needs one before its album's "
                                     "changes can be queued", missing->path,
                                     field == TAG_TRACKNUMBER ? "track number" : "disc number"));
        return;
    }

    int lock = lock_for_write(res);
    if (lock < 0)
        return;
    int queued = 0, dropped = 0;
    int rc = db_exec(music_db, "BEGIN IMMEDIATE");
    for (int k = 0; k < n && rc == 0; k++)
        for (int i = 0; i < TAG_FIELDS && rc == 0; i++)
            if (tracks[k].set[i] != NULL)
                rc = queue_one(req, &tracks[k], i, &queued, &dropped);
    if (rc == 0)
        rc = db_exec(music_db, "COMMIT");
    if (rc != 0)
        db_exec(music_db, "ROLLBACK");
    music_unlock(lock);

    cJSON *out = cJSON_CreateObject();
    if (rc != 0 || out == NULL || cJSON_AddNumberToObject(out, "queued", queued) == NULL ||
        cJSON_AddNumberToObject(out, "dropped", dropped) == NULL) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, out);
}

/* POST /api/music/changes/cancel {ids: [change ids]}: deletes those that
 * are still pending. -> 200 {cancelled} */
void music_changes_cancel(struct request *req, struct response *res)
{
    cJSON *body = json_body(req, res);
    if (body == NULL)
        return;
    const cJSON *ids = cJSON_GetObjectItemCaseSensitive(body, "ids");
    if (!cJSON_IsArray(ids) || cJSON_GetArraySize(ids) < 1 ||
        cJSON_GetArraySize(ids) > MAX_CANCEL) {
        json_error(res, 400, "'ids' must be a list of 1 to 1000 change ids");
        return;
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, ids) {
        if (!cJSON_IsNumber(item) || item->valuedouble < 1 ||
            item->valuedouble > (double)ID_MAX ||
            item->valuedouble != (double)(long)item->valuedouble) {
            json_error(res, 400, "'ids' must be a list of 1 to 1000 change ids");
            return;
        }
    }

    int lock = lock_for_write(res);
    if (lock < 0)
        return;
    int cancelled = 0;
    int rc = db_exec(music_db, "BEGIN IMMEDIATE");
    cJSON_ArrayForEach(item, ids) {
        if (rc != 0)
            break;
        sqlite3_stmt *st = db_prepare(music_db,
            "DELETE FROM changes WHERE id = ? AND state = 'pending'");
        if (st == NULL || sqlite3_bind_int64(st, 1, (long)item->valuedouble) != SQLITE_OK) {
            sqlite3_finalize(st);
            rc = -1;
            break;
        }
        rc = run_once(st);
        cancelled += rc == 0 && sqlite3_changes(music_db) > 0;
    }
    if (rc == 0)
        rc = db_exec(music_db, "COMMIT");
    if (rc != 0)
        db_exec(music_db, "ROLLBACK");
    music_unlock(lock);

    cJSON *out = cJSON_CreateObject();
    if (rc != 0 || out == NULL || cJSON_AddNumberToObject(out, "cancelled", cancelled) == NULL) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, out);
}

/* ---- starting the services ---------------------------------------------- */

/*
 * Shared by POST /api/music/scan and /api/music/write {password}: checks
 * the password, then (holding the lock, so no service can start in
 * between) records the start in the audit table and runs the root action,
 * which starts the service. need_pending: refuse if nothing is queued.
 * -> 202
 */
static void start_service(struct request *req, struct response *res, const char *action,
                          int need_pending)
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

    int lock = lock_for_write(res);
    if (lock < 0)
        return;
    long long pending =
        need_pending ? single_number("SELECT count(*) FROM changes WHERE state = 'pending'") : 1;
    long audit = pending > 0 ? audit_begin(req, action) : -1;
    if (pending <= 0 || audit < 0) {
        music_unlock(lock);
        json_error(res, pending == 0 ? 409 : 500,
                   pending == 0 ? "there are no pending changes to write"
                                : "can not write the audit log; nothing was started");
        return;
    }
    /* The service waits a moment for this lock, so it starts after we
     * release it. */
    int started = action_run(action) == 0;
    audit_end(audit, started ? "ok: started" : "failed: the action did not start the service");
    music_unlock(lock);
    if (!started) {
        json_error(res, 500, "could not start it; see the server log");
        return;
    }
    json_reply(res, 202, cJSON_CreateObject());
}

/* POST /api/music/scan {password}: starts nylm-music-scan.service. */
void music_scan_start(struct request *req, struct response *res)
{
    start_service(req, res, "music-scan", 0);
}

/* POST /api/music/write {password}: starts nylm-music-write.service. */
void music_write_start(struct request *req, struct response *res)
{
    start_service(req, res, "music-write", 1);
}
