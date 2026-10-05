/*
 * nylm-musicbrainz: the MusicBrainz service (only in that binary, with
 * src/https.c).
 *
 * One run: the next MB_ALBUMS albums without a genre that have a
 * MusicBrainz album id (a release id) are looked up: the release, then its
 * release group, whose genres (voted by MusicBrainz's users) are usually
 * the fuller ones; the release's own are used when the group has none.
 * The top MB_GENRES are queued as the genre of the album's tracks without
 * one, as one batch, like any edit: nothing is written to the files here.
 * Every lookup is recorded, so an album is asked once.
 *
 * MusicBrainz allows one request a second and asks for a User-Agent that
 * names the application.
 */
#define _POSIX_C_SOURCE 200809L

#include "musicbrainz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "arena.h"
#include "db.h"
#include "https.h"
#include "music.h"
#include "tags.h"

#define API        "https://musicbrainz.org/ws/2/"
#define USER_AGENT "User-Agent: nylm/1.0 ( personal music library manager )"
#define MAX_JSON   (1024 * 1024)
#define GAP_NS     1100000000LL /* between two requests: a little over a second */
#define TRIES      3            /* a request MusicBrainz answers 503 (busy) */
#define MAX_LISTED 200          /* genres of an entity read, at most */
#define LOCK_TRIES 60           /* seconds to wait for the library lock */
#define NOTE_LEN   256

