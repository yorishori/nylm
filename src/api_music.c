/*
 * Music library: albums, their tracks' tags, and the changes to them
 * (src/music.c). The server never opens a music file: it reads the cache
 * in music_db, queues tag changes and scans, and starts the services that
 * work on the files (scan, write), each after asking for the password
 * again and recording it in the audit table.
 *
 * An album is the tracks that share ALBUM and ALBUMARTIST in the files (the
 * cache); it is named by the id of any of its tracks. What the pages show
 * is planned: the files' values with the pending changes applied.
 *
 * While a service runs the server writes nothing to music_db: every write
 * here holds the library lock shared, and answers 409 if a service has it.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "action.h"
#include "api.h"
#include "arena.h"
#include "art.h"
#include "audit.h"
#include "db.h"
#include "dupes.h"
#include "json.h"
#include "move.h"
#include "music.h"
#include "qobuz.h"
#include "tags.h"

#define ID_MAX       9007199254740991LL /* 2^53 - 1: exact in a JSON number */
#define MAX_TRACKS   2000  /* tracks in one album page, one refresh */
#define MAX_EDITS    2000  /* track changes in one request */
#define MAX_LIST     5000  /* rows in a list of values or pending changes */
#define MAX_HISTORY  300   /* written changes listed */

#define BUSY_MESSAGE "the library is busy: a scan or write is running; try again when it is done"

/* The tags an album row shows (the same for all its tracks, or mixed). */
static const enum tag_field album_fields[] = {
    TAG_ALBUM, TAG_ALBUMARTIST, TAG_DATE, TAG_COMPILATION, TAG_GENRE, TAG_COMPOSER,
};
#define NALBUM_FIELDS (sizeof album_fields / sizeof album_fields[0])

/* The tracks of the album of track ?1, in a query on "tracks t". */
#define SAME_ALBUM                                                \
    " t.album IS (SELECT album FROM tracks WHERE id = ?1)"        \
    " AND t.albumartist IS (SELECT albumartist FROM tracks WHERE id = ?1)"

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

/* Runs sql (?1 bound to id, if sql has it) and returns its first row's
 * first column as a number; -1 on error (logged). */
static long long single_number(const char *sql, long long id)
{
    sqlite3_stmt *st = db_prepare(music_db, sql);
    if (st == NULL)
        return -1;
    long long v = -1;
    if (sqlite3_bind_parameter_count(st) == 0 || sqlite3_bind_int64(st, 1, id) == SQLITE_OK)
        v = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : -1;
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

/* Appends one JSON object per row of st (its first ncols columns) to list,
 * at most max. 0, or -1 on error (logged). */
static int add_rows(cJSON *list, sqlite3_stmt *st, int ncols, int max)
{
    int rc, n = 0;
    while (n < max && (rc = sqlite3_step(st)) == SQLITE_ROW) {
        cJSON *obj = json_row(st, ncols);
        if (obj == NULL || !cJSON_AddItemToArray(list, obj)) {
            fprintf(stderr, "music: out of memory building a list\n");
            return -1;
        }
        n++;
    }
    if (n == max)
        return 0;
    if (rc != SQLITE_DONE)
        db_log_error(music_db, sqlite3_sql(st));
    return rc == SQLITE_DONE ? 0 : -1;
}

/* Adds key: [names of the fields whose bit is set; "picture" for
 * MUSIC_COVER_BIT]. NULL when out of memory. */
static cJSON *add_field_names(cJSON *obj, const char *key, unsigned bits)
{
    cJSON *list = cJSON_AddArrayToObject(obj, key);
    for (int i = 0; list != NULL && i <= TAG_FIELDS; i++) {
        if ((bits & (1u << i)) == 0)
            continue;
        cJSON *name = cJSON_CreateString(i < TAG_FIELDS ? tags_name[i] : MUSIC_COVER_FIELD);
        if (name == NULL || !cJSON_AddItemToArray(list, name))
            return NULL;
    }
    return list;
}

/* Adds key: the values as JSON (a string, or for genre and composer a
 * list; null when absent). NULL when out of memory. */
static cJSON *add_values(cJSON *obj, const char *key, enum tag_field f,
                         const struct tag_values *v)
{
    if (tags_is_multi(f)) {
        cJSON *list = cJSON_AddArrayToObject(obj, key);
        for (size_t i = 0; list != NULL && i < v->n; i++) {
            cJSON *s = cJSON_CreateString(v->v[i]);
            if (s == NULL || !cJSON_AddItemToArray(list, s))
                return NULL;
        }
        return list;
    }
    return v->n > 0 ? cJSON_AddStringToObject(obj, key, v->v[0])
                    : cJSON_AddNullToObject(obj, key);
}

/* Adds key: a text column as JSON: genre and composer (a JSON array in the
 * column) as a list, the rest as a string; null when NULL. NULL when out
 * of memory or the array is invalid. */
static cJSON *add_column(cJSON *obj, const char *key, enum tag_field f, sqlite3_stmt *st,
                         int col)
{
    const char *s = (const char *)sqlite3_column_text(st, col);
    if (s == NULL)
        return cJSON_AddNullToObject(obj, key);
    if (!tags_is_multi(f))
        return cJSON_AddStringToObject(obj, key, s);
    cJSON *list = cJSON_Parse(s);
    return cJSON_IsArray(list) && cJSON_AddItemToObject(obj, key, list) ? list : NULL;
}

/* Reads a positive id from query parameter name; replies 400 if invalid. */
static int query_id(const struct request *req, struct response *res, const char *name,
                    long long *out)
{
    const char *s = NULL;
    if (http_query(req, name, &s) == 0 && s[0] >= '1' && s[0] <= '9') {
        char *end;
        errno = 0;
        long long v = strtoll(s, &end, 10);
        if (errno == 0 && *end == '\0' && v <= ID_MAX) {
            *out = v;
            return 0;
        }
    }
    json_error(res, 400, message("query parameter '%s' must be a positive whole number%s",
                                 name, ""));
    return -1;
}

/* Reads body[key] as a track id. NULL or an error message. */
static const char *get_id(const cJSON *body, const char *key, long long *out)
{
    long v = 0;
    const char *err = json_get_int(body, key, 1, (long)ID_MAX, &v);
    *out = v;
    return err;
}

/* 1 if track id exists, 0 if not (replied 404), -1 on error (replied 500). */
static int track_exists(struct response *res, long long id)
{
    long long n = single_number("SELECT count(*) FROM tracks WHERE id = ?", id);
    if (n < 0)
        json_error(res, 500, "internal error");
    else if (n == 0)
        json_error(res, 404, "track not found");
    return n < 0 ? -1 : n > 0;
}

/* ---- planned tags ------------------------------------------------------- */

/*
 * The track's tags from st's row (MUSIC_TAG_COLUMNS at col) with its
 * pending changes applied (pending: "SELECT field, value FROM changes
 * WHERE state = 'pending' AND track_id = ?"; a new cover is its only
 * picture), into the arena. *changed: the bits of the fields with a
 * pending change (MUSIC_COVER_BIT for the cover). 0 or -1 (logged).
 */
static int planned_tags(sqlite3_stmt *st, int col, long long id, sqlite3_stmt *pending,
                        struct tags *out, unsigned *changed)
{
    *changed = 0;
    if (music_track_tags(st, col, out) != 0 || sqlite3_bind_int64(pending, 1, id) != SQLITE_OK) {
        fprintf(stderr, "music: track %lld: can not read its tags\n", id);
        return -1;
    }
    int rc, bad = 0;
    while ((rc = sqlite3_step(pending)) == SQLITE_ROW && !bad) {
        const char *name = (const char *)sqlite3_column_text(pending, 0);
        int f = tags_field_of(name);
        const char *v = (const char *)sqlite3_column_text(pending, 1);
        const char *copy = v != NULL ? arena_strndup(v, (size_t)sqlite3_column_bytes(pending, 1))
                                     : NULL;
        if (strcmp(name, MUSIC_COVER_FIELD) == 0) {
            bad = copy == NULL || music_set_cover(out, copy) != 0;
            *changed |= bad ? 0 : MUSIC_COVER_BIT;
            continue;
        }
        bad = f < 0 || copy == NULL ||
              (tags_is_multi((enum tag_field)f)
                   ? music_values_parse(copy, &out->value[f]) != 0
                   : tags_set_one(out, (enum tag_field)f, copy) != 0);
        if (!bad)
            *changed |= 1u << f;
    }
    if (rc != SQLITE_DONE && rc != SQLITE_ROW)
        db_log_error(music_db, "pending changes");
    sqlite3_reset(pending);
    if (bad)
        fprintf(stderr, "music: track %lld: a pending change can not be read\n", id);
    return bad || (rc != SQLITE_DONE && rc != SQLITE_ROW) ? -1 : 0;
}

#define PENDING_SQL "SELECT field, value FROM changes WHERE state = 'pending' AND track_id = ?"

/*
 * What is wrong with planned tags, among the fields the web app edits:
 * *missing the required ones without a value, *invalid those whose value
 * breaks a rule (tags_check()).
 */
static void problems(const struct tags *t, unsigned *missing, unsigned *invalid)
{
    *missing = *invalid = 0;
    for (int i = 0; i < TAG_FIELDS; i++) {
        const struct tag_values *v = &t->value[i];
        if (!tags_is_editable((enum tag_field)i))
            continue; /* the sort tags are mirrored and cut when written */
        int empty = v->n == 0 || (!tags_is_multi((enum tag_field)i) && v->v[0][0] == '\0');
        if (empty)
            *missing |= tags_is_required((enum tag_field)i) ? 1u << i : 0;
        else if (tags_check((enum tag_field)i, v) != NULL)
            *invalid |= 1u << i;
    }
}

/* A 64-bit FNV-1a hash of a tag's values, to compare them between tracks. */
static uint64_t hash_values(const struct tag_values *v)
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < v->n; i++) {
        for (const unsigned char *p = (const unsigned char *)v->v[i]; *p != '\0'; p++)
            h = (h ^ *p) * 1099511628211ULL;
        h = (h ^ 0x1f) * 1099511628211ULL; /* a separator no value has */
    }
    return (h ^ v->n) * 1099511628211ULL;
}

/* The same for a track's pictures (their hashes, in order). */
static uint64_t hash_pictures(const struct tags *t)
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < t->npictures; i++)
        for (const unsigned char *p = (const unsigned char *)t->pictures[i].hash; *p != '\0'; p++)
            h = (h ^ *p) * 1099511628211ULL;
    return (h ^ t->npictures) * 1099511628211ULL;
}

/* ---- reads -------------------------------------------------------------- */

/* Adds the latest scan of the whole library (null if none) with the files
 * it found that are not music, by extension. 0 or -1 (logged). */
static int add_last_scan(cJSON *obj)
{
    sqlite3_stmt *st = db_prepare(music_db,
        "SELECT id, requested, started, finished, state, files, parsed, removed, failed"
        " FROM scans WHERE path IS NULL AND state <> 'queued' ORDER BY id DESC LIMIT 1");
    int rc = st != NULL ? sqlite3_step(st) : SQLITE_ERROR;
    cJSON *scan = rc == SQLITE_ROW ? json_row(st, 9) : cJSON_CreateNull();
    long long id = rc == SQLITE_ROW ? sqlite3_column_int64(st, 0) : 0;
    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
        db_log_error(music_db, "last scan");
    sqlite3_finalize(st);
    if ((rc != SQLITE_ROW && rc != SQLITE_DONE) || scan == NULL ||
        !cJSON_AddItemToObject(obj, "scan", scan))
        return -1;
    if (id == 0)
        return 0;
    cJSON *others = cJSON_AddArrayToObject(scan, "others");
    st = others != NULL ? db_prepare(music_db, "SELECT ext, count FROM scan_extensions"
                                               " WHERE scan_id = ? ORDER BY count DESC, ext")
                        : NULL;
    int ok = st != NULL && sqlite3_bind_int64(st, 1, id) == SQLITE_OK &&
             add_rows(others, st, 2, MAX_LIST) == 0;
    sqlite3_finalize(st);
    return ok ? 0 : -1;
}

