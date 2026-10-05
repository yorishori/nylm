/*
 * nylm-musicbrainz: the MusicBrainz service (only in that binary, with
 * src/https.c).
 *
 * A run of ids: the next MB_ALBUMS albums without a valid MusicBrainz album
 * id are searched for by their names; a release whose title and artist
 * credit are the same names (normalised) gives its id, queued for the
 * album's tracks as one batch. Anything less sure is left alone.
 *
 * A run of genres: the next MB_ALBUMS albums without a genre that have a
 * MusicBrainz album id (a release id) are looked up: the release, then its
 * release group, whose genres (voted by MusicBrainz's users) are usually
 * the fuller ones; the release's own are used when the group has none.
 * The top MB_GENRES and the release's language are queued as the genre of
 * the album's tracks without one, as one batch, like any edit: nothing is written to the files here.
 * Every lookup is recorded, so an album is asked once.
 *
 * MusicBrainz allows one request a second and asks for a User-Agent that
 * names the application.
 */
#define _POSIX_C_SOURCE 200809L

#include "musicbrainz.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "arena.h"
#include "db.h"
#include "https.h"
#include "music.h"
#include "qobuz.h"
#include "tags.h"

#define API        "https://musicbrainz.org/ws/2/"
#define USER_AGENT "User-Agent: nylm/1.0 ( personal music library manager )"
#define MAX_JSON   (1024 * 1024)
#define GAP_NS     1100000000LL /* between two requests: a little over a second */
#define TRIES      3            /* a request MusicBrainz answers 503 (busy) */
#define MAX_LISTED 200          /* genres of an entity read, at most */
#define LOCK_TRIES 60           /* seconds to wait for the library lock */
#define NOTE_LEN   256
#define MAX_URL    (sizeof API + MB_MAX_QUERY)
#define SEARCHED   25           /* releases a search lists */

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
        mb_add_genre(out, names[best]);
        names[best] = NULL;
    }
    return out->n;
}

void mb_add_genre(struct mb_genres *g, const char *name)
{
    for (size_t k = 0; k < g->n; k++)
        if (strcmp(g->name[k], name) == 0)
            return;
    if (g->n == MB_ALL_GENRES)
        return;
    snprintf(g->name[g->n], sizeof g->name[g->n], "%s", name);
    g->v[g->n] = g->name[g->n];
    g->n++;
}

int mb_add_language(struct mb_genres *g, const char *language)
{
    for (size_t k = 0; k < g->n; k++)
        if (strcmp(g->name[k], "instrumental") == 0)
            return 0;
    mb_add_genre(g, language);
    return 1;
}

/* ISO 639-3 codes and their names as genres. */
static const char *const languages[][2] = {
    { "afr", "afrikaans" }, { "ara", "arabic" },      { "ben", "bengali" },
    { "bul", "bulgarian" }, { "cat", "catalan" },     { "ces", "czech" },
    { "cmn", "chinese" },   { "dan", "danish" },      { "deu", "german" },
    { "ell", "greek" },     { "eng", "english" },     { "est", "estonian" },
    { "eus", "basque" },    { "fas", "persian" },     { "fin", "finnish" },
    { "fra", "french" },    { "gle", "irish" },       { "glg", "galician" },
    { "heb", "hebrew" },    { "hin", "hindi" },       { "hrv", "croatian" },
    { "hun", "hungarian" }, { "ind", "indonesian" },  { "isl", "icelandic" },
    { "ita", "italian" },   { "jpn", "japanese" },    { "kor", "korean" },
    { "lat", "latin" },     { "lav", "latvian" },     { "lit", "lithuanian" },
    { "msa", "malay" },     { "nld", "dutch" },       { "nor", "norwegian" },
    { "pol", "polish" },    { "por", "portuguese" },  { "ron", "romanian" },
    { "rus", "russian" },   { "slk", "slovak" },      { "slv", "slovenian" },
    { "spa", "spanish" },   { "srp", "serbian" },     { "swa", "swahili" },
    { "swe", "swedish" },   { "tam", "tamil" },       { "tgl", "filipino" },
    { "tha", "thai" },      { "tur", "turkish" },     { "ukr", "ukrainian" },
    { "urd", "urdu" },      { "vie", "vietnamese" },  { "yue", "cantonese" },
    { "zho", "chinese" },   { "zxx", "instrumental" },
};
#define NLANGUAGES (sizeof languages / sizeof languages[0])

