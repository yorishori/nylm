/*
 * Navidrome in the music app (src/navidrome.h): its login, and what it
 * keeps that nylm can use, each song matched to the track in nylm's cache
 * (track: its id, or null when nylm does not have it): starred albums and
 * songs, playlists and their songs. Navidrome answers on this machine in
 * a few milliseconds; each call has a 10 s timeout.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>

#include "api.h"
#include "audit.h"
#include "db.h"
#include "json.h"
#include "music.h"
#include "navidrome.h"
#include "tags.h"

#define MAX_ROWS 5000 /* albums, songs or playlists in one answer */

/* Replies 503 and returns 0 unless Navidrome is set up. */
static int ready(struct response *res)
{
    if (navidrome_url() == NULL) {
        json_error(res, 503, "Navidrome is not set up: set NYLM_NAVIDROME in /etc/nylm.conf");
        return 0;
    }
    return 1;
}

/* Replies 409 and returns 0 unless a login is kept (500 on error). */
static int has_login(struct response *res)
{
    const char *user;
    int found = navidrome_login(&user);
    if (found == 0)
        json_error(res, 409, "nylm has no Navidrome login yet: add it first");
    else if (found < 0)
        json_error(res, 500, "internal error");
    return found == 1;
}

/* Calls method with the kept login; replies 502 (Navidrome did not answer,
 * or refused) and returns NULL if it fails. */
static cJSON *call(struct response *res, const char *method, const char *extra)
{
    char err[512];
    cJSON *r = navidrome_call(method, extra, NULL, NULL, NULL, err, sizeof err);
    if (r == NULL) {
        fprintf(stderr, "navidrome: %s: %s\n", method, err);
        json_error(res, 502, err);
    }
    return r;
}

/* Adds key: the string item of obj, or null. NULL when out of memory. */
static cJSON *add_text(cJSON *to, const char *key, const cJSON *obj, const char *from)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, from);
    return cJSON_IsString(v) && text_valid(v->valuestring, 0)
               ? cJSON_AddStringToObject(to, key, v->valuestring)
               : cJSON_AddNullToObject(to, key);
}

/* Adds key: the number item of obj, or null. NULL when out of memory. */
static cJSON *add_number(cJSON *to, const char *key, const cJSON *obj, const char *from)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, from);
    return cJSON_IsNumber(v) ? cJSON_AddNumberToObject(to, key, v->valuedouble)
                             : cJSON_AddNullToObject(to, key);
}

/* Adds "track": the first row's id of st (reset after), with text a and
 * b bound (b NULL: one parameter), or null. 0, or -1 (logged). */
static int add_track(cJSON *to, sqlite3_stmt *st, const char *a, const char *b)
{
    int rc = sqlite3_bind_text(st, 1, a, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK && b != NULL)
        rc = sqlite3_bind_text(st, 2, b, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK)
        rc = sqlite3_step(st);
    cJSON *added = rc == SQLITE_ROW
                       ? cJSON_AddNumberToObject(to, "track", (double)sqlite3_column_int64(st, 0))
                       : cJSON_AddNullToObject(to, "track");
    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
        db_log_error(music_db, "navidrome: match");
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    return (rc == SQLITE_ROW || rc == SQLITE_DONE) && added != NULL ? 0 : -1;
}

/*
 * Adds each song of the list in r (a JSON array, may be missing) to list:
 * id, title, artist, album, duration and the track nylm has at its path.
 * 0, or -1 (logged).
 */
static int add_songs(cJSON *list, const cJSON *songs)
{
    sqlite3_stmt *st = db_prepare(music_db, "SELECT id FROM tracks WHERE path = ?");
    if (st == NULL)
        return -1;
    const cJSON *s;
    int ok = 1, n = 0;
    cJSON_ArrayForEach(s, songs) {
        if (!ok || n++ >= MAX_ROWS)
            break;
        const cJSON *path = cJSON_GetObjectItemCaseSensitive(s, "path");
        char local[TAGS_MAX_PATH];
        int found = cJSON_IsString(path) &&
                    navidrome_local_path(navidrome_music(), music_root(), path->valuestring,
                                         local, sizeof local) == 0;
        cJSON *row = cJSON_CreateObject();
        ok = row != NULL && cJSON_AddItemToArray(list, row) &&
             add_text(row, "id", s, "id") != NULL && add_text(row, "title", s, "title") != NULL &&
             add_text(row, "artist", s, "artist") != NULL &&
             add_text(row, "album", s, "album") != NULL &&
             add_number(row, "duration", s, "duration") != NULL &&
             (found ? add_track(row, st, local, NULL) == 0
                    : cJSON_AddNullToObject(row, "track") != NULL);
    }
    sqlite3_finalize(st);
    if (!ok)
        fprintf(stderr, "navidrome: songs: out of memory\n");
    return ok ? 0 : -1;
}