/*
 * GET /api/music: whether music is set up, the folder, which service runs
 * (busy: "scan", "write", "busy" or null), the scan running now (path null:
 * the whole library) and how many wait, pending changes, the last scan of
 * the whole library, and counts of the library.
 */
void music_overview(struct request *req, struct response *res)
{
    (void)req;
    int error;
    const char *busy = music_busy(&error);
    int configured = music_root() != NULL;
    cJSON *obj = cJSON_CreateObject();
    cJSON *stats = obj != NULL ? cJSON_AddObjectToObject(obj, "stats") : NULL;
    long long tracks = single_number("SELECT count(*) FROM tracks", 0);
    long long size = single_number("SELECT coalesce(sum(size), 0) FROM tracks", 0);
    long long albums =
        single_number("SELECT count(*) FROM (SELECT 1 FROM tracks GROUP BY album, albumartist)", 0);
    long long pending = single_number("SELECT count(*) FROM changes WHERE state = 'pending'", 0);
    long long queued = single_number("SELECT count(*) FROM scans WHERE state = 'queued'", 0);
    if (error || stats == NULL || tracks < 0 || size < 0 || albums < 0 || pending < 0 ||
        queued < 0 || cJSON_AddBoolToObject(obj, "configured", configured) == NULL ||
        cJSON_AddBoolToObject(obj, "available", configured && music_available()) == NULL ||
        (configured ? cJSON_AddStringToObject(obj, "root", music_root())
                    : cJSON_AddNullToObject(obj, "root")) == NULL ||
        (busy != NULL ? cJSON_AddStringToObject(obj, "busy", busy)
                      : cJSON_AddNullToObject(obj, "busy")) == NULL ||
        cJSON_AddNumberToObject(obj, "pending", (double)pending) == NULL ||
        cJSON_AddNumberToObject(obj, "queued_scans", (double)queued) == NULL ||
        cJSON_AddNumberToObject(stats, "tracks", (double)tracks) == NULL ||
        cJSON_AddNumberToObject(stats, "albums", (double)albums) == NULL ||
        cJSON_AddNumberToObject(stats, "size", (double)size) == NULL ||
        add_last_scan(obj) != 0)
        goto fail;

    cJSON *exts = cJSON_AddArrayToObject(stats, "extensions");
    sqlite3_stmt *st = exts != NULL ? db_prepare(music_db,
        "SELECT ext, count(*) AS tracks, sum(size) AS size FROM tracks GROUP BY ext"
        " ORDER BY count(*) DESC, ext") : NULL;
    int ok = st != NULL && add_rows(exts, st, 3, MAX_LIST) == 0;
    sqlite3_finalize(st);
    st = ok ? db_prepare(music_db, "SELECT path, files, parsed FROM scans"
                                   " WHERE state = 'running' ORDER BY id LIMIT 1") : NULL;
    int rc = st != NULL ? sqlite3_step(st) : SQLITE_ERROR;
    cJSON *running = rc == SQLITE_ROW ? json_row(st, 3) : cJSON_CreateNull();
    sqlite3_finalize(st);
    if (!ok || (rc != SQLITE_ROW && rc != SQLITE_DONE) || running == NULL ||
        !cJSON_AddItemToObject(obj, "running", running))
        goto fail;
    json_reply(res, 200, obj);
    return;
fail:
    fprintf(stderr, "music: overview failed\n");
    json_error(res, 500, "internal error");
}

/* A planned single-valued column: the pending change ("" removes it), else
 * the file's value. */
#define PLANNED(c)                                                                     \
    "(SELECT CASE WHEN count(*) = 0 THEN t." c " ELSE nullif(max(c.value), '') END"    \
    " FROM changes c WHERE c.state = 'pending' AND c.track_id = t.id AND c.field = '" c "')"
/* The same for genre or composer, as a JSON array. */
#define PLANNED_LIST(f)                                                                \
    "(SELECT CASE WHEN count(*) = 0 THEN (SELECT json_group_array(value ORDER BY position)" \
    " FROM track_values v WHERE v.track_id = t.id AND v.field = '" f "')"              \
    " ELSE max(c.value) END"                                                           \
    " FROM changes c WHERE c.state = 'pending' AND c.track_id = t.id AND c.field = '" f "')"

/*
 * Every track (or those of the album of track ?1, if ?2 is 1), by album:
 * id, the album key (the files' album and album artist), the planned
 * album fields (album_fields order), then MUSIC_TAG_COLUMNS.
 */
#define ALBUMS_SQL                                                                     \
    "SELECT t.id, t.album, t.albumartist, " PLANNED("album") ", " PLANNED("albumartist") \
    ", " PLANNED("date") ", " PLANNED("compilation") ", " PLANNED_LIST("genre") ", "   \
    PLANNED_LIST("composer") ", " MUSIC_TAG_COLUMNS                                    \
    " FROM tracks t WHERE ?2 = 0 OR (" SAME_ALBUM ")"                                  \
    " ORDER BY t.albumartist COLLATE NOCASE, t.album COLLATE NOCASE, t.albumartist,"   \
    " t.album, t.id"
#define ALBUMS_TAGS_COL (3 + (int)NALBUM_FIELDS)

/* What an album row says about its tracks, gathered track by track. */
struct album_sum {
    cJSON *row;
    int tracks;
    uint64_t hash[NALBUM_FIELDS], artist, pictures;
    unsigned mixed, changed, missing, invalid;
    int several_artists, all_compilation, no_art, mixed_art;
    char cover[ART_HASH_LEN + 1]; /* "" if no track has a picture */
    int cover_front;              /* cover is a "Front Cover" */
};

/* Notes the album's cover from a track's pictures: the first front cover
 * of the album's tracks, else the first picture. */
static void find_cover(struct album_sum *a, const struct tags *t)
{
    for (size_t i = 0; i < t->npictures && !a->cover_front; i++) {
        int front = strcmp(t->pictures[i].type, "Front Cover") == 0;
        if (front || a->cover[0] == '\0') {
            memcpy(a->cover, t->pictures[i].hash, sizeof a->cover);
            a->cover_front = front;
        }
    }
}

/* Starts an album row from its first track: id, the planned values. NULL
 * when out of memory. */
static cJSON *album_row(sqlite3_stmt *st)
{
    cJSON *row = cJSON_CreateObject();
    if (row == NULL || cJSON_AddNumberToObject(row, "track",
                                               (double)sqlite3_column_int64(st, 0)) == NULL)
        return NULL;
    for (size_t i = 0; i < NALBUM_FIELDS; i++)
        if (add_column(row, tags_name[album_fields[i]], album_fields[i], st, 3 + (int)i) == NULL)
            return NULL;
    return row;
}

/* Ends an album row: what was gathered. 0, or -1 when out of memory. */
static int album_end(const struct album_sum *a)
{
    cJSON *row = a->row;
    /* A mixed value is not the album's: the row shows none. */
    for (size_t i = 0; i < NALBUM_FIELDS; i++) {
        const char *name = tags_name[album_fields[i]];
        if ((a->mixed & (1u << album_fields[i])) &&
            !cJSON_ReplaceItemInObjectCaseSensitive(row, name, cJSON_CreateNull()))
            return -1;
    }
    return cJSON_AddNumberToObject(row, "tracks", a->tracks) != NULL &&
                   add_field_names(row, "mixed", a->mixed) != NULL &&
                   add_field_names(row, "changed", a->changed) != NULL &&
                   add_field_names(row, "missing", a->missing) != NULL &&
                   add_field_names(row, "invalid", a->invalid) != NULL &&
                   cJSON_AddBoolToObject(row, "several_artists",
                                         a->several_artists && !a->all_compilation) != NULL &&
                   cJSON_AddBoolToObject(row, "no_art", a->no_art) != NULL &&
                   (a->cover[0] != '\0' ? cJSON_AddStringToObject(row, "cover", a->cover)
                                        : cJSON_AddNullToObject(row, "cover")) != NULL &&
                   cJSON_AddBoolToObject(row, "mixed_art", a->mixed_art) != NULL
               ? 0
               : -1;
}

/* 1 if the row's album key (columns 1 and 2) differs from the last one. */
static int new_album(sqlite3_stmt *st, const char **album, const char **artist, int first)
{
    const char *a = (const char *)sqlite3_column_text(st, 1);
    const char *b = (const char *)sqlite3_column_text(st, 2);
    int same = !first && (a == NULL ? *album == NULL : *album != NULL && strcmp(a, *album) == 0) &&
               (b == NULL ? *artist == NULL : *artist != NULL && strcmp(b, *artist) == 0);
    return !same;
}

/*
 * GET /api/music/albums[?track=N]: every album (or the album of track N),
 * one row each: track (one of its tracks), the planned album fields
 * (null if absent or mixed), tracks, mixed and changed (fields;
 * "picture" for a new cover), missing and invalid (the required tags a
 * track has no value for, the tags a track has an invalid value in),
 * whether the tracks have several artists without being a compilation,
 * and whether a track has no picture; cover (the hash of the planned
 * front cover, else of the first picture; null if none) and whether the
 * tracks' pictures differ (mixed_art).
 */
void music_albums(struct request *req, struct response *res)
{
    long long one = 0;
    const char *s;
    int found = http_query(req, "track", &s);
    if (found < 0 || (found == 0 && query_id(req, res, "track", &one) != 0)) {
        if (found < 0)
            json_error(res, 400, "query parameter 'track' must be a positive whole number");
        return;
    }
    if (one != 0 && track_exists(res, one) != 1)
        return;
    cJSON *list = cJSON_CreateArray();
    sqlite3_stmt *st = list != NULL ? db_prepare(music_db, ALBUMS_SQL) : NULL;
    sqlite3_stmt *pending = st != NULL ? db_prepare(music_db, PENDING_SQL) : NULL;
    int rc = pending != NULL && sqlite3_bind_int64(st, 1, one) == SQLITE_OK &&
                     sqlite3_bind_int(st, 2, one != 0) == SQLITE_OK
                 ? SQLITE_ROW
                 : SQLITE_ERROR;

    struct album_sum a;
    memset(&a, 0, sizeof a);
    const char *key_album = NULL, *key_artist = NULL;
    while (rc == SQLITE_ROW && (rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (new_album(st, &key_album, &key_artist, a.row == NULL)) {
            if (a.row != NULL && album_end(&a) != 0)
                break;
            memset(&a, 0, sizeof a);
            a.all_compilation = 1;
            a.row = album_row(st);
            const char *k1 = (const char *)sqlite3_column_text(st, 1);
            const char *k2 = (const char *)sqlite3_column_text(st, 2);
            key_album = k1 != NULL ? arena_strndup(k1, strlen(k1)) : NULL;
            key_artist = k2 != NULL ? arena_strndup(k2, strlen(k2)) : NULL;
            if (a.row == NULL || !cJSON_AddItemToArray(list, a.row) ||
                (k1 != NULL && key_album == NULL) || (k2 != NULL && key_artist == NULL))
                break;
        }
        /* The track itself is scratch: only what it adds to the row stays. */
        size_t mark = arena_mark();
        struct tags t;
        unsigned changed, missing, invalid;
        if (planned_tags(st, ALBUMS_TAGS_COL, sqlite3_column_int64(st, 0), pending, &t,
                         &changed) != 0)
            break;
        problems(&t, &missing, &invalid);
        for (size_t i = 0; i < NALBUM_FIELDS; i++) {
            uint64_t h = hash_values(&t.value[album_fields[i]]);
            if (a.tracks == 0)
                a.hash[i] = h;
            else if (h != a.hash[i])
                a.mixed |= 1u << album_fields[i];
        }
        uint64_t artist = hash_values(&t.value[TAG_ARTIST]);
        a.several_artists |= a.tracks > 0 && artist != a.artist;
        a.artist = a.tracks == 0 ? artist : a.artist;
        const struct tag_values *c = &t.value[TAG_COMPILATION];
        a.all_compilation &= c->n == 1 && strcmp(c->v[0], "1") == 0;
        a.no_art |= t.npictures == 0;
        uint64_t pictures = hash_pictures(&t);
        a.mixed_art |= a.tracks > 0 && pictures != a.pictures;
        a.pictures = a.tracks == 0 ? pictures : a.pictures;
        find_cover(&a, &t);
        a.changed |= changed;
        a.missing |= missing;
        a.invalid |= invalid;
        a.tracks++;
        arena_rewind(mark);
    }
    if (rc == SQLITE_DONE && a.row != NULL && album_end(&a) != 0)
        rc = SQLITE_NOMEM;
    if (rc != SQLITE_DONE)
        fprintf(stderr, "music: albums list failed: %s\n", sqlite3_errmsg(music_db));
    sqlite3_finalize(st);
    sqlite3_finalize(pending);
    if (rc != SQLITE_DONE) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, list);
}