/* The index in languages of code (a JSON item), or -1 if it is not a
 * string of 3 lowercase letters (*valid 0) or not in the list. */
static int language_index(const cJSON *code, int *valid)
{
    const char *s = cJSON_IsString(code) ? code->valuestring : NULL;
    int ok = s != NULL && strlen(s) == 3;
    for (int i = 0; ok && i < 3; i++)
        ok = s[i] >= 'a' && s[i] <= 'z';
    *valid = ok;
    for (size_t i = 0; ok && i < NLANGUAGES; i++)
        if (strcmp(languages[i][0], s) == 0)
            return (int)i;
    return -1;
}

const char *mb_language(const cJSON *release, const char **code)
{
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(release, "text-representation");
    const cJSON *lang = cJSON_GetObjectItemCaseSensitive(text, "language");
    int valid;
    int i = language_index(lang, &valid);
    *code = valid ? lang->valuestring : NULL;
    return i >= 0 ? languages[i][1] : NULL;
}

size_t mb_lyrics_languages(const cJSON *release, const char *out[MB_LANGUAGES])
{
    int tracks[NLANGUAGES] = { 0 };  /* tracks sung in each language */
    int first[NLANGUAGES] = { 0 };   /* the order they were first sung in */
    int order = 0, read = 0;
    size_t zxx = NLANGUAGES;         /* the index of "no lyrics" */
    for (size_t i = 0; i < NLANGUAGES; i++)
        if (strcmp(languages[i][0], "zxx") == 0)
            zxx = i;
    const cJSON *media = cJSON_GetObjectItemCaseSensitive(release, "media");
    const cJSON *medium, *track, *rel, *code;
    cJSON_ArrayForEach(medium, media) {
        const cJSON *list = cJSON_GetObjectItemCaseSensitive(medium, "tracks");
        cJSON_ArrayForEach(track, list) {
            if (read++ == MB_TRACKS)
                break;
            /* A track counts once for each language, whatever its works. */
            int here[NLANGUAGES] = { 0 };
            const cJSON *rec = cJSON_GetObjectItemCaseSensitive(track, "recording");
            const cJSON *rels = cJSON_GetObjectItemCaseSensitive(rec, "relations");
            cJSON_ArrayForEach(rel, rels) {
                const cJSON *type = cJSON_GetObjectItemCaseSensitive(rel, "type");
                const cJSON *work = cJSON_GetObjectItemCaseSensitive(rel, "work");
                if (!cJSON_IsString(type) || strcmp(type->valuestring, "performance") != 0)
                    continue;
                const cJSON *codes = cJSON_GetObjectItemCaseSensitive(work, "languages");
                cJSON_ArrayForEach(code, codes) {
                    int valid;
                    int i = language_index(code, &valid);
                    if (i >= 0)
                        here[i] = 1;
                }
            }
            for (size_t i = 0; i < NLANGUAGES; i++)
                if (here[i] && tracks[i]++ == 0)
                    first[i] = order++;
        }
    }
    /* Sung languages first; "instrumental" only when none is sung. */
    size_t n = 0;
    int sung = 0;
    for (size_t i = 0; i < NLANGUAGES; i++)
        sung |= i != zxx && tracks[i] > 0;
    if (sung && zxx < NLANGUAGES)
        tracks[zxx] = 0;
    while (n < MB_LANGUAGES) {
        int best = -1;
        for (size_t i = 0; i < NLANGUAGES; i++)
            if (tracks[i] > 0 && (best < 0 || tracks[i] > tracks[best] ||
                                  (tracks[i] == tracks[best] && first[i] < first[best])))
                best = (int)i;
        if (best < 0)
            break;
        out[n++] = languages[best][1];
        tracks[best] = 0;
    }
    return n;
}