/*
 * GET /api/music/navidrome: whether Navidrome is set up (its address and
 * music folder), the login's user name (null: none), and whether
 * Navidrome answers with it (connected; version; error says why not).
 */
void music_navidrome(struct request *req, struct response *res)
{
    (void)req;
    const char *user = NULL;
    int found = navidrome_url() != NULL ? navidrome_login(&user) : 0;
    char err[512] = "";
    cJSON *r = found == 1 ? navidrome_call("ping", "", NULL, NULL, NULL, err, sizeof err) : NULL;
    cJSON *obj = cJSON_CreateObject();
    const char *url = navidrome_url();
    if (found < 0 || obj == NULL ||
        cJSON_AddBoolToObject(obj, "configured", url != NULL) == NULL ||
        (url != NULL ? cJSON_AddStringToObject(obj, "url", url)
                     : cJSON_AddNullToObject(obj, "url")) == NULL ||
        (url != NULL ? cJSON_AddStringToObject(obj, "music", navidrome_music())
                     : cJSON_AddNullToObject(obj, "music")) == NULL ||
        (user != NULL ? cJSON_AddStringToObject(obj, "user", user)
                      : cJSON_AddNullToObject(obj, "user")) == NULL ||
        cJSON_AddBoolToObject(obj, "connected", r != NULL) == NULL ||
        add_text(obj, "version", r, "serverVersion") == NULL ||
        cJSON_AddStringToObject(obj, "error", err) == NULL) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, obj);
}

/*
 * POST /api/music/navidrome/login {password, user, navidrome_password}:
 * keeps the Navidrome login nylm uses, once Navidrome accepts it and the
 * user is an admin there (400 if not, 502 if Navidrome does not answer).
 * The password again; audited. -> 200 {user}
 */
void music_navidrome_login(struct request *req, struct response *res)
{
    cJSON *body = json_body(req, res);
    if (body == NULL || !audit_password_ok(body, res))
        return;
    const char *user, *password;
    if (json_get_text(body, "user", 1, NAVIDROME_MAX_USER, 0, &user) != NULL) {
        json_error(res, 400, "'user' must be Navidrome's user name (1 to 64 bytes of text)");
        return;
    }
    if (json_get_text(body, "navidrome_password", 1, NAVIDROME_MAX_PASSWORD, 0, &password) !=
        NULL) {
        json_error(res, 400, "'navidrome_password' must be the user's password in Navidrome "
                             "(1 to 256 bytes of text)");
        return;
    }
    if (!ready(res))
        return;
    int refused;
    char err[512];
    if (navidrome_check_login(user, password, &refused, err, sizeof err) != 0) {
        fprintf(stderr, "navidrome: login of %s not kept: %s\n", user, err);
        json_error(res, refused ? 400 : 502, err);
        return;
    }
    int lock = music_lock_shared();
    if (lock < 0) {
        json_error(res, lock == MUSIC_BUSY ? 409 : 500,
                   lock == MUSIC_BUSY ? "the library is busy: a scan or write is running; try "
                                        "again when it is done"
                                      : "internal error");
        return;
    }
    cJSON *detail = cJSON_CreateObject();
    char *text = detail != NULL && cJSON_AddStringToObject(detail, "user", user) != NULL
                     ? cJSON_PrintUnformatted(detail)
                     : NULL;
    long long audit = text != NULL ? audit_begin(music_db, req, "navidrome-login", text) : -1;
    sqlite3_stmt *st = audit >= 0 ? db_prepare(music_db,
        "INSERT INTO navidrome_account (id, user, password) VALUES (1, ?, ?)"
        " ON CONFLICT (id) DO UPDATE SET user = excluded.user, password = excluded.password,"
        " saved = unixepoch()") : NULL;
    int ok = st != NULL && sqlite3_bind_text(st, 1, user, -1, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_bind_text(st, 2, password, -1, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_step(st) == SQLITE_DONE;
    if (st != NULL && !ok)
        db_log_error(music_db, "navidrome: keep the login");
    sqlite3_finalize(st);
    if (audit >= 0)
        audit_end(music_db, audit, ok ? "ok" : "failed: database error");
    music_unlock(lock);
    if (!ok) {
        json_error(res, 500, "internal error; the login was not kept");
        return;
    }
    cJSON *out = cJSON_CreateObject();
    if (out == NULL || cJSON_AddStringToObject(out, "user", user) == NULL) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, out);
}

/*
 * GET /api/music/navidrome/starred: the albums and songs starred in
 * Navidrome (at most 5000 each). An album: id, name, artist, year, songs
 * and the track of nylm's album with that name and album artist; a song
 * as in add_songs().
 */
void music_navidrome_starred(struct request *req, struct response *res)
{
    (void)req;
    if (!ready(res) || !has_login(res))
        return;
    cJSON *r = call(res, "getStarred2", "");
    if (r == NULL)
        return;
    const cJSON *starred = cJSON_GetObjectItemCaseSensitive(r, "starred2");
    cJSON *obj = cJSON_CreateObject();
    cJSON *albums = obj != NULL ? cJSON_AddArrayToObject(obj, "albums") : NULL;
    cJSON *songs = albums != NULL ? cJSON_AddArrayToObject(obj, "songs") : NULL;
    sqlite3_stmt *st = songs != NULL ? db_prepare(music_db,
        "SELECT id FROM tracks WHERE album = ? AND albumartist = ? ORDER BY path LIMIT 1")
                                     : NULL;
    int ok = st != NULL, n = 0;
    const cJSON *a;
    cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(starred, "album")) {
        if (!ok || n++ >= MAX_ROWS)
            break;
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(a, "name");
        const cJSON *artist = cJSON_GetObjectItemCaseSensitive(a, "artist");
        cJSON *row = cJSON_CreateObject();
        ok = row != NULL && cJSON_AddItemToArray(albums, row) &&
             add_text(row, "id", a, "id") != NULL && add_text(row, "name", a, "name") != NULL &&
             add_text(row, "artist", a, "artist") != NULL &&
             add_number(row, "year", a, "year") != NULL &&
             add_number(row, "songs", a, "songCount") != NULL &&
             (cJSON_IsString(name) && cJSON_IsString(artist)
                  ? add_track(row, st, name->valuestring, artist->valuestring) == 0
                  : cJSON_AddNullToObject(row, "track") != NULL);
    }
    sqlite3_finalize(st);
    if (!ok || add_songs(songs, cJSON_GetObjectItemCaseSensitive(starred, "song")) != 0) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, obj);
}