/* A track's pictures in a query on "tracks t", as a JSON array: hash, type,
 * description, and what the art table knows (mime, size, width, height,
 * thumb). */
#define PICTURES_SQL                                                                   \
    "(SELECT json_group_array(json_object('hash', p.hash, 'type', p.type,"             \
    "  'description', p.description, 'mime', a.mime, 'size', a.size, 'width', a.width," \
    "  'height', a.height, 'thumb', a.thumb) ORDER BY p.position)"                     \
    " FROM track_pictures p JOIN art a ON a.hash = p.hash WHERE p.track_id = t.id)"

/*
 * GET /api/music/album?track=N: the tracks of the album of track N: their
 * file (path, size, ext, scanned), tags as in the files, pending changes
 * ({field: new value}; picture: the hash of a new cover), pictures
 * (PICTURES_SQL), and which planned tags are missing or invalid.
 */
void music_album(struct request *req, struct response *res)
{
    long long id;
    if (query_id(req, res, "track", &id) != 0 || track_exists(res, id) != 1)
        return;
    long long n = single_number("SELECT count(*) FROM tracks t WHERE" SAME_ALBUM, id);
    if (n < 0) {
        json_error(res, 500, "internal error");
        return;
    }
    if (n > MAX_TRACKS) {
        json_error(res, 413, "this album has more than 2000 tracks: too many for one page");
        return;
    }
    cJSON *list = cJSON_CreateArray();
    sqlite3_stmt *st = list != NULL ? db_prepare(music_db,
        "SELECT t.id, t.path, t.size, t.ext, t.scanned, " PICTURES_SQL ", " MUSIC_TAG_COLUMNS
        " FROM tracks t WHERE" SAME_ALBUM " ORDER BY t.path") : NULL;
    sqlite3_stmt *pending = st != NULL ? db_prepare(music_db, PENDING_SQL) : NULL;
    int rc = pending != NULL && sqlite3_bind_int64(st, 1, id) == SQLITE_OK ? SQLITE_ROW
                                                                            : SQLITE_ERROR;
    const int pictures_col = 5, tags_col = 6;
    while (rc == SQLITE_ROW && (rc = sqlite3_step(st)) == SQLITE_ROW) {
        long long tid = sqlite3_column_int64(st, 0);
        size_t mark = arena_mark();
        struct tags t;
        unsigned changed, missing, invalid;
        if (planned_tags(st, tags_col, tid, pending, &t, &changed) != 0) {
            rc = SQLITE_ERROR;
            break;
        }
        problems(&t, &missing, &invalid);
        arena_rewind(mark);

        /* The row as JSON: everything in it stays in the arena. */
        cJSON *row = json_row(st, pictures_col);
        cJSON *tags = row != NULL ? cJSON_AddObjectToObject(row, "tags") : NULL;
        cJSON *planned = row != NULL ? cJSON_AddObjectToObject(row, "pending") : NULL;
        cJSON *pictures = cJSON_Parse((const char *)sqlite3_column_text(st, pictures_col));
        int ok = planned != NULL && cJSON_IsArray(pictures) && cJSON_AddItemToArray(list, row) &&
                 cJSON_AddItemToObject(row, "pictures", pictures) &&
                 add_field_names(row, "missing", missing) != NULL &&
                 add_field_names(row, "invalid", invalid) != NULL &&
                 planned_tags(st, tags_col, tid, pending, &t, &changed) == 0;
        for (int f = 0; ok && f < TAG_FIELDS; f++)
            ok = add_column(tags, tags_name[f], (enum tag_field)f, st, tags_col + 1 + f) != NULL &&
                 (!(changed & (1u << f)) ||
                  add_values(planned, tags_name[f], (enum tag_field)f, &t.value[f]) != NULL);
        if (ok && (changed & MUSIC_COVER_BIT))
            ok = cJSON_AddStringToObject(planned, MUSIC_COVER_FIELD, t.pictures[0].hash) != NULL;
        if (!ok) {
            rc = SQLITE_NOMEM;
            break;
        }
    }
    if (rc != SQLITE_DONE)
        fprintf(stderr, "music: album failed: %s\n", sqlite3_errmsg(music_db));
    sqlite3_finalize(st);
    sqlite3_finalize(pending);
    if (rc != SQLITE_DONE) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, list);
}

/*
 * The values of a column (or genre / composer) used anywhere: in the files
 * (cache) or in pending changes. At most 5000, sorted.
 */
#define VALUES_SQL(c)                                                          \
    "SELECT " c " FROM tracks WHERE " c " IS NOT NULL AND " c " <> ''"         \
    " UNION SELECT value FROM changes"                                         \
    " WHERE state = 'pending' AND field = '" c "' AND value <> ''"             \
    " ORDER BY 1 COLLATE NOCASE LIMIT 5000"
#define LIST_VALUES_SQL(f)                                                     \
    "SELECT value FROM track_values WHERE field = '" f "'"                     \
    " UNION SELECT j.value FROM changes c, json_each(c.value) j"               \
    " WHERE c.state = 'pending' AND c.field = '" f "'"                         \
    " ORDER BY 1 COLLATE NOCASE LIMIT 5000"

/* GET /api/music/values?field=artist|albumartist|genre|composer: the values
 * in use, for choosing from. */
void music_values(struct request *req, struct response *res)
{
    static const struct {
        const char *field;
        const char *sql;
    } lists[] = {
        { "artist", VALUES_SQL("artist") },
        { "albumartist", VALUES_SQL("albumartist") },
        { "genre", LIST_VALUES_SQL("genre") },
        { "composer", LIST_VALUES_SQL("composer") },
    };
    const char *field = NULL;
    const char *sql = NULL;
    if (http_query(req, "field", &field) == 0)
        for (size_t i = 0; i < sizeof lists / sizeof lists[0]; i++)
            if (strcmp(field, lists[i].field) == 0)
                sql = lists[i].sql;
    if (sql == NULL) {
        json_error(res, 400,
                   "query parameter 'field' must be artist, albumartist, genre or composer");
        return;
    }
    cJSON *list = cJSON_CreateArray();
    sqlite3_stmt *st = list != NULL ? db_prepare(music_db, sql) : NULL;
    if (st == NULL) {
        json_error(res, 500, "internal error");
        return;
    }
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        cJSON *v = cJSON_CreateString((const char *)sqlite3_column_text(st, 0));
        if (v == NULL || !cJSON_AddItemToArray(list, v)) {
            rc = SQLITE_NOMEM;
            break;
        }
    }
    if (rc != SQLITE_DONE)
        db_log_error(music_db, "values");
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, list);
}

/* An album in a count: its album and album artist (either may be NULL). */
#define ALBUM_KEY(t) "json_array(" t ".album, " t ".albumartist)"

/*
 * GET /api/music/charts: from the files' tags, how many albums have each
 * genre (most first) and each date (in order). An album whose tracks have
 * several genres or dates counts once for each.
 */
void music_charts(struct request *req, struct response *res)
{
    (void)req;
    cJSON *obj = cJSON_CreateObject();
    cJSON *genres = obj != NULL ? cJSON_AddArrayToObject(obj, "genres") : NULL;
    cJSON *dates = genres != NULL ? cJSON_AddArrayToObject(obj, "dates") : NULL;
    sqlite3_stmt *st = dates != NULL ? db_prepare(music_db,
        "SELECT v.value AS genre, count(DISTINCT " ALBUM_KEY("t") ") AS albums"
        " FROM track_values v JOIN tracks t ON t.id = v.track_id WHERE v.field = 'genre'"
        " GROUP BY v.value ORDER BY albums DESC, v.value") : NULL;
    int ok = st != NULL && add_rows(genres, st, 2, MAX_LIST) == 0;
    sqlite3_finalize(st);
    st = ok ? db_prepare(music_db,
        "SELECT t.date, count(DISTINCT " ALBUM_KEY("t") ") AS albums"
        " FROM tracks t WHERE t.date IS NOT NULL GROUP BY t.date ORDER BY t.date") : NULL;
    ok = st != NULL && add_rows(dates, st, 2, MAX_LIST) == 0;
    sqlite3_finalize(st);
    if (!ok) {
        fprintf(stderr, "music: charts failed\n");
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, obj);
}

/* A change's tag as the file has it now (genre and composer: JSON; the
 * cover: the hashes of the track's pictures, JSON). */
#define NOW_VALUE                                                                      \
    "CASE c.field WHEN 'title' THEN t.title WHEN 'album' THEN t.album"                 \
    " WHEN 'artist' THEN t.artist WHEN 'albumartist' THEN t.albumartist"               \
    " WHEN 'tracknumber' THEN t.tracknumber WHEN 'discnumber' THEN t.discnumber"       \
    " WHEN 'date' THEN t.date WHEN 'compilation' THEN t.compilation"                   \
    " WHEN 'isrc' THEN t.isrc WHEN 'asin' THEN t.asin WHEN 'bpm' THEN t.bpm"           \
    " WHEN 'copyright' THEN t.copyright WHEN 'encodedby' THEN t.encodedby"             \
    " WHEN 'mood' THEN t.mood WHEN 'media' THEN t.media WHEN 'label' THEN t.label"     \
    " WHEN 'catalognumber' THEN t.catalognumber WHEN 'barcode' THEN t.barcode"         \
    " WHEN 'musicbrainz_trackid' THEN t.musicbrainz_trackid"                           \
    " WHEN 'musicbrainz_albumid' THEN t.musicbrainz_albumid"                           \
    " WHEN 'picture' THEN (SELECT json_group_array(hash ORDER BY position)"             \
    "  FROM track_pictures p WHERE p.track_id = t.id)"                                 \
    " ELSE (SELECT json_group_array(value ORDER BY position) FROM track_values v"       \
    "  WHERE v.track_id = t.id AND v.field = c.field) END"

/*
 * GET /api/music/changes: the pending changes (at most 5000, by album (the
 * files' album and album artist), track and change; now is the file's
 * value), how many there are, and the latest written ones
 * (done, warning, failed) with their notes. track and path are null when
 * a scan removed the track. Genre and composer values are JSON arrays.
 */