const char *mb_release_group(const cJSON *release)
{
    const cJSON *group = cJSON_GetObjectItemCaseSensitive(release, "release-group");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(group, "id");
    return cJSON_IsString(id) && mb_id_valid(id->valuestring) ? id->valuestring : NULL;
}

/* ---- finding an id by its names ----------------------------------------- */

/* The length of a typographic punctuation mark (UTF-8) at s: ‐ ‑ ‒ – — ―
 * ‘ ’ “ ” …; 0 if s is not one. */
static size_t typographic(const unsigned char *s)
{
    if (s[0] != 0xe2 || s[1] != 0x80)
        return 0;
    unsigned char c = s[2];
    return (c >= 0x90 && c <= 0x95) || c == 0x98 || c == 0x99 || c == 0x9c || c == 0x9d ||
                   c == 0xa6
               ? 3
               : 0;
}

int mb_normalize(const char *name, char *out, size_t size)
{
    const unsigned char *s = (const unsigned char *)name;
    size_t n = 0;
    int space = 0; /* a space is owed before the next character */
    while (*s != '\0') {
        size_t skip = typographic(s);
        unsigned char c = *s;
        if (skip > 0 || (c < 0x80 && ispunct(c))) {
            s += skip > 0 ? skip : 1;
            continue;
        }
        s++;
        if (c < 0x80 && isspace(c)) {
            space = n > 0;
            continue;
        }
        /* À..Þ (but ×) in UTF-8 is c3 80..9e; its lowercase is 0x20 on */
        int latin = c == 0xc3 && *s >= 0x80 && *s <= 0x9e && *s != 0x97;
        if (n + (size_t)space + 1 + (size_t)latin >= size)
            return -1;
        if (space)
            out[n++] = ' ';
        space = 0;
        out[n++] = (char)(c < 0x80 ? tolower(c) : c);
        if (latin)
            out[n++] = (char)(*s++ + 0x20);
    }
    out[n] = '\0';
    if (strncmp(out, "the ", 4) == 0)
        memmove(out, out + 4, n - 3);
    return 0;
}

/* s as a Lucene phrase (in quotes, " and \ escaped) at out + *n. -1 if it
 * does not fit. */
static int phrase(const char *s, char *out, size_t size, size_t *n)
{
    if (*n + 1 >= size)
        return -1;
    out[(*n)++] = '"';
    for (; *s != '\0'; s++) {
        if (*n + 3 >= size)
            return -1;
        if (*s == '"' || *s == '\\')
            out[(*n)++] = '\\';
        out[(*n)++] = *s;
    }
    out[(*n)++] = '"';
    out[*n] = '\0';
    return 0;
}

int mb_search_path(const char *album, const char *albumartist, char *out, size_t size)
{
    char query[MB_MAX_QUERY], encoded[MB_MAX_QUERY];
    size_t n = (size_t)snprintf(query, sizeof query, "release:");
    if (phrase(album, query, sizeof query, &n) != 0)
        return -1;
    int w = snprintf(query + n, sizeof query - n, " AND artist:");
    if (w < 0 || (size_t)w >= sizeof query - n)
        return -1;
    n += (size_t)w;
    if (phrase(albumartist, query, sizeof query, &n) != 0 ||
        qobuz_urlencode(query, encoded, sizeof encoded) != 0)
        return -1;
    w = snprintf(out, size, "release/?query=%s&limit=%d&fmt=json", encoded, SEARCHED);
    return w < 0 || (size_t)w >= size ? -1 : 0;
}

/* The artist credit of a release (names and join phrases) into out. 0, or
 * -1 if it has none or it does not fit. */