/* GET /api/music/navidrome/playlists: Navidrome's playlists (at most
 * 5000): id, name, songs, duration, owner. */
void music_navidrome_playlists(struct request *req, struct response *res)
{
    (void)req;
    if (!ready(res) || !has_login(res))
        return;
    cJSON *r = call(res, "getPlaylists", "");
    if (r == NULL)
        return;
    const cJSON *all = cJSON_GetObjectItemCaseSensitive(r, "playlists");
    cJSON *obj = cJSON_CreateObject();
    cJSON *list = obj != NULL ? cJSON_AddArrayToObject(obj, "playlists") : NULL;
    int ok = list != NULL, n = 0;
    const cJSON *p;
    cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(all, "playlist")) {
        if (!ok || n++ >= MAX_ROWS)
            break;
        cJSON *row = cJSON_CreateObject();
        ok = row != NULL && cJSON_AddItemToArray(list, row) &&
             add_text(row, "id", p, "id") != NULL && add_text(row, "name", p, "name") != NULL &&
             add_number(row, "songs", p, "songCount") != NULL &&
             add_number(row, "duration", p, "duration") != NULL &&
             add_text(row, "owner", p, "owner") != NULL;
    }
    if (!ok) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, obj);
}

/* GET /api/music/navidrome/playlist?id=ID: one playlist: id, name and its
 * songs (at most 5000), as in add_songs(). */
void music_navidrome_playlist(struct request *req, struct response *res)
{
    const char *id = NULL;
    if (http_query(req, "id", &id) != 0 || !navidrome_id_valid(id)) {
        json_error(res, 400, "query parameter 'id' must be a Navidrome playlist id "
                             "(1 to 64 of A-Z a-z 0-9 - _)");
        return;
    }
    if (!ready(res) || !has_login(res))
        return;
    char extra[NAVIDROME_MAX_ID + 8];
    snprintf(extra, sizeof extra, "&id=%s", id);
    cJSON *r = call(res, "getPlaylist", extra);
    if (r == NULL)
        return;
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(r, "playlist");
    cJSON *obj = cJSON_CreateObject();
    cJSON *songs = obj != NULL && add_text(obj, "id", p, "id") != NULL &&
                           add_text(obj, "name", p, "name") != NULL
                       ? cJSON_AddArrayToObject(obj, "songs")
                       : NULL;
    if (songs == NULL || add_songs(songs, cJSON_GetObjectItemCaseSensitive(p, "entry")) != 0) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, obj);
}