void music_changes(struct request *req, struct response *res)
{
    (void)req;
    cJSON *obj = cJSON_CreateObject();
    cJSON *pending = obj != NULL ? cJSON_AddArrayToObject(obj, "pending") : NULL;
    cJSON *history = obj != NULL ? cJSON_AddArrayToObject(obj, "history") : NULL;
    long long count = single_number("SELECT count(*) FROM changes WHERE state = 'pending'", 0);
    sqlite3_stmt *a = pending == NULL ? NULL : db_prepare(music_db,
        "SELECT c.id, c.track_id AS track, t.path, t.album, t.albumartist, t.title, c.field,"
        " c.value, " NOW_VALUE " AS now FROM changes c LEFT JOIN tracks t ON t.id = c.track_id"
        " WHERE c.state = 'pending' ORDER BY t.id IS NULL, t.albumartist COLLATE NOCASE,"
        " t.album COLLATE NOCASE, t.albumartist, t.album, t.path, c.id");
    sqlite3_stmt *b = history == NULL ? NULL : db_prepare(music_db,
        "SELECT c.id, c.batch, c.track_id AS track, t.path, c.field, c.value, c.started,"
        " c.finished, c.state, c.note FROM changes c LEFT JOIN tracks t ON t.id = c.track_id"
        " WHERE c.done = 1 ORDER BY c.finished DESC, c.id DESC LIMIT 300");
    int ok = a != NULL && b != NULL && count >= 0 &&
             cJSON_AddNumberToObject(obj, "count", (double)count) != NULL &&
             add_rows(pending, a, 9, MAX_LIST) == 0 && add_rows(history, b, 10, MAX_HISTORY) == 0;
    sqlite3_finalize(a);
    sqlite3_finalize(b);
    if (!ok) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, obj);
}

/* The picture type mime as a constant string, if nylm shows it; else NULL. */
static const char *shown_mime(const char *mime)
{
    static const char *const types[] = { "image/jpeg", "image/png", "image/gif", "image/webp" };
    for (size_t i = 0; mime != NULL && i < sizeof types / sizeof types[0]; i++)
        if (strcmp(mime, types[i]) == 0)
            return types[i];
    return NULL;
}

/*
 * GET /api/music/art?hash=H&size=full|thumb: a stored picture: full as it
 * is in the file, thumb its JPEG thumbnail (the picture itself if it has
 * none). The one answer of the API that is not JSON: the bytes, with the
 * type the art table has (found from the bytes, never what a file claims),
 * cached for a year (a hash always names the same bytes). 404 if it is not
 * stored or not a type a browser shows.
 */
void music_art(struct request *req, struct response *res)
{
    const char *hash = NULL, *size = NULL;
    if (http_query(req, "hash", &hash) != 0 || !art_hash_valid(hash)) {
        json_error(res, 400, "query parameter 'hash' must be 64 lowercase hex characters");
        return;
    }
    if (http_query(req, "size", &size) != 0 ||
        (strcmp(size, "full") != 0 && strcmp(size, "thumb") != 0)) {
        json_error(res, 400, "query parameter 'size' must be full or thumb");
        return;
    }
    sqlite3_stmt *st = db_prepare(music_db, "SELECT mime, thumb FROM art WHERE hash = ?");
    int rc = st != NULL && sqlite3_bind_text(st, 1, hash, -1, SQLITE_STATIC) == SQLITE_OK
                 ? sqlite3_step(st)
                 : SQLITE_ERROR;
    const char *mime = rc == SQLITE_ROW ? shown_mime((const char *)sqlite3_column_text(st, 0))
                                        : NULL;
    int thumb = rc == SQLITE_ROW && strcmp(size, "thumb") == 0 && sqlite3_column_int(st, 1);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
        db_log_error(music_db, "art");
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        json_error(res, 500, "internal error");
        return;
    }
    if (mime == NULL) {
        json_error(res, 404, rc == SQLITE_ROW ? "this picture is not a type nylm shows"
                                              : "picture not found");
        return;
    }
    int fd = art_open(ART_MUSIC, hash, thumb);
    struct stat sb;
    if (fd < 0 || fstat(fd, &sb) != 0) {
        int missing = fd < 0 && errno == ENOENT;
        if (fd >= 0)
            close(fd);
        if (!missing)
            fprintf(stderr, "music: art %s: %s\n", hash, strerror(errno));
        json_error(res, missing ? 404 : 500, missing ? "picture not found" : "internal error");
        return;
    }
    res->status = 200;
    res->content_type = thumb ? "image/jpeg" : mime;
    res->cache_control = "private, max-age=31536000, immutable";
    res->file_fd = fd;
    res->file_size = (size_t)sb.st_size;
}

/* ---- queueing changes --------------------------------------------------- */

/* One tag of one track to change. */
struct edit {
    long long track;
    enum tag_field field;
    struct tag_values value;
    const char *stored; /* the value as the changes table keeps it */
};

/*
 * Reads a new value for field f from item: genre and composer a list of
 * strings, the others a string ("" removes the tag). It must pass
 * tags_check(). NULL, or an error message.
 */
static const char *read_value(const cJSON *item, enum tag_field f, struct edit *e)
{
    e->field = f;
    e->value.n = 0;
    e->value.v = NULL;
    if (tags_is_multi(f)) {
        if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) > TAGS_MAX_VALUES)
            return message("'%s' must be a list of at most 64 strings%s", tags_name[f], "");
        e->value.v = arena_alloc((size_t)(cJSON_GetArraySize(item) + 1) * sizeof *e->value.v);
        if (e->value.v == NULL)
            return "out of memory";
        const cJSON *s;
        cJSON_ArrayForEach(s, item) {
            if (!cJSON_IsString(s))
                return message("'%s' must be a list of at most 64 strings%s", tags_name[f], "");
            e->value.v[e->value.n++] = s->valuestring;
        }
        e->stored = music_values_json(&e->value);
    } else {
        if (!cJSON_IsString(item))
            return message("'%s' must be a string%s", tags_name[f], "");
        e->stored = item->valuestring;
        if (item->valuestring[0] != '\0') {
            e->value.v = arena_alloc(sizeof *e->value.v);
            if (e->value.v == NULL)
                return "out of memory";
            e->value.v[0] = item->valuestring;
            e->value.n = 1;
        }
    }
    if (e->stored == NULL)
        return "out of memory";
    const char *why = tags_check(f, &e->value);
    return why != NULL ? message("'%s' %s", tags_name[f], why) : NULL;
}

/* The editable field named by s, or -1. */
static int editable_field(const char *s)
{
    int f = tags_field_of(s);
    return f >= 0 && tags_is_editable((enum tag_field)f) ? f : -1;
}

/*
 * {"album": track id, "set": {field: value, ...}}: the fields for every
 * track of that album. Fills edits (MAX_EDITS). Their count, or -1 after
 * replying.
 */
static int read_album_edits(const cJSON *body, struct response *res, struct edit *edits)
{
    long long album;
    const char *err = get_id(body, "album", &album);
    const cJSON *set = cJSON_GetObjectItemCaseSensitive(body, "set");
    if (err == NULL && (!cJSON_IsObject(set) || set->child == NULL))
        err = "'set' must be an object of at least one field";
    struct edit fields[TAG_FIELDS];
    int nfields = 0;
    for (const cJSON *item = set != NULL ? set->child : NULL; err == NULL && item != NULL;
         item = item->next) {
        int f = editable_field(item->string);
        if (f < 0)
            err = message("'%.100s' is not a tag that can be changed%s", item->string, "");
        for (int i = 0; err == NULL && i < nfields; i++)
            if (fields[i].field == (enum tag_field)f)
                err = message("'%s' is set twice%s", tags_name[f], "");
        if (err == NULL)
            err = read_value(item, (enum tag_field)f, &fields[nfields++]);
    }
    if (err != NULL) {
        json_error(res, 400, err);
        return -1;
    }
    if (track_exists(res, album) != 1)
        return -1;

    sqlite3_stmt *st = db_prepare(music_db, "SELECT t.id FROM tracks t WHERE" SAME_ALBUM);
    int rc = st != NULL && sqlite3_bind_int64(st, 1, album) == SQLITE_OK ? SQLITE_ROW
                                                                          : SQLITE_ERROR;
    int n = 0;
    while (rc == SQLITE_ROW && (rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (n + nfields > MAX_EDITS) {
            rc = SQLITE_FULL;
            break;
        }
        for (int i = 0; i < nfields; i++) {
            edits[n] = fields[i];
            edits[n++].track = sqlite3_column_int64(st, 0);
        }
    }
    if (rc != SQLITE_DONE && rc != SQLITE_FULL)
        db_log_error(music_db, "album tracks");
    sqlite3_finalize(st);
    if (rc == SQLITE_FULL)
        json_error(res, 413, "too many changes at once (at most 2000 track tags)");
    else if (rc != SQLITE_DONE)
        json_error(res, 500, "internal error");
    return rc == SQLITE_DONE ? n : -1;
}

/*
 * {"edits": [{"track": id, "field": name, "value": value}, ...]}: one tag
 * of one track each, none twice. Fills edits (MAX_EDITS). Their count, or
 * -1 after replying.
 */
static int read_track_edits(const cJSON *body, struct response *res, struct edit *edits)
{
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(body, "edits");
    int size = cJSON_GetArraySize(list);
    if (!cJSON_IsArray(list) || size < 1 || size > MAX_EDITS) {
        json_error(res, 400, "'edits' must be a list of 1 to 2000 changes");
        return -1;
    }
    const char *err = NULL;
    int n = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, list) {
        const char *name = NULL;
        if (!cJSON_IsObject(item)) {
            err = "each edit must be an object";
        } else if ((err = get_id(item, "track", &edits[n].track)) == NULL &&
                   (err = json_get_string(item, "field", 1, 64, &name)) == NULL) {
            int f = editable_field(name);
            err = f < 0 ? message("'%.64s' is not a tag that can be changed%s", name, "")
                        : read_value(cJSON_GetObjectItemCaseSensitive(item, "value"),
                                     (enum tag_field)f, &edits[n]);
        }
        for (int i = 0; err == NULL && i < n; i++)
            if (edits[i].track == edits[n].track && edits[i].field == edits[n].field)
                err = message("'%s' of one track is changed twice%s",
                              tags_name[edits[n].field], "");
        if (err != NULL)
            break;
        n++;
    }
    if (err != NULL) {
        json_error(res, 400, err);
        return -1;
    }
    for (int i = 0; i < n; i++) {
        long long found = single_number("SELECT count(*) FROM tracks WHERE id = ?",
                                        edits[i].track);
        if (found <= 0) {
            json_error(res, found < 0 ? 500 : 404, found < 0 ? "internal error"
                                                             : "a track in 'edits' is not found");
            return -1;
        }
    }
    return n;
}

/*
 * Queues e in batch: or, if the file already has that value, drops its
 * pending change. Adds 1 to *queued or *dropped when it did. 0 or -1
 * (logged).
 */