static int credit(const cJSON *release, char *out, size_t size)
{
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(release, "artist-credit");
    const cJSON *item;
    size_t n = 0;
    out[0] = '\0';
    if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) == 0)
        return -1;
    cJSON_ArrayForEach(item, list) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        const cJSON *join = cJSON_GetObjectItemCaseSensitive(item, "joinphrase");
        if (!cJSON_IsString(name))
            return -1;
        int w = snprintf(out + n, size - n, "%s%s", name->valuestring,
                         cJSON_IsString(join) ? join->valuestring : "");
        if (w < 0 || (size_t)w >= size - n)
            return -1;
        n += (size_t)w;
    }
    return 0;
}

const char *mb_pick_release(const cJSON *search, const char *album, const char *albumartist)
{
    char want_album[MB_MAX_NAME], want_artist[MB_MAX_NAME];
    char raw[MB_MAX_NAME], title[MB_MAX_NAME], artist[MB_MAX_NAME];
    if (mb_normalize(album, want_album, sizeof want_album) != 0 ||
        mb_normalize(albumartist, want_artist, sizeof want_artist) != 0)
        return NULL;
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(search, "releases");
    const cJSON *r;
    const char *first = NULL;
    int read = 0;
    cJSON_ArrayForEach(r, list) {
        if (read++ == SEARCHED)
            break;
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(r, "id");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(r, "title");
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(r, "status");
        if (!cJSON_IsString(id) || !mb_id_valid(id->valuestring) || !cJSON_IsString(name) ||
            mb_normalize(name->valuestring, title, sizeof title) != 0 ||
            strcmp(title, want_album) != 0 || credit(r, raw, sizeof raw) != 0 ||
            mb_normalize(raw, artist, sizeof artist) != 0 || strcmp(artist, want_artist) != 0)
            continue;
        if (cJSON_IsString(status) && strcmp(status->valuestring, "Official") == 0)
            return id->valuestring;
        if (first == NULL)
            first = id->valuestring;
    }
    return first;
}

/* ---- the albums --------------------------------------------------------- */

/* A track's planned value of single-valued field f ("" or absent: NULL). */
#define PLANNED(f)                                                                    \
    "nullif(coalesce((SELECT c.value FROM changes c WHERE c.state = 'pending'"        \
    " AND c.track_id = t.id AND c.field = '" f "'), t." f "), '')"
#define PLANNED_MBID PLANNED("musicbrainz_albumid")
/* GLOB pattern of a valid MusicBrainz id (as mb_id_valid()). */
#define HEX4 "[0-9a-f][0-9a-f][0-9a-f][0-9a-f]"
#define ID_GLOB "'" HEX4 HEX4 "-" HEX4 "-" HEX4 "-" HEX4 "-" HEX4 HEX4 HEX4 "'"
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

/* 1 if a search by album and albumartist was answered before (it is not
 * asked again), 0 if not, -1 (logged). */
static int searched(const char *album, const char *albumartist)
{
    sqlite3_stmt *st = db_prepare(music_db,
        "SELECT count(*) FROM musicbrainz_searches"
        " WHERE album = ? AND albumartist = ? AND state IN ('queued', 'unsure', 'not_found')");
    int rc = st != NULL && sqlite3_bind_text(st, 1, album, -1, SQLITE_STATIC) == SQLITE_OK &&
                     sqlite3_bind_text(st, 2, albumartist, -1, SQLITE_STATIC) == SQLITE_OK
                 ? sqlite3_step(st)
                 : SQLITE_ERROR;
    int found = rc == SQLITE_ROW ? sqlite3_column_int(st, 0) > 0 : -1;
    if (rc != SQLITE_ROW)
        db_log_error(music_db, "musicbrainz: searches");
    sqlite3_finalize(st);
    return found;
}

/*
 * The next albums of one job, at most max, into out (in the arena), as
 * mb_next_albums() (ids 0) or mb_next_unidentified() (ids 1) say. The
 * count, or -1 (logged).
 */