int mb_id_valid(const char *s)
{
    for (int i = 0; i < MB_ID_LEN; i++) {
        int dash = i == 8 || i == 13 || i == 18 || i == 23;
        char c = s[i];
        if (dash ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return 0;
    }
    return s[MB_ID_LEN] == '\0';
}

/* 1 if name may be taken as a genre: the rule, and short enough. */
static int genre_ok(const char *name)
{
    struct tag_values v = { 1, &name };
    return strlen(name) <= MB_MAX_GENRE && tags_check(TAG_GENRE, &v) == NULL;
}

size_t mb_genres(const cJSON *entity, struct mb_genres *out)
{
    out->n = 0;
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(entity, "genres");
    if (!cJSON_IsArray(list))
        return 0;
    /* The valid ones with their votes, then the most voted first. */
    const char *names[MAX_LISTED];
    double votes[MAX_LISTED];
    int n = 0, read = 0;
    const cJSON *g;
    cJSON_ArrayForEach(g, list) {
        if (read++ == MAX_LISTED)
            break;
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(g, "name");
        const cJSON *count = cJSON_GetObjectItemCaseSensitive(g, "count");
        if (cJSON_IsString(name) && cJSON_IsNumber(count) && count->valuedouble >= 1 &&
            genre_ok(name->valuestring)) {
            names[n] = name->valuestring;
            votes[n++] = count->valuedouble;
        }
    }
    while (out->n < MB_GENRES) {
        int best = -1;
        for (int i = 0; i < n; i++)
            if (names[i] != NULL && (best < 0 || votes[i] > votes[best]))
                best = i;
        if (best < 0)
            break;
        int twice = 0;
        for (size_t k = 0; k < out->n && !twice; k++)
            twice = strcmp(out->name[k], names[best]) == 0;
        if (!twice) {
            snprintf(out->name[out->n], sizeof out->name[out->n], "%s", names[best]);
            out->v[out->n] = out->name[out->n];
            out->n++;
        }
        names[best] = NULL;
    }
    return out->n;
}

const char *mb_release_group(const cJSON *release)
{
    const cJSON *group = cJSON_GetObjectItemCaseSensitive(release, "release-group");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(group, "id");
    return cJSON_IsString(id) && mb_id_valid(id->valuestring) ? id->valuestring : NULL;
}

/* ---- the albums --------------------------------------------------------- */

/* A track's planned MusicBrainz album id ("" or absent: NULL). */
#define PLANNED_MBID                                                                  \
    "nullif(coalesce((SELECT c.value FROM changes c WHERE c.state = 'pending'"        \
    " AND c.track_id = t.id AND c.field = 'musicbrainz_albumid'),"                    \
    " t.musicbrainz_albumid), '')"
/* The track has no planned genre: a pending change to none, or no pending
 * change and none in the file. */
#define NO_GENRE                                                                      \
    "CASE WHEN EXISTS (SELECT 1 FROM changes c WHERE c.state = 'pending'"             \
    "  AND c.track_id = t.id AND c.field = 'genre')"                                  \
    " THEN EXISTS (SELECT 1 FROM changes c WHERE c.state = 'pending'"                 \
    "  AND c.track_id = t.id AND c.field = 'genre' AND json_array_length(c.value) = 0)" \
    " ELSE NOT EXISTS (SELECT 1 FROM track_values v WHERE v.track_id = t.id"          \
    "  AND v.field = 'genre') END"

/* A copy of column col of st in the arena (NULL stays NULL); *bad set
 * when out of memory. */
static const char *column_copy(sqlite3_stmt *st, int col, int *bad)
{
    const char *s = (const char *)sqlite3_column_text(st, col);
    const char *copy = s != NULL ? arena_strndup(s, (size_t)sqlite3_column_bytes(st, col)) : NULL;
    *bad |= s != NULL && copy == NULL;
    return copy;
}

/* 1 if a and b (either may be NULL) are the same text. */
static int same_text(const char *a, const char *b)
{
    return a == NULL ? b == NULL : b != NULL && strcmp(a, b) == 0;
}

/* 1 if a lookup found something for mbid before (it is not asked again),
 * 0 if not, -1 (logged). */
static int looked_up(const char *mbid)
{
    sqlite3_stmt *st = db_prepare(music_db,
        "SELECT count(*) FROM musicbrainz_lookups"
        " WHERE mbid = ? AND state IN ('queued', 'none', 'not_found')");
    int rc = st != NULL && sqlite3_bind_text(st, 1, mbid, -1, SQLITE_STATIC) == SQLITE_OK
                 ? sqlite3_step(st)
                 : SQLITE_ERROR;
    int found = rc == SQLITE_ROW ? sqlite3_column_int(st, 0) > 0 : -1;
    if (rc != SQLITE_ROW)
        db_log_error(music_db, "musicbrainz: lookups");
    sqlite3_finalize(st);
    return found;
}

int mb_next_albums(struct mb_album *out, int max)
{
    sqlite3_stmt *st = db_prepare(music_db,
        "SELECT t.id, t.album, t.albumartist, " PLANNED_MBID " FROM tracks t"
        " WHERE " NO_GENRE
        " ORDER BY t.albumartist COLLATE NOCASE, t.album COLLATE NOCASE, t.albumartist,"
        " t.album, t.id");
    if (st == NULL)
        return -1;
    /* The tracks of the album being read; it is kept if it qualifies. */
    int n = 0, bad = 0, rc;
    struct mb_album a = { .tracks = NULL };
    size_t room = 0;
    int ok = 0;   /* the album so far has one valid id */
    int kept = 0; /* the album read before was taken */
    size_t mark = 0;
    for (;;) {
        rc = sqlite3_step(st);
        const char *album = NULL, *artist = NULL;
        if (rc == SQLITE_ROW) {
            album = (const char *)sqlite3_column_text(st, 1);
            artist = (const char *)sqlite3_column_text(st, 2);
        }
        int next = rc != SQLITE_ROW || a.tracks == NULL || !same_text(album, a.album) ||
                   !same_text(artist, a.albumartist);
        kept = 0;
        if (next && a.tracks != NULL && ok) {
            int seen = looked_up(a.mbid);
            bad |= seen < 0;
            kept = seen == 0;
            if (kept)
                out[n++] = a;
        }
        if (rc != SQLITE_ROW || n == max || bad)
            break;
        const char *mbid = (const char *)sqlite3_column_text(st, 3);
        if (next) {
            if (a.tracks != NULL && !kept) /* the memory of an album not taken */
                arena_rewind(mark);
            mark = arena_mark();
            a.album = column_copy(st, 1, &bad);
            a.albumartist = column_copy(st, 2, &bad);
            a.ntracks = 0;
            room = 16;
            a.tracks = arena_alloc(room * sizeof *a.tracks);
            bad |= a.tracks == NULL;
            ok = mbid != NULL && mb_id_valid(mbid);
            if (ok)
                memcpy(a.mbid, mbid, MB_ID_LEN + 1);
        } else {
            ok = ok && mbid != NULL && strcmp(mbid, a.mbid) == 0;
        }
        if (!bad && a.ntracks == room) {
            long long *more = arena_alloc(2 * room * sizeof *more);
            if (more != NULL)
                memcpy(more, a.tracks, room * sizeof *more);
            a.tracks = more;
            room *= 2;
            bad |= more == NULL;
        }
        if (bad)
            break;
        a.tracks[a.ntracks++] = sqlite3_column_int64(st, 0);
    }
    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
        db_log_error(music_db, "musicbrainz: albums");
    if (bad)
        fprintf(stderr, "musicbrainz: out of memory\n");
    sqlite3_finalize(st);
    return bad || (rc != SQLITE_ROW && rc != SQLITE_DONE) ? -1 : n;
}

/* The genres g as a JSON array in the arena (none: "[]"); NULL when out of
 * memory. */
static const char *genres_json(const struct mb_genres *g)
{
    const char *names[MB_GENRES];
    struct tag_values v = { 0, names };
    for (size_t i = 0; g != NULL && i < g->n; i++)
        names[v.n++] = g->name[i];
    return music_values_json(&v);
}

int mb_queue(const struct mb_album *a, const struct mb_genres *g, long long batch)
{
    const char *value = genres_json(g);
    sqlite3_stmt *st = value != NULL ? db_prepare(music_db,
        "INSERT INTO changes (batch, track_id, field, value)"
        " SELECT ?1, t.id, 'genre', ?3 FROM tracks t"
        " WHERE t.id = ?2 AND " NO_GENRE " AND " PLANNED_MBID " = ?4"
        " ON CONFLICT (track_id, field) WHERE state = 'pending' DO UPDATE SET"
        "  batch = excluded.batch, value = excluded.value") : NULL;
    int queued = 0;
    int rc = st != NULL && sqlite3_bind_int64(st, 1, batch) == SQLITE_OK &&
                     sqlite3_bind_text(st, 3, value, -1, SQLITE_STATIC) == SQLITE_OK &&
                     sqlite3_bind_text(st, 4, a->mbid, -1, SQLITE_STATIC) == SQLITE_OK
                 ? SQLITE_DONE
                 : SQLITE_ERROR;
    for (size_t i = 0; rc == SQLITE_DONE && i < a->ntracks; i++) {
        rc = sqlite3_bind_int64(st, 2, a->tracks[i]) == SQLITE_OK ? sqlite3_step(st)
                                                                   : SQLITE_ERROR;
        queued += rc == SQLITE_DONE && sqlite3_changes(music_db) > 0;
        sqlite3_reset(st);
    }
    if (rc != SQLITE_DONE)
        db_log_error(music_db, "musicbrainz: queue");
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? queued : -1;
}

int mb_record(const struct mb_album *a, const char *state, const struct mb_genres *g,
              const char *note)
{
    const char *value = genres_json(g);
    sqlite3_stmt *st = value != NULL ? db_prepare(music_db,
        "INSERT INTO musicbrainz_lookups (mbid, track, album, albumartist, state, genres, note)"
        " VALUES (?, ?, ?, ?, ?, ?, ?)") : NULL;
    int rc = st != NULL && sqlite3_bind_text(st, 1, a->mbid, -1, SQLITE_STATIC) == SQLITE_OK &&
                     sqlite3_bind_int64(st, 2, a->ntracks > 0 ? a->tracks[0] : 0) == SQLITE_OK &&
                     sqlite3_bind_text(st, 3, a->album, -1, SQLITE_STATIC) == SQLITE_OK &&
                     sqlite3_bind_text(st, 4, a->albumartist, -1, SQLITE_STATIC) == SQLITE_OK &&
                     sqlite3_bind_text(st, 5, state, -1, SQLITE_STATIC) == SQLITE_OK &&
                     sqlite3_bind_text(st, 6, value, -1, SQLITE_STATIC) == SQLITE_OK &&
                     sqlite3_bind_text(st, 7, note, -1, SQLITE_STATIC) == SQLITE_OK
                 ? sqlite3_step(st)
                 : SQLITE_ERROR;
    if (rc != SQLITE_DONE)
        db_log_error(music_db, "musicbrainz: record");
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* ---- requests ----------------------------------------------------------- */

/* Waits until GAP_NS have passed since the last request. */
static void wait_turn(void)
{
    static struct timespec last;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long since = (now.tv_sec - last.tv_sec) * 1000000000LL + (now.tv_nsec - last.tv_nsec);
    if (last.tv_sec != 0 && since < GAP_NS) {
        long long left = GAP_NS - since;
        struct timespec pause = { (time_t)(left / 1000000000LL), (long)(left % 1000000000LL) };
        nanosleep(&pause, NULL);
    }
    clock_gettime(CLOCK_MONOTONIC, &last);
}

/*
 * GET path of the API: its JSON (in the arena) into *out. 1, 0 if
 * MusicBrainz has no such thing (404), or -1 with err set.
 */
static int get(const char *path, cJSON **out, char *err, size_t errlen)
{
    char url[256];
    int u = snprintf(url, sizeof url, "%s%s", API, path);
    if (u < 0 || (size_t)u >= sizeof url) {
        snprintf(err, errlen, "a request is too long");
        return -1;
    }
    const char *headers[] = { USER_AGENT, "Accept: application/json", NULL };
    for (int i = 0; i < TRIES; i++) {
        wait_turn();
        struct https_response r;
        if (https_request("GET", url, headers, NULL, NULL, 0, -1, MAX_JSON, &r, err, errlen) != 0)
            return -1;
        if (r.status == 503 && i + 1 < TRIES) { /* busy: wait a little longer */
            free(r.body);
            wait_turn();
            continue;
        }
        if (r.status == 404) {
            free(r.body);
            return 0;
        }
        if (r.status != 200) {
            snprintf(err, errlen, "MusicBrainz answered %d", r.status);
            free(r.body);
            return -1;
        }
        *out = cJSON_ParseWithLength(r.body, r.len);
        free(r.body);
        if (*out == NULL) {
            snprintf(err, errlen, "MusicBrainz's answer is not JSON");
            return -1;
        }
        return 1;
    }
    snprintf(err, errlen, "MusicBrainz is busy: try again later");
    return -1;
}

/*
 * Looks up the genres of release mbid into g: the release group's, else
 * the release's own. The state for the lookups table, with note.
 */
static const char *lookup(const char *mbid, struct mb_genres *g, char *note, size_t notelen)
{
    size_t mark = arena_mark();
    char path[128], group[MB_ID_LEN + 1] = "";
    cJSON *release = NULL, *rg = NULL;
    g->n = 0;
    snprintf(path, sizeof path, "release/%s?inc=genres+release-groups&fmt=json", mbid);
    int r = get(path, &release, note, notelen);
    const char *state = r < 0 ? "failed" : r == 0 ? "not_found" : NULL;
    if (r == 0)
        snprintf(note, notelen, "MusicBrainz has no release with this id");
    const char *id = r > 0 ? mb_release_group(release) : NULL;
    if (id != NULL)
        memcpy(group, id, sizeof group);
    if (state == NULL && group[0] != '\0') {
        snprintf(path, sizeof path, "release-group/%s?inc=genres&fmt=json", group);
        r = get(path, &rg, note, notelen);
        if (r < 0)
            state = "failed";
        else if (r > 0 && mb_genres(rg, g) > 0)
            snprintf(note, notelen, "from the release group");
    }
    if (state == NULL && g->n == 0 && mb_genres(release, g) > 0)
        snprintf(note, notelen, "from the release");
    if (state == NULL && g->n == 0) {
        state = "none";
        snprintf(note, notelen, "MusicBrainz has no genres for it that nylm can use");
    }
    arena_rewind(mark);
    return state != NULL ? state : "queued";
}

/* ---- a run -------------------------------------------------------------- */

/* Takes the library lock as the server does for a short write, waiting up
 * to LOCK_TRIES seconds while a scan, write or move runs. The fd, or -1. */
static int library_lock(void)
{
    const struct timespec second = { 1, 0 };
    for (int i = 0; i < LOCK_TRIES; i++) {
        int fd = music_lock_shared();
        if (fd != MUSIC_BUSY)
            return fd;
        nanosleep(&second, NULL);
    }
    fprintf(stderr, "musicbrainz: the library stayed busy (a scan, write or move runs)\n");
    return -1;
}

/*
 * Queues the genres found (state "queued") and records every lookup, in one
 * transaction under the library lock. An album that got a genre meanwhile
 * is recorded as skipped. 0 or -1 (logged).
 */
static int save(struct mb_album *albums, int n, const char **state, struct mb_genres *found,
                char (*notes)[NOTE_LEN], int *queued)
{
    int lock = library_lock();
    if (lock < 0)
        return -1;
    long long batch = -1;
    int rc = db_exec(music_db, "BEGIN IMMEDIATE");
    sqlite3_stmt *st = rc == 0 ? db_prepare(music_db,
        "SELECT coalesce(max(batch), 0) + 1 FROM changes") : NULL;
    if (st != NULL && sqlite3_step(st) == SQLITE_ROW)
        batch = sqlite3_column_int64(st, 0);
    else
        rc = -1;
    sqlite3_finalize(st);
    for (int i = 0; rc == 0 && i < n; i++) {
        if (strcmp(state[i], "queued") == 0) {
            int q = mb_queue(&albums[i], &found[i], batch);
            if (q < 0)
                rc = -1;
            else if (q == 0) {
                state[i] = "skipped";
                snprintf(notes[i], NOTE_LEN, "the album has a genre now");
            }
            *queued += q > 0 ? q : 0;
        }
        if (rc == 0)
            rc = mb_record(&albums[i], state[i], &found[i], notes[i]);
    }
    if (rc == 0)
        rc = db_exec(music_db, "COMMIT");
    if (rc != 0 && sqlite3_get_autocommit(music_db) == 0)
        db_exec(music_db, "ROLLBACK");
    music_unlock(lock);
    return rc;
}

int musicbrainz_run(void)
{
    int lock = music_musicbrainz_lock(1);
    if (lock < 0)
        return 1;
    struct mb_album albums[MB_ALBUMS];
    struct mb_genres found[MB_ALBUMS];
    const char *state[MB_ALBUMS];
    char notes[MB_ALBUMS][NOTE_LEN];
    int n = mb_next_albums(albums, MB_ALBUMS);
    for (int i = 0; i < n; i++) {
        notes[i][0] = '\0';
        state[i] = lookup(albums[i].mbid, &found[i], notes[i], NOTE_LEN);
        printf("musicbrainz: %s (%s): %s%s%s\n", albums[i].mbid,
               albums[i].album != NULL ? albums[i].album : "no album name", state[i],
               notes[i][0] != '\0' ? ", " : "", notes[i]);
    }
    int queued = 0;
    int rc = n < 0 ? -1 : n > 0 ? save(albums, n, state, found, notes, &queued) : 0;
    if (rc == 0)
        printf("musicbrainz: %d albums looked up, genres queued for %d tracks\n", n, queued);
    music_unlock(lock);
    return rc == 0 ? 0 : 1;
}