static int queue_one(const struct edit *e, long long batch, sqlite3_stmt *track, int *queued,
                     int *dropped)
{
    size_t mark = arena_mark();
    struct tags now;
    int rc = sqlite3_bind_int64(track, 1, e->track) == SQLITE_OK &&
                     sqlite3_step(track) == SQLITE_ROW && music_track_tags(track, 0, &now) == 0
                 ? 0
                 : -1;
    int same = rc == 0 && tags_equal(&now.value[e->field], &e->value);
    sqlite3_reset(track);
    arena_rewind(mark);
    if (rc != 0) {
        db_log_error(music_db, "queue: track");
        return -1;
    }
    sqlite3_stmt *st = same
        ? db_prepare(music_db, "DELETE FROM changes"
                               " WHERE state = 'pending' AND track_id = ?2 AND field = ?3")
        : db_prepare(music_db,
              "INSERT INTO changes (batch, track_id, field, value) VALUES (?1, ?2, ?3, ?4)"
              " ON CONFLICT (track_id, field) WHERE state = 'pending' DO UPDATE SET"
              "  batch = excluded.batch, value = excluded.value"
              " WHERE value IS NOT excluded.value");
    rc = st != NULL ? SQLITE_OK : SQLITE_ERROR;
    if (rc == SQLITE_OK && !same)
        rc = sqlite3_bind_int64(st, 1, batch);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(st, 2, e->track);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_text(st, 3, tags_name[e->field], -1, SQLITE_STATIC);
    if (rc == SQLITE_OK && !same)
        rc = sqlite3_bind_text(st, 4, e->stored, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK || run_once(st) != 0) {
        if (rc != SQLITE_OK)
            sqlite3_finalize(st);
        return -1;
    }
    *(same ? dropped : queued) += sqlite3_changes(music_db) > 0;
    return 0;
}

/*
 * POST /api/music/queue: queues tag changes, as one batch; nothing is
 * written to the files until the write service runs. Either
 *   {"album": track id, "set": {field: value, ...}}  every track of that album
 *   {"edits": [{"track": id, "field": name, "value": value}, ...]}
 * Values: a string ("" removes the tag), or for genre and composer a list
 * of strings; each must pass the rules (tags_check()). A value the file
 * already has drops the pending change instead.
 * -> 200 {batch, queued, dropped}
 */
void music_queue(struct request *req, struct response *res)
{
    cJSON *body = json_body(req, res);
    if (body == NULL)
        return;
    int by_album = cJSON_GetObjectItemCaseSensitive(body, "album") != NULL;
    if (by_album == (cJSON_GetObjectItemCaseSensitive(body, "edits") != NULL)) {
        json_error(res, 400, "give either 'album' and 'set', or 'edits'");
        return;
    }
    struct edit *edits = arena_alloc(MAX_EDITS * sizeof *edits);
    if (edits == NULL) {
        json_error(res, 500, "internal error");
        return;
    }
    int n = by_album ? read_album_edits(body, res, edits) : read_track_edits(body, res, edits);
    if (n < 0)
        return;

    int lock = lock_for_write(res);
    if (lock < 0)
        return;
    int queued = 0, dropped = 0;
    long long batch = -1;
    sqlite3_stmt *track = db_prepare(music_db,
        "SELECT " MUSIC_TAG_COLUMNS " FROM tracks t WHERE t.id = ?");
    int rc = track != NULL ? db_exec(music_db, "BEGIN IMMEDIATE") : -1;
    if (rc == 0 &&
        (batch = single_number("SELECT coalesce(max(batch), 0) + 1 FROM changes", 0)) < 0)
        rc = -1;
    for (int i = 0; i < n && rc == 0; i++)
        rc = queue_one(&edits[i], batch, track, &queued, &dropped);
    sqlite3_finalize(track);
    if (rc == 0)
        rc = db_exec(music_db, "COMMIT");
    if (rc != 0 && sqlite3_get_autocommit(music_db) == 0)
        db_exec(music_db, "ROLLBACK");
    music_unlock(lock);

    cJSON *out = cJSON_CreateObject();
    if (rc != 0 || out == NULL || cJSON_AddNumberToObject(out, "batch", (double)batch) == NULL ||
        cJSON_AddNumberToObject(out, "queued", queued) == NULL ||
        cJSON_AddNumberToObject(out, "dropped", dropped) == NULL) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, out);
}

/* ---- duplicates --------------------------------------------------------- */

#define MAX_DUPES      500  /* rows in one list of possible duplicates */
#define MAX_MERGE_FROM 64   /* names merged into one at once */
#define MAX_MERGE_NAME 4096 /* bytes in a name merged (files may hold long ones) */

/*
 * The planned values, as "p": every track (id, path) with the tags the
 * duplicates use, pending changes applied (a removed tag is NULL); and as
 * "pv" (track_id, field, value) the planned genres and composers.
 */
#define PLANNED_PC(f) " max(CASE field WHEN '" f "' THEN value END) AS " f
#define PLANNED_P(f)  " nullif(coalesce(pc." f ", t." f "), '') AS " f
#define PLANNED_CTE                                                                         \
    "WITH pc AS (SELECT track_id," PLANNED_PC("title") "," PLANNED_PC("artist") ","         \
    PLANNED_PC("album") "," PLANNED_PC("albumartist") "," PLANNED_PC("isrc") ","            \
    PLANNED_PC("barcode") "," PLANNED_PC("musicbrainz_trackid") ","                         \
    PLANNED_PC("musicbrainz_albumid")                                                       \
    "  FROM changes WHERE state = 'pending' GROUP BY track_id),"                            \
    " p AS (SELECT t.id, t.path," PLANNED_P("title") "," PLANNED_P("artist") ","            \
    PLANNED_P("album") "," PLANNED_P("albumartist") "," PLANNED_P("isrc") ","               \
    PLANNED_P("barcode") "," PLANNED_P("musicbrainz_trackid") ","                           \
    PLANNED_P("musicbrainz_albumid")                                                        \
    "  FROM tracks t LEFT JOIN pc ON pc.track_id = t.id),"                                  \
    " pv AS (SELECT v.track_id, v.field, v.value FROM track_values v WHERE NOT EXISTS"      \
    "  (SELECT 1 FROM changes c WHERE c.state = 'pending' AND c.track_id = v.track_id"      \
    "   AND c.field = v.field)"                                                             \
    "  UNION ALL SELECT c.track_id, c.field, j.value FROM changes c, json_each(c.value) j"  \
    "  WHERE c.state = 'pending' AND c.field IN ('genre', 'composer'))"

/* Tracks that share key (an SQL expression on p; NULL: none): key, id,
 * path, artist, title, album, albumartist. ?1: at most this many rows. */
#define TRACK_DUPES(key)                                                                     \
    PLANNED_CTE ", keyed AS (SELECT " key " AS k, id FROM p),"                               \
    " g AS (SELECT k FROM keyed WHERE k IS NOT NULL GROUP BY k HAVING count(*) > 1)"         \
    " SELECT keyed.k AS key, p.id, p.path, p.artist, p.title, p.album, p.albumartist"        \
    " FROM keyed JOIN g USING (k) JOIN p ON p.id = keyed.id ORDER BY keyed.k, p.path LIMIT ?1"

/* Albums (the tracks of one album and album artist in one folder) that
 * share key, in groups that pass having (on a, with ak: the album artist's
 * key): key, id (a track), album, albumartist, folder, tracks. */
#define ALBUM_DUPES(key, having)                                                             \
    PLANNED_CTE ", u AS (SELECT " key " AS k, coalesce(dupe_key(albumartist), '') AS ak,"    \
    "  album, albumartist, rtrim(rtrim(path, replace(path, '/', '')), '/') AS folder, id"    \
    "  FROM p),"                                                                             \
    " a AS (SELECT k, min(ak) AS ak, album, albumartist, folder, min(id) AS id,"             \
    "  count(*) AS tracks FROM u WHERE k IS NOT NULL GROUP BY k, album, albumartist, folder)," \
    " g AS (SELECT k FROM a GROUP BY k HAVING " having ")"                                   \
    " SELECT a.k AS key, a.id, a.album, a.albumartist, a.folder, a.tracks FROM a JOIN g USING (k)" \
    " ORDER BY a.k, a.album, a.albumartist, a.folder LIMIT ?1"

/* Names (values: value, tracks) that share a key: key, value, tracks. */
#define NAME_DUPES(values)                                                                   \
    PLANNED_CTE ", v AS (" values "),"                                                       \
    " keyed AS (SELECT dupe_key(value) AS k, value, tracks FROM v),"                         \
    " g AS (SELECT k FROM keyed WHERE k IS NOT NULL GROUP BY k HAVING count(*) > 1)"         \
    " SELECT keyed.k AS key, keyed.value, keyed.tracks FROM keyed JOIN g USING (k)"          \
    " ORDER BY keyed.k, keyed.tracks DESC, keyed.value LIMIT ?1"
#define ONE_NAME(c) \
    "SELECT " c " AS value, count(*) AS tracks FROM p WHERE " c " IS NOT NULL GROUP BY " c
#define LIST_NAME(f) \
    "SELECT value, count(DISTINCT track_id) AS tracks FROM pv WHERE field = '" f "' GROUP BY value"

/*
 * Adds name: {"rows": [...], "more": bool} to obj, the first MAX_DUPES rows
 * of sql (ncols columns); more if there were others. 0 or -1 (logged).
 */
static int add_dupes(cJSON *obj, const char *name, const char *sql, int ncols)
{
    cJSON *section = cJSON_AddObjectToObject(obj, name);
    cJSON *rows = section != NULL ? cJSON_AddArrayToObject(section, "rows") : NULL;
    sqlite3_stmt *st = rows != NULL ? db_prepare(music_db, sql) : NULL;
    int rc = st != NULL && sqlite3_bind_int(st, 1, MAX_DUPES + 1) == SQLITE_OK &&
                     add_rows(rows, st, ncols, MAX_DUPES) == 0
                 ? 0
                 : -1;
    int more = 0;
    if (rc == 0 && cJSON_GetArraySize(rows) == MAX_DUPES) {
        int step = sqlite3_step(st);
        more = step == SQLITE_ROW;
        if (step != SQLITE_ROW && step != SQLITE_DONE) {
            db_log_error(music_db, name);
            rc = -1;
        }
    }
    if (rc == 0 && cJSON_AddBoolToObject(section, "more", more) == NULL)
        rc = -1;
    if (rows == NULL)
        fprintf(stderr, "music: out of memory listing duplicates\n");
    sqlite3_finalize(st);
    return rc;
}

/*
 * GET /api/music/duplicates: possible duplicates, by the planned tags.
 * Two names are the same when their keys are (src/dupes.h).
 * -> 200 {"tracks": {"trackid", "isrc", "title"},
 *         "albums": {"albumid", "barcode", "name", "title"},
 *         "names": {"artist", "albumartist", "composer", "genre"}}
 * each {"rows": [...], "more": bool}, the rows of a group together (key).
 * tracks: the same MusicBrainz track id, ISRC, artist and title; albums:
 * the same MusicBrainz album id, barcode, album and album artist, album
 * with album artists that differ.
 */
void music_duplicates(struct request *req, struct response *res)
{
    (void)req;
    static const struct {
        const char *group, *name, *sql;
        int ncols;
    } lists[] = {
        { "tracks", "trackid", TRACK_DUPES("dupe_key(musicbrainz_trackid)"), 7 },
        { "tracks", "isrc", TRACK_DUPES("dupe_key(isrc)"), 7 },
        { "tracks", "title", TRACK_DUPES("dupe_key(artist) || ' ' || dupe_key(title)"), 7 },
        { "albums", "albumid", ALBUM_DUPES("dupe_key(musicbrainz_albumid)", "count(*) > 1"), 6 },
        { "albums", "barcode", ALBUM_DUPES("dupe_key(barcode)", "count(*) > 1"), 6 },
        { "albums", "name",
          ALBUM_DUPES("dupe_key(album) || ' ' || coalesce(dupe_key(albumartist), '')",
                      "count(*) > 1"), 6 },
        { "albums", "title", ALBUM_DUPES("dupe_key(album)", "count(DISTINCT ak) > 1"), 6 },
        { "names", "artist", NAME_DUPES(ONE_NAME("artist")), 3 },
        { "names", "albumartist", NAME_DUPES(ONE_NAME("albumartist")), 3 },
        { "names", "composer", NAME_DUPES(LIST_NAME("composer")), 3 },
        { "names", "genre", NAME_DUPES(LIST_NAME("genre")), 3 },
    };
    int rc = dupes_register(music_db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "music: can not add dupe_key(): %s\n", sqlite3_errstr(rc));
        json_error(res, 500, "internal error");
        return;
    }
    cJSON *obj = cJSON_CreateObject();
    cJSON *group = NULL;
    int ok = obj != NULL;
    for (size_t i = 0; ok && i < sizeof lists / sizeof lists[0]; i++) {
        if (i == 0 || strcmp(lists[i].group, lists[i - 1].group) != 0)
            ok = (group = cJSON_AddObjectToObject(obj, lists[i].group)) != NULL;
        ok = ok && add_dupes(group, lists[i].name, lists[i].sql, lists[i].ncols) == 0;
    }
    if (!ok) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, obj);
}