static int next_albums(int ids, struct mb_album *out, int max)
{
    sqlite3_stmt *st = db_prepare(music_db, ids
        ? "SELECT id, album, albumartist, mbid FROM (SELECT t.id, " PLANNED("album")
          " AS album, " PLANNED("albumartist") " AS albumartist, " PLANNED_MBID " AS mbid"
          " FROM tracks t) ORDER BY albumartist COLLATE NOCASE, album COLLATE NOCASE,"
          " albumartist, album, id"
        : "SELECT t.id, t.album, t.albumartist, " PLANNED_MBID " FROM tracks t"
          " WHERE " NO_GENRE
          " ORDER BY t.albumartist COLLATE NOCASE, t.album COLLATE NOCASE, t.albumartist,"
          " t.album, t.id");
    if (st == NULL)
        return -1;
    /* The tracks of the album being read; it is kept if it qualifies. */
    int n = 0, bad = 0, rc;
    struct mb_album a = { .tracks = NULL };
    size_t room = 0;
    int ok = 0;   /* the album so far qualifies: one valid id (genres), or
                   * names and no valid id (ids) */
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
            int seen = ids ? searched(a.album, a.albumartist) : looked_up(a.mbid);
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
            int valid = mbid != NULL && mb_id_valid(mbid);
            ok = ids ? a.album != NULL && a.albumartist != NULL && !valid : valid;
            a.mbid[0] = '\0';
            if (valid && !ids)
                memcpy(a.mbid, mbid, MB_ID_LEN + 1);
        } else if (ids) {
            ok = ok && (mbid == NULL || !mb_id_valid(mbid));
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
        db_log_error(music_db, ids ? "musicbrainz: albums without an id" : "musicbrainz: albums");
    if (bad)
        fprintf(stderr, "musicbrainz: out of memory\n");
    sqlite3_finalize(st);
    return bad || (rc != SQLITE_ROW && rc != SQLITE_DONE) ? -1 : n;
}

int mb_next_albums(struct mb_album *out, int max)
{
    return next_albums(0, out, max);
}

int mb_next_unidentified(struct mb_album *out, int max)
{
    return next_albums(1, out, max);
}

/* The genres g as a JSON array in the arena (none: "[]"); NULL when out of
 * memory. */