/* The tracks with one of the values ?1 (a JSON array) in a field, in the
 * file or in a pending change. */
#define MERGE_ONE_SQL(f)                                                                   \
    "SELECT t.id FROM tracks t WHERE t." f " IN (SELECT value FROM json_each(?1))"         \
    " OR EXISTS (SELECT 1 FROM changes c WHERE c.state = 'pending' AND c.track_id = t.id"  \
    "  AND c.field = '" f "' AND c.value IN (SELECT value FROM json_each(?1)))"
#define MERGE_LIST_SQL(f)                                                                  \
    "SELECT t.id FROM tracks t WHERE EXISTS (SELECT 1 FROM track_values v"                 \
    "  WHERE v.track_id = t.id AND v.field = '" f "'"                                      \
    "  AND v.value IN (SELECT value FROM json_each(?1)))"                                  \
    " OR EXISTS (SELECT 1 FROM changes c, json_each(c.value) j WHERE c.state = 'pending'"  \
    "  AND c.track_id = t.id AND c.field = '" f "'"                                        \
    "  AND j.value IN (SELECT value FROM json_each(?1)))"

/* 1 if s is one of from. */
static int merged_name(const struct tag_values *from, const char *s)
{
    for (size_t i = 0; i < from->n; i++)
        if (strcmp(from->v[i], s) == 0)
            return 1;
    return 0;
}

/*
 * The edit that merges from into to in field f of a track's planned tags
 * t: in a list each of from becomes to (once, where the first was). 1 and
 * e filled, 0 if the track has none of from, -1 when out of memory.
 */
static int merge_edit(const struct tags *t, enum tag_field f, const struct tag_values *from,
                      const char *to, struct edit *e)
{
    const struct tag_values *now = &t->value[f];
    e->field = f;
    if (!tags_is_multi(f)) {
        if (now->n != 1 || !merged_name(from, now->v[0]))
            return 0;
        e->value.n = 1;
        e->value.v = arena_alloc(sizeof *e->value.v);
        if (e->value.v == NULL)
            return -1;
        e->value.v[0] = to;
        e->stored = to;
        return 1;
    }
    e->value.n = 0;
    e->value.v = arena_alloc((now->n + 1) * sizeof *e->value.v);
    if (e->value.v == NULL)
        return -1;
    int found = 0;
    for (size_t i = 0; i < now->n; i++) {
        int merged = merged_name(from, now->v[i]);
        const char *s = merged ? to : now->v[i];
        found |= merged;
        int twice = 0;
        for (size_t j = 0; j < e->value.n && !twice; j++)
            twice = strcmp(e->value.v[j], s) == 0;
        if (!twice)
            e->value.v[e->value.n++] = s;
    }
    if (!found)
        return 0;
    e->stored = music_values_json(&e->value);
    return e->stored != NULL ? 1 : -1;
}

/*
 * {"field": f, "from": [names], "to": name}: reads and checks a merge into
 * *f, from and *to. NULL or an error message.
 */
static const char *read_merge(const cJSON *body, enum tag_field *f, struct tag_values *from,
                              const char **to)
{
    static const enum tag_field fields[] = {
        TAG_ARTIST, TAG_ALBUMARTIST, TAG_COMPOSER, TAG_GENRE,
    };
    const char *name = NULL;
    const char *err = json_get_string(body, "field", 1, 32, &name);
    int found = 0;
    for (size_t i = 0; err == NULL && i < sizeof fields / sizeof fields[0]; i++)
        if (strcmp(name, tags_name[fields[i]]) == 0) {
            *f = fields[i];
            found = 1;
        }
    if (err != NULL || !found)
        return "'field' must be artist, albumartist, composer or genre";

    struct edit target;
    cJSON *one = NULL;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(body, "to");
    if (!cJSON_IsString(item))
        return "'to' must be a string";
    if (tags_is_multi(*f)) { /* read_value() takes a list for these */
        one = cJSON_CreateArray();
        cJSON *s = one != NULL ? cJSON_CreateString(item->valuestring) : NULL;
        if (s == NULL || !cJSON_AddItemToArray(one, s))
            return "out of memory";
        item = one;
    }
    if ((err = read_value(item, *f, &target)) != NULL)
        return err;
    if (target.value.n != 1)
        return message("'%s' is required%s", "to", "");
    *to = target.value.v[0];

    char to_key[DUPES_KEY_MAX + 1], key[DUPES_KEY_MAX + 1];
    dupes_key(*to, strlen(*to), to_key, sizeof to_key);
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(body, "from");
    int size = cJSON_GetArraySize(list);
    if (!cJSON_IsArray(list) || size < 1 || size > MAX_MERGE_FROM)
        return "'from' must be a list of 1 to 64 names";
    from->n = 0;
    from->v = arena_alloc((size_t)size * sizeof *from->v);
    if (from->v == NULL)
        return "out of memory";
    cJSON_ArrayForEach(item, list) {
        const char *s = cJSON_IsString(item) ? item->valuestring : NULL;
        size_t len = s != NULL ? strlen(s) : 0;
        if (len < 1 || len > MAX_MERGE_NAME || !text_valid(s, 0))
            return "each name in 'from' must be text of 1 to 4096 bytes";
        if (strcmp(s, *to) == 0 || merged_name(from, s))
            return "each name in 'from' must be given once, and not be 'to'";
        dupes_key(s, len, key, sizeof key);
        if (to_key[0] == '\0' || strcmp(key, to_key) != 0)
            return message("'%.100s' is not another spelling of '%.100s'", s, *to);
        from->v[from->n++] = s;
    }
    return NULL;
}

/*
 * POST /api/music/merge {"field": f, "from": [names], "to": name}: queues,
 * as one batch, the change of every track whose planned field has a name
 * of from to name to (the duplicates: the same apart from case, accents,
 * punctuation and spaces). field: artist, albumartist, composer or genre;
 * to must pass the rules. A list that would then break the rules (another
 * invalid value) is skipped.
 * -> 200 {batch, queued, dropped, skipped}
 */
void music_merge(struct request *req, struct response *res)
{
    static const struct {
        enum tag_field f;
        const char *sql;
    } tracks_sql[] = {
        { TAG_ARTIST, MERGE_ONE_SQL("artist") },
        { TAG_ALBUMARTIST, MERGE_ONE_SQL("albumartist") },
        { TAG_COMPOSER, MERGE_LIST_SQL("composer") },
        { TAG_GENRE, MERGE_LIST_SQL("genre") },
    };
    cJSON *body = json_body(req, res);
    if (body == NULL)
        return;
    enum tag_field f = TAG_ARTIST;
    struct tag_values from;
    const char *to = NULL;
    const char *err = read_merge(body, &f, &from, &to);
    if (err != NULL) {
        json_error(res, 400, err);
        return;
    }
    const char *sql = NULL;
    for (size_t i = 0; i < sizeof tracks_sql / sizeof tracks_sql[0]; i++)
        if (tracks_sql[i].f == f)
            sql = tracks_sql[i].sql;
    const char *from_json = music_values_json(&from);
    if (sql == NULL || from_json == NULL) {
        json_error(res, 500, "internal error");
        return;
    }

    int lock = lock_for_write(res);
    if (lock < 0)
        return;
    /* The tracks first: queueing changes what the query reads. */
    long long *ids = NULL;
    long long n = 0, batch = -1;
    int queued = 0, dropped = 0, skipped = 0;
    sqlite3_stmt *find = db_prepare(music_db, sql);
    sqlite3_stmt *track = find != NULL ? db_prepare(music_db,
        "SELECT " MUSIC_TAG_COLUMNS " FROM tracks t WHERE t.id = ?") : NULL;
    sqlite3_stmt *pending = track != NULL ? db_prepare(music_db, PENDING_SQL) : NULL;
    int rc = pending != NULL ? db_exec(music_db, "BEGIN IMMEDIATE") : -1;
    long long count = rc == 0 ? single_number("SELECT count(*) FROM tracks", 0) : -1;
    if (count < 0 || (ids = arena_alloc((size_t)(count + 1) * sizeof *ids)) == NULL ||
        sqlite3_bind_text(find, 1, from_json, -1, SQLITE_STATIC) != SQLITE_OK)
        rc = -1;
    int step = SQLITE_ERROR;
    while (rc == 0 && n < count && (step = sqlite3_step(find)) == SQLITE_ROW)
        ids[n++] = sqlite3_column_int64(find, 0);
    if (rc == 0 && step != SQLITE_DONE && step != SQLITE_ROW && n < count) {
        db_log_error(music_db, "merge: tracks");
        rc = -1;
    }
    if (rc == 0 && n > 0 &&
        (batch = single_number("SELECT coalesce(max(batch), 0) + 1 FROM changes", 0)) < 0)
        rc = -1;
    for (long long i = 0; rc == 0 && i < n; i++) {
        size_t mark = arena_mark();
        struct tags t;
        struct edit e;
        unsigned changed;
        int found = 0;
        if (sqlite3_bind_int64(track, 1, ids[i]) != SQLITE_OK ||
            sqlite3_step(track) != SQLITE_ROW ||
            planned_tags(track, 0, ids[i], pending, &t, &changed) != 0 ||
            (found = merge_edit(&t, f, &from, to, &e)) < 0)
            rc = -1;
        sqlite3_reset(track);
        e.track = ids[i];
        if (rc == 0 && found && tags_check(f, &e.value) != NULL)
            skipped++;
        else if (rc == 0 && found)
            rc = queue_one(&e, batch, track, &queued, &dropped);
        arena_rewind(mark);
    }
    if (rc != 0)
        fprintf(stderr, "music: merge into '%.100s' failed\n", to);
    sqlite3_finalize(find);
    sqlite3_finalize(track);
    sqlite3_finalize(pending);
    if (rc == 0)
        rc = db_exec(music_db, "COMMIT");
    if (rc != 0 && sqlite3_get_autocommit(music_db) == 0)
        db_exec(music_db, "ROLLBACK");
    music_unlock(lock);

    cJSON *out = cJSON_CreateObject();
    if (rc != 0 || out == NULL || cJSON_AddNumberToObject(out, "batch", (double)batch) == NULL ||
        cJSON_AddNumberToObject(out, "queued", queued) == NULL ||
        cJSON_AddNumberToObject(out, "dropped", dropped) == NULL ||
        cJSON_AddNumberToObject(out, "skipped", skipped) == NULL) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, out);
}

/* 1 if the track's only picture is hash, 0 if not, -1 on error (logged). */
static int has_only_picture(long long track, const char *hash)
{
    sqlite3_stmt *st = db_prepare(music_db,
        "SELECT count(*) = 1 AND max(hash) = ?2 FROM track_pictures WHERE track_id = ?1");
    int rc = st != NULL && sqlite3_bind_int64(st, 1, track) == SQLITE_OK &&
                     sqlite3_bind_text(st, 2, hash, -1, SQLITE_STATIC) == SQLITE_OK
                 ? sqlite3_step(st)
                 : SQLITE_ERROR;
    int same = rc == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
    if (rc != SQLITE_ROW)
        db_log_error(music_db, "cover: pictures");
    sqlite3_finalize(st);
    return same;
}

/*
 * Queues the cover hash for every track of the album of track album, as
 * batch: or, for a track whose only picture it is already, drops its
 * pending cover. Counts in *queued and *dropped. 0 or -1 (logged).
 */
static int queue_cover(long long album, const char *hash, long long batch, int *queued,
                       int *dropped)
{
    sqlite3_stmt *tracks = db_prepare(music_db, "SELECT t.id FROM tracks t WHERE" SAME_ALBUM);
    sqlite3_stmt *add = db_prepare(music_db,
        "INSERT INTO changes (batch, track_id, field, value)"
        " VALUES (?1, ?2, '" MUSIC_COVER_FIELD "', ?3)"
        " ON CONFLICT (track_id, field) WHERE state = 'pending' DO UPDATE SET"
        "  batch = excluded.batch, value = excluded.value"
        " WHERE value IS NOT excluded.value");
    sqlite3_stmt *drop = db_prepare(music_db,
        "DELETE FROM changes WHERE state = 'pending' AND track_id = ?2"
        " AND field = '" MUSIC_COVER_FIELD "'"); /* ?2 like add's */
    int rc = tracks != NULL && add != NULL && drop != NULL &&
                     sqlite3_bind_int64(tracks, 1, album) == SQLITE_OK
                 ? SQLITE_ROW
                 : SQLITE_ERROR;
    while (rc == SQLITE_ROW && (rc = sqlite3_step(tracks)) == SQLITE_ROW) {
        long long id = sqlite3_column_int64(tracks, 0);
        int same = has_only_picture(id, hash);
        sqlite3_stmt *st = same ? drop : add;
        if (same < 0 || sqlite3_bind_int64(st, 2, id) != SQLITE_OK ||
            (!same && (sqlite3_bind_int64(st, 1, batch) != SQLITE_OK ||
                       sqlite3_bind_text(st, 3, hash, -1, SQLITE_STATIC) != SQLITE_OK)) ||
            sqlite3_step(st) != SQLITE_DONE) {
            rc = SQLITE_ERROR;
            break;
        }
        *(same ? dropped : queued) += sqlite3_changes(music_db) > 0;
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
    }
    if (rc != SQLITE_DONE)
        db_log_error(music_db, "queue cover");
    sqlite3_finalize(tracks);
    sqlite3_finalize(add);
    sqlite3_finalize(drop);
    return rc == SQLITE_DONE ? 0 : -1;
}

/*
 * POST /api/music/cover {"album": track id, "image": base64 of a JPEG}:
 * sets the album's cover. The picture (at most ART_MAX_UPLOAD bytes; only
 * its first bytes are checked here, the write service decodes it) is
 * stored in the art folder, and a change that makes it each track's only
 * picture is queued for every track of the album, as one batch; nothing
 * is written to the files until the write service runs. A track that has
 * only this picture already gets none (its pending cover is dropped).
 * -> 200 {batch, queued, dropped, hash}
 */
void music_cover(struct request *req, struct response *res)
{
    cJSON *body = json_body(req, res);
    if (body == NULL)
        return;
    long long album;
    const char *image = NULL;
    const char *err = get_id(body, "album", &album);
    if (err == NULL)
        err = json_get_string(body, "image", 1, HTTP_MAX_BODY, &image);
    if (err != NULL) {
        json_error(res, 400, err);
        return;
    }
    unsigned char *data = arena_alloc(ART_MAX_UPLOAD);
    if (data == NULL) {
        json_error(res, 500, "internal error");
        return;
    }
    long size = art_base64_decode(image, strlen(image), data, ART_MAX_UPLOAD);
    if (size == -2) {
        json_error(res, 413, "the picture is bigger than 700 KiB");
        return;
    }
    if (size < 0) {
        json_error(res, 400, "'image' must be base64 (A-Z a-z 0-9 + /, padded with =)");
        return;
    }
    const char *mime = art_mime(data, (size_t)size);
    if (mime == NULL || strcmp(mime, "image/jpeg") != 0) {
        json_error(res, 400, "'image' must be a JPEG picture");
        return;
    }
    if (track_exists(res, album) != 1)
        return;
    long long n = single_number("SELECT count(*) FROM tracks t WHERE" SAME_ALBUM, album);
    if (n < 0 || n > MAX_TRACKS) {
        json_error(res, n < 0 ? 500 : 413, n < 0 ? "internal error"
                                                 : "this album has more than 2000 tracks");
        return;
    }
    char hash[ART_HASH_LEN + 1];
    if (art_hash(data, (size_t)size, hash) != 0) {
        json_error(res, 500, "internal error");
        return;
    }

    int lock = lock_for_write(res);
    if (lock < 0)
        return;
    int queued = 0, dropped = 0;
    long long batch = -1;
    sqlite3_stmt *add = db_prepare(music_db,
        "INSERT INTO art (hash, mime, size) VALUES (?, 'image/jpeg', ?)"
        " ON CONFLICT (hash) DO NOTHING");
    /* The file first: a row never names a missing file. If what follows
     * fails, the scan of the whole library removes the file. */
    int rc = add != NULL && art_save(ART_MUSIC, hash, 0, data, (size_t)size) == 0 &&
                     sqlite3_bind_text(add, 1, hash, -1, SQLITE_STATIC) == SQLITE_OK &&
                     sqlite3_bind_int64(add, 2, size) == SQLITE_OK
                 ? db_exec(music_db, "BEGIN IMMEDIATE")
                 : -1;
    if (rc == 0)
        rc = run_once(add); /* finalizes add */
    else
        sqlite3_finalize(add);
    if (rc == 0 &&
        (batch = single_number("SELECT coalesce(max(batch), 0) + 1 FROM changes", 0)) < 0)
        rc = -1;
    if (rc == 0)
        rc = queue_cover(album, hash, batch, &queued, &dropped);
    if (rc == 0)
        rc = db_exec(music_db, "COMMIT");
    if (rc != 0 && sqlite3_get_autocommit(music_db) == 0)
        db_exec(music_db, "ROLLBACK");
    music_unlock(lock);

    cJSON *out = cJSON_CreateObject();
    if (rc != 0 || out == NULL || cJSON_AddNumberToObject(out, "batch", (double)batch) == NULL ||
        cJSON_AddNumberToObject(out, "queued", queued) == NULL ||
        cJSON_AddNumberToObject(out, "dropped", dropped) == NULL ||
        cJSON_AddStringToObject(out, "hash", hash) == NULL) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, out);
}

/*
 * POST /api/music/discard {track} | {album} | {removed: true} | {all: true}:
 * deletes the pending changes of a track, of every track of the album of
 * track album, of the tracks a scan removed, or all. -> 200 {discarded}
 */
void music_discard(struct request *req, struct response *res)
{
    static const char *const sql[] = {
        "DELETE FROM changes WHERE state = 'pending' AND track_id = ?1",
        "DELETE FROM changes WHERE state = 'pending' AND track_id IN"
        " (SELECT t.id FROM tracks t WHERE" SAME_ALBUM ")",
        "DELETE FROM changes WHERE state = 'pending' AND track_id IS NULL",
        "DELETE FROM changes WHERE state = 'pending'",
    };
    static const char *const keys[] = { "track", "album", "removed", "all" };
    cJSON *body = json_body(req, res);
    if (body == NULL)
        return;
    int which = -1, given = 0;
    for (int i = 0; i < 4; i++) {
        if (cJSON_GetObjectItemCaseSensitive(body, keys[i]) != NULL) {
            which = i;
            given++;
        }
    }
    if (given != 1) {
        json_error(res, 400, "give exactly one of 'track', 'album', 'removed' or 'all'");
        return;
    }
    long long id = 0;
    int yes = 0;
    const char *err = which < 2 ? get_id(body, keys[which], &id)
                                : json_get_bool(body, keys[which], &yes);
    if (err == NULL && which >= 2 && !yes)
        err = which == 2 ? "'removed' must be true" : "'all' must be true";
    if (err != NULL) {
        json_error(res, 400, err);
        return;
    }
    if (which < 2 && track_exists(res, id) != 1)
        return;
    int lock = lock_for_write(res);
    if (lock < 0)
        return;
    sqlite3_stmt *st = db_prepare(music_db, sql[which]);
    int rc = -1;
    if (st != NULL && (which >= 2 || sqlite3_bind_int64(st, 1, id) == SQLITE_OK))
        rc = run_once(st); /* finalizes st */
    else
        sqlite3_finalize(st);
    int discarded = rc == 0 ? sqlite3_changes(music_db) : 0;
    music_unlock(lock);

    cJSON *out = cJSON_CreateObject();
    if (rc != 0 || out == NULL || cJSON_AddNumberToObject(out, "discarded", discarded) == NULL) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, out);
}

/* ---- starting the services ---------------------------------------------- */

/* Checks the body's password, after the library (replies 400, 503, 403
 * after a delay, or 500). The body, or NULL after replying. */
static cJSON *password_checked(struct request *req, struct response *res)
{
    cJSON *body = json_body(req, res);
    if (body == NULL)
        return NULL;
    if (cJSON_GetObjectItemCaseSensitive(body, "password") != NULL && !library_ready(res))
        return NULL;
    return audit_password_ok(body, res) ? body : NULL;
}

/*
 * Records the start in the audit table and runs the root action, which
 * starts the service; the caller holds the lock, so no service can start
 * in between, and has done what the service needs (rc 0) or not. -> 202
 */
static void start_action(struct request *req, struct response *res, int lock, int rc,
                         const char *action, const char *detail)
{
    long long audit = rc == 0 ? audit_begin(music_db, req, action, detail) : -1;
    if (audit < 0) {
        if (sqlite3_get_autocommit(music_db) == 0)
            db_exec(music_db, "ROLLBACK");
        music_unlock(lock);
        json_error(res, 500, rc == 0 ? "can not write the audit log; nothing was started"
                                     : "internal error; nothing was started");
        return;
    }
    if (sqlite3_get_autocommit(music_db) == 0 && db_exec(music_db, "COMMIT") != 0) {
        audit_end(music_db, audit, "failed: database error");
        music_unlock(lock);
        json_error(res, 500, "internal error");
        return;
    }
    /* The service waits a moment for this lock, so it starts after we
     * release it. */
    int started = action_run(action, NULL) == 0;
    audit_end(music_db, audit,
              started ? "ok: started" : "failed: the action did not start the service");
    music_unlock(lock);
    if (!started) {
        json_error(res, 500, "could not start it; see the server log");
        return;
    }
    json_reply(res, 202, cJSON_CreateObject());
}

/*
 * POST /api/music/scan {password, track?}: queues a scan of the whole
 * library, or with track one of each track of that album (to read them
 * again), and starts nylm-music-scan.service. -> 202
 */