static const char *genres_json(const struct mb_genres *g)
{
    const char *names[MB_ALL_GENRES];
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

int mb_queue_id(const struct mb_album *a, const char *mbid, long long batch)
{
    sqlite3_stmt *st = db_prepare(music_db,
        "INSERT INTO changes (batch, track_id, field, value)"
        " SELECT ?1, t.id, 'musicbrainz_albumid', ?3 FROM tracks t"
        " WHERE t.id = ?2 AND coalesce(" PLANNED_MBID ", '') NOT GLOB " ID_GLOB
        " ON CONFLICT (track_id, field) WHERE state = 'pending' DO UPDATE SET"
        "  batch = excluded.batch, value = excluded.value");
    int queued = 0;
    int rc = st != NULL && sqlite3_bind_int64(st, 1, batch) == SQLITE_OK &&
                     sqlite3_bind_text(st, 3, mbid, -1, SQLITE_STATIC) == SQLITE_OK
                 ? SQLITE_DONE
                 : SQLITE_ERROR;
    for (size_t i = 0; rc == SQLITE_DONE && i < a->ntracks; i++) {
        rc = sqlite3_bind_int64(st, 2, a->tracks[i]) == SQLITE_OK ? sqlite3_step(st)
                                                                   : SQLITE_ERROR;
        queued += rc == SQLITE_DONE && sqlite3_changes(music_db) > 0;
        sqlite3_reset(st);
    }
    if (rc != SQLITE_DONE)
        db_log_error(music_db, "musicbrainz: queue an id");
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? queued : -1;
}

int mb_record_search(const struct mb_album *a, const char *state, const char *mbid,
                     const char *note)
{
    sqlite3_stmt *st = db_prepare(music_db,
        "INSERT INTO musicbrainz_searches (track, album, albumartist, state, mbid, note)"
        " VALUES (?, ?, ?, ?, ?, ?)");
    int rc = st != NULL &&
                     sqlite3_bind_int64(st, 1, a->ntracks > 0 ? a->tracks[0] : 0) == SQLITE_OK &&
                     sqlite3_bind_text(st, 2, a->album, -1, SQLITE_STATIC) == SQLITE_OK &&
                     sqlite3_bind_text(st, 3, a->albumartist, -1, SQLITE_STATIC) == SQLITE_OK &&
                     sqlite3_bind_text(st, 4, state, -1, SQLITE_STATIC) == SQLITE_OK &&
                     sqlite3_bind_text(st, 5, mbid, -1, SQLITE_STATIC) == SQLITE_OK &&
                     sqlite3_bind_text(st, 6, note, -1, SQLITE_STATIC) == SQLITE_OK
                 ? sqlite3_step(st)
                 : SQLITE_ERROR;
    if (rc != SQLITE_DONE)
        db_log_error(music_db, "musicbrainz: record a search");
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
    char url[MAX_URL];
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
    snprintf(path, sizeof path,
             "release/%s?inc=genres+release-groups+recordings+work-rels+recording-level-rels"
             "&fmt=json", mbid);
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
            snprintf(note, notelen, "genres from the release group");
    }
    if (state == NULL && g->n == 0 && mb_genres(release, g) > 0)
        snprintf(note, notelen, "genres from the release");
    if (state == NULL && g->n == 0)
        snprintf(note, notelen, "no genres that nylm can use");
    if (state == NULL) {
        const char *code = NULL, *sung[MB_LANGUAGES];
        size_t nsung = mb_lyrics_languages(release, sung);
        const char *language = nsung == 0 ? mb_language(release, &code) : NULL;
        size_t len = strlen(note);
        int added = 0;
        for (size_t i = 0; i < nsung; i++)
            added |= mb_add_language(g, sung[i]);
        if (language != NULL)
            added = mb_add_language(g, language);
        snprintf(note + len, notelen - len, "%s",
                 added && nsung > 0               ? ", and the lyrics' languages"
                 : added                          ? ", and the release's language"
                 : nsung > 0 || language != NULL  ? ", instrumental (no language)"
                 : code == NULL                   ? ", no language"
                 : strcmp(code, "mul") == 0       ? ", several languages (left out)"
                                                  : ", a language nylm does not know: ");
        len = strlen(note);
        if (nsung == 0 && language == NULL && code != NULL && strcmp(code, "mul") != 0)
            snprintf(note + len, notelen - len, "%s", code);
        if (g->n == 0)
            state = "none";
    }
    arena_rewind(mark);
    return state != NULL ? state : "queued";
}

/*
 * Searches for album a by its names, into mbid ("" if none found): the
 * search as given, and again without a leading "The " in the album artist
 * (MusicBrainz often credits "Pixies" for "The Pixies"). The state for the
 * searches table, with note.
 */
static const char *search(const struct mb_album *a, char *mbid, char *note, size_t notelen)
{
    size_t mark = arena_mark();
    const char *state = NULL;
    int listed = 0; /* releases found that did not match */
    mbid[0] = '\0';
    for (int pass = 0; pass < 2 && state == NULL && mbid[0] == '\0'; pass++) {
        const char *artist = a->albumartist;
        if (pass == 1 && strncasecmp(artist, "the ", 4) != 0)
            break;
        if (pass == 1)
            artist += 4;
        char path[MB_MAX_QUERY];
        cJSON *found = NULL;
        if (mb_search_path(a->album, artist, path, sizeof path) != 0) {
            state = "not_found";
            snprintf(note, notelen, "the names are too long to search for");
            break;
        }
        int r = get(path, &found, note, notelen);
        if (r <= 0) {
            state = "failed";
            if (r == 0)
                snprintf(note, notelen, "MusicBrainz did not find the search");
            break;
        }
        const char *id = mb_pick_release(found, a->album, a->albumartist);
        if (id != NULL)
            memcpy(mbid, id, MB_ID_LEN + 1);
        const cJSON *list = cJSON_GetObjectItemCaseSensitive(found, "releases");
        listed += cJSON_IsArray(list) ? cJSON_GetArraySize(list) : 0;
    }
    arena_rewind(mark);
    if (state != NULL)
        return state;
    if (mbid[0] != '\0') {
        snprintf(note, notelen, "a release with these names");
        return "queued";
    }
    if (listed == 0) {
        snprintf(note, notelen, "MusicBrainz has no release by these names");
        return "not_found";
    }
    snprintf(note, notelen, "%d releases found, none with exactly these names", listed);
    return "unsure";
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

/* What a lookup or search found for an album. */
struct result {
    const char *state;
    struct mb_genres genres;      /* genres */
    char mbid[MB_ID_LEN + 1];     /* ids: "" if none */
    char note[NOTE_LEN];
};

/*
 * Queues what was found (state "queued": the ids or the genres) and
 * records every lookup or search, in one transaction under the library
 * lock. An album that got an id or a genre meanwhile is recorded as
 * skipped. *queued gets the tracks queued. 0 or -1 (logged).
 */
static int save(int ids, struct mb_album *albums, int n, struct result *res, int *queued)
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
        struct result *r = &res[i];
        if (strcmp(r->state, "queued") == 0) {
            int q = ids ? mb_queue_id(&albums[i], r->mbid, batch)
                        : mb_queue(&albums[i], &r->genres, batch);
            if (q < 0)
                rc = -1;
            else if (q == 0) {
                r->state = "skipped";
                snprintf(r->note, NOTE_LEN, ids ? "the album has an id now"
                                                : "the album has a genre now");
            }
            *queued += q > 0 ? q : 0;
        }
        if (rc == 0)
            rc = ids ? mb_record_search(&albums[i], r->state, r->mbid[0] != '\0' ? r->mbid : NULL,
                                        r->note)
                     : mb_record(&albums[i], r->state, &r->genres, r->note);
    }
    if (rc == 0)
        rc = db_exec(music_db, "COMMIT");
    if (rc != 0 && sqlite3_get_autocommit(music_db) == 0)
        db_exec(music_db, "ROLLBACK");
    music_unlock(lock);
    return rc;
}

int musicbrainz_run(const char *what)
{
    int ids = strcmp(what, "ids") == 0;
    int lock = music_musicbrainz_lock(1);
    if (lock < 0)
        return 1;
    struct mb_album albums[MB_ALBUMS];
    struct result res[MB_ALBUMS];
    int n = ids ? mb_next_unidentified(albums, MB_ALBUMS) : mb_next_albums(albums, MB_ALBUMS);
    for (int i = 0; i < n; i++) {
        struct result *r = &res[i];
        r->note[0] = '\0';
        r->mbid[0] = '\0';
        r->genres.n = 0;
        r->state = ids ? search(&albums[i], r->mbid, r->note, NOTE_LEN)
                       : lookup(albums[i].mbid, &r->genres, r->note, NOTE_LEN);
        const char *id = ids ? r->mbid : albums[i].mbid;
        printf("musicbrainz: %s / %s: %s%s%s%s%s\n",
               albums[i].albumartist != NULL ? albums[i].albumartist : "no album artist",
               albums[i].album != NULL ? albums[i].album : "no album name", r->state,
               id[0] != '\0' ? " " : "", id, r->note[0] != '\0' ? ", " : "", r->note);
    }
    int queued = 0;
    int rc = n < 0 ? -1 : n > 0 ? save(ids, albums, n, res, &queued) : 0;
    if (rc == 0)
        printf("musicbrainz: %d albums looked up, %s queued for %d tracks\n", n,
               ids ? "ids" : "genres", queued);
    music_unlock(lock);
    return rc == 0 ? 0 : 1;
}