void music_scan_start(struct request *req, struct response *res)
{
    cJSON *body = password_checked(req, res);
    if (body == NULL)
        return;
    long long track = 0;
    if (cJSON_GetObjectItemCaseSensitive(body, "track") != NULL) {
        const char *err = get_id(body, "track", &track);
        if (err != NULL) {
            json_error(res, 400, err);
            return;
        }
        if (track_exists(res, track) != 1)
            return;
        long long n = single_number("SELECT count(*) FROM tracks t WHERE" SAME_ALBUM, track);
        if (n < 0 || n > MAX_TRACKS) {
            json_error(res, n < 0 ? 500 : 413, n < 0 ? "internal error"
                                                     : "this album has more than 2000 tracks");
            return;
        }
    }
    int lock = lock_for_write(res);
    if (lock < 0)
        return;
    sqlite3_stmt *st = db_prepare(music_db, track == 0
        ? "INSERT INTO scans (path) VALUES (NULL)"
        : "INSERT INTO scans (path) SELECT t.path FROM tracks t WHERE" SAME_ALBUM
          " ORDER BY t.path");
    int rc = db_exec(music_db, "BEGIN IMMEDIATE");
    if (rc == 0 && (st == NULL || (track != 0 && sqlite3_bind_int64(st, 1, track) != SQLITE_OK)))
        rc = -1;
    if (rc == 0)
        rc = run_once(st);
    else
        sqlite3_finalize(st);
    char detail[64];
    snprintf(detail, sizeof detail, track != 0 ? "{\"album_of_track\":%lld}" : "{}", track);
    start_action(req, res, lock, rc, "music-scan", detail);
}

/*
 * GET /api/music/moves: the plan of the move service (src/move.h), from
 * the cache: how many tracks move and the first 5000 (track, from, to);
 * how many can not and the first 5000 (track, path, problem); how many
 * changes are pending (the service waits for them); and the latest moves
 * it made (done, failed, kept).
 */
void music_moves(struct request *req, struct response *res)
{
    (void)req;
    if (music_root() == NULL) {
        json_error(res, 503, "music is not set up: set NYLM_MUSIC in /etc/nylm.conf");
        return;
    }
    struct move_plan plan;
    long long pending = single_number("SELECT count(*) FROM changes WHERE state = 'pending'", 0);
    cJSON *obj = cJSON_CreateObject();
    cJSON *moves = obj != NULL ? cJSON_AddArrayToObject(obj, "moves") : NULL;
    cJSON *problems = moves != NULL ? cJSON_AddArrayToObject(obj, "problems") : NULL;
    cJSON *history = problems != NULL ? cJSON_AddArrayToObject(obj, "history") : NULL;
    int ok = history != NULL && pending >= 0 && move_plan(&plan) == 0 &&
             cJSON_AddNumberToObject(obj, "pending", (double)pending) != NULL &&
             cJSON_AddNumberToObject(obj, "count", (double)plan.moves) != NULL &&
             cJSON_AddNumberToObject(obj, "problem_count", (double)plan.problems) != NULL;
    for (size_t i = 0; ok && i < plan.n; i++) {
        const struct move_item *it = &plan.items[i];
        cJSON *list = it->to != NULL ? moves : it->problem != NULL ? problems : NULL;
        if (list == NULL || cJSON_GetArraySize(list) >= MAX_LIST)
            continue;
        cJSON *row = cJSON_CreateObject();
        ok = row != NULL && cJSON_AddItemToArray(list, row) &&
             cJSON_AddNumberToObject(row, "track", (double)it->id) != NULL &&
             cJSON_AddStringToObject(row, it->to != NULL ? "from" : "path", it->from) != NULL &&
             cJSON_AddStringToObject(row, it->to != NULL ? "to" : "problem",
                                     it->to != NULL ? it->to : it->problem) != NULL;
    }
    sqlite3_stmt *st = ok ? db_prepare(music_db,
        "SELECT id, track_id AS track, from_path, to_path, state, note, finished FROM moves"
        " ORDER BY id DESC LIMIT 300") : NULL;
    ok = st != NULL && add_rows(history, st, 7, MAX_HISTORY) == 0;
    sqlite3_finalize(st);
    if (!ok) {
        fprintf(stderr, "music: moves failed\n");
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, obj);
}

/* POST /api/music/move {password}: starts nylm-music-move.service, when
 * no change is pending (the names come from the files' tags) and a track
 * has to move. -> 202 */
void music_move_start(struct request *req, struct response *res)
{
    if (password_checked(req, res) == NULL)
        return;
    int lock = lock_for_write(res);
    if (lock < 0)
        return;
    struct move_plan plan;
    long long pending = single_number("SELECT count(*) FROM changes WHERE state = 'pending'", 0);
    const char *refused = NULL;
    if (pending < 0 || move_plan(&plan) != 0) {
        music_unlock(lock);
        json_error(res, 500, "internal error");
        return;
    }
    if (pending > 0)
        refused = "write or discard the pending changes first: the names come from the files' tags";
    else if (plan.moves == 0)
        refused = "every track is where its tags put it: there is nothing to move";
    if (refused != NULL) {
        music_unlock(lock);
        json_error(res, 409, refused);
        return;
    }
    start_action(req, res, lock, 0, "music-move", "{}");
}

/* ---- Qobuz ------------------------------------------------------------ */

#define QOBUZ_MAX_URLS      50  /* album links in one request */
#define QOBUZ_MAX_DOWNLOADS 100 /* downloads listed */

/*
 * GET /api/music/qobuz: whether nylm-qobuz runs, the login (connected,
 * the subscription's label, a pasted login waiting, the last error; the
 * token itself never leaves the server), the link to log in at once the
 * web player's app id is known, and the latest downloads.
 */
void music_qobuz(struct request *req, struct response *res)
{
    (void)req;
    int lock = music_qobuz_lock(0);
    if (lock == -1) {
        json_error(res, 500, "internal error");
        return;
    }
    music_unlock(lock);
    sqlite3_stmt *st = db_prepare(music_db,
        "SELECT app_id, token IS NOT NULL AND token <> '', label, login IS NOT NULL, error"
        " FROM qobuz_account WHERE id = 1");
    int rc = st != NULL ? sqlite3_step(st) : SQLITE_ERROR;
    cJSON *obj = cJSON_CreateObject();
    int ok = rc == SQLITE_ROW && obj != NULL &&
             cJSON_AddBoolToObject(obj, "running", lock == MUSIC_BUSY) != NULL;
    if (ok) {
        const char *app_id = (const char *)sqlite3_column_text(st, 0);
        const char *label = (const char *)sqlite3_column_text(st, 2);
        char url[128];
        snprintf(url, sizeof url, "https://www.qobuz.com/signin/oauth?ext_app_id=%s"
                                  "&redirect_url=http://localhost", app_id ? app_id : "");
        ok = (app_id != NULL ? cJSON_AddStringToObject(obj, "login_url", url)
                             : cJSON_AddNullToObject(obj, "login_url")) != NULL &&
             cJSON_AddBoolToObject(obj, "connected", sqlite3_column_int(st, 1)) != NULL &&
             (label != NULL ? cJSON_AddStringToObject(obj, "label", label)
                            : cJSON_AddNullToObject(obj, "label")) != NULL &&
             cJSON_AddBoolToObject(obj, "login_pending", sqlite3_column_int(st, 3)) != NULL &&
             cJSON_AddStringToObject(obj, "error",
                                     (const char *)sqlite3_column_text(st, 4)) != NULL;
    }
    if (rc != SQLITE_ROW)
        db_log_error(music_db, "qobuz account");
    sqlite3_finalize(st);
    cJSON *list = ok ? cJSON_AddArrayToObject(obj, "downloads") : NULL;
    st = list != NULL ? db_prepare(music_db,
        "SELECT id, album_id, title, artist, requested, started, finished, state, tracks, saved,"
        " note FROM qobuz_downloads ORDER BY id DESC LIMIT 100") : NULL;
    ok = st != NULL && add_rows(list, st, 11, QOBUZ_MAX_DOWNLOADS) == 0;
    sqlite3_finalize(st);
    if (!ok) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, obj);
}

/*
 * POST /api/music/qobuz/start {password, login?, urls?}: queues the login
 * the user pasted (the address Qobuz sent them to, or its code) and the
 * album links to download (1 to 50), and starts nylm-qobuz.service, which
 * also reads the web player's app id for the login link. -> 202
 */
void music_qobuz_start(struct request *req, struct response *res)
{
    cJSON *body = password_checked(req, res);
    if (body == NULL)
        return;
    const cJSON *login = cJSON_GetObjectItemCaseSensitive(body, "login");
    const cJSON *urls = cJSON_GetObjectItemCaseSensitive(body, "urls");
    char code[QOBUZ_MAX_TOKEN + 1], token[QOBUZ_MAX_TOKEN + 1], user[QOBUZ_MAX_TOKEN + 1];
    if (login != NULL && (!cJSON_IsString(login) ||
                          qobuz_redirect_parse(login->valuestring, code, token, user) != 0)) {
        json_error(res, 400, "'login' must be the address Qobuz sent you to after logging in "
                             "(it has a code or a token)");
        return;
    }
    int nurls = urls != NULL ? cJSON_GetArraySize(urls) : 0;
    if (urls != NULL && (!cJSON_IsArray(urls) || nurls < 1 || nurls > QOBUZ_MAX_URLS)) {
        json_error(res, 400, "'urls' must be a list of 1 to 50 Qobuz album links");
        return;
    }
    char (*ids)[QOBUZ_MAX_ID + 1] = arena_alloc((size_t)(nurls + 1) * sizeof *ids);
    if (ids == NULL) {
        json_error(res, 500, "internal error");
        return;
    }
    for (int i = 0; i < nurls; i++) {
        const cJSON *u = cJSON_GetArrayItem(urls, i);
        if (!cJSON_IsString(u) || qobuz_album_id(u->valuestring, ids[i]) != 0) {
            char n[16];
            snprintf(n, sizeof n, "%d", i + 1);
            json_error(res, 400, message("link %s is not a Qobuz album link (like %s)", n,
                                         "https://www.qobuz.com/us-en/album/name/ID"));
            return;
        }
    }
    long long connected = single_number(
        "SELECT count(*) FROM qobuz_account WHERE token IS NOT NULL AND token <> ''", 0);
    if (connected < 0) {
        json_error(res, 500, "internal error");
        return;
    }
    if (nurls > 0 && connected == 0 && login == NULL) {
        json_error(res, 409, "connect to Qobuz first");
        return;
    }
    int lock = music_qobuz_lock(0);
    if (lock < 0) {
        json_error(res, lock == MUSIC_BUSY ? 409 : 500,
                   lock == MUSIC_BUSY ? "Qobuz is busy: try again when it is done"
                                      : "internal error");
        return;
    }
    /* One transaction; start_action() commits it, or rolls it back. */
    int rc = db_exec(music_db, "BEGIN IMMEDIATE");
    for (int i = login != NULL ? -1 : 0; rc == 0 && i < nurls; i++) {
        sqlite3_stmt *st = db_prepare(music_db, i < 0
            ? "UPDATE qobuz_account SET login = ?, error = '', updated = unixepoch() WHERE id = 1"
            : "INSERT INTO qobuz_downloads (album_id) VALUES (?)");
        const char *v = i < 0 ? login->valuestring : ids[i];
        if (st != NULL && sqlite3_bind_text(st, 1, v, -1, SQLITE_STATIC) == SQLITE_OK) {
            rc = run_once(st); /* finalizes st */
        } else {
            sqlite3_finalize(st);
            rc = -1;
        }
    }
    char detail[64];
    snprintf(detail, sizeof detail, "{\"login\":%s,\"albums\":%d}", login != NULL ? "true" : "false",
             nurls);
    start_action(req, res, lock, rc, "qobuz", detail);
}

/* POST /api/music/write {password}: starts nylm-music-write.service. -> 202 */
void music_write_start(struct request *req, struct response *res)
{
    if (password_checked(req, res) == NULL)
        return;
    int lock = lock_for_write(res);
    if (lock < 0)
        return;
    long long pending = single_number("SELECT count(*) FROM changes WHERE state = 'pending'", 0);
    if (pending <= 0) {
        music_unlock(lock);
        json_error(res, pending == 0 ? 409 : 500,
                   pending == 0 ? "there are no pending changes to write" : "internal error");
        return;
    }
    start_action(req, res, lock, 0, "music-write", "{}");
}
