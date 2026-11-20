/*
 * DAPs in the music app (src/dap.h): the USB drives with a FAT or exFAT
 * file system plugged into the server, the one mounted at DAP_MOUNT,
 * mounting and unmounting it (root actions dap-mount, dap-umount; both
 * recorded in the audit table), the names given to them (daps), what goes
 * on each (dap_items) and its syncs (dap_syncs, src/dapsync.h).
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>

#include "action.h"
#include "api.h"
#include "audit.h"
#include "dap.h"
#include "db.h"
#include "json.h"
#include "music.h"
#include "navidrome.h"
#include "sysinfo.h"

#define SYS_DIR    "/sys"
#define UDEV_DIR   "/run/udev/data"
#define MOUNTINFO  "/proc/self/mountinfo"
#define ID_MAX     9007199254740991LL /* 2^53 - 1: exact in a JSON number */
#define MAX_ITEMS  2000               /* albums, tracks and playlists on one DAP */
#define MAX_SYNCS  10                 /* syncs listed */

/* What is plugged in and mounted now. */
struct state {
    struct dap_drive drives[DAP_MAX];
    int n;
    int mounted;  /* index of the drive at DAP_MOUNT, or -1 */
    int other;    /* 1 if something nylm does not list is mounted there */
};

/* Reads the state; 0, or -1 after replying 500. */
static int look(struct state *s, struct response *res)
{
    s->n = dap_list(SYS_DIR, UDEV_DIR, MOUNTINFO, s->drives, DAP_MAX);
    s->mounted = -1;
    s->other = 0;
    const char *mounts = sysinfo_read_file(MOUNTINFO, 1024 * 1024);
    if (s->n < 0 || mounts == NULL) {
        json_error(res, 500, "can not read the drives (/sys, /proc); see the server log");
        return -1;
    }
    char dev[24];
    if (dap_device_at(mounts, DAP_MOUNT, dev, sizeof dev)) {
        for (int i = 0; i < s->n; i++)
            if (strcmp(s->drives[i].majmin, dev) == 0)
                s->mounted = i;
        s->other = s->mounted < 0;
    }
    return 0;
}

/* The drive with uuid, or NULL. */
static const struct dap_drive *find(const struct state *s, const char *uuid)
{
    for (int i = 0; i < s->n; i++)
        if (strcmp(s->drives[i].uuid, uuid) == 0)
            return &s->drives[i];
    return NULL;
}

/* 1 if no DAP sync runs; else replies 409 (500 on error) and returns 0. */
static int sync_idle(struct response *res)
{
    int lock = music_dap_lock(0);
    if (lock >= 0) {
        music_unlock(lock);
        return 1;
    }
    json_error(res, lock == MUSIC_BUSY ? 409 : 500,
               lock == MUSIC_BUSY ? "a DAP sync is running: try again when it is done"
                                  : "internal error");
    return 0;
}

/* Adds key: s, or null when it is empty. NULL when out of memory. */
static cJSON *add_text(cJSON *obj, const char *key, const char *s)
{
    return s[0] != '\0' ? cJSON_AddStringToObject(obj, key, s) : cJSON_AddNullToObject(obj, key);
}

/* A drive as JSON: dev, uuid, fstype, label, vendor, model, size, mounted
 * (where, or null), name (given in nylm, or null). NULL on error. */
static cJSON *drive_json(const struct dap_drive *d, sqlite3_stmt *name)
{
    cJSON *obj = cJSON_CreateObject();
    int rc = sqlite3_bind_text(name, 1, d->uuid, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK)
        rc = sqlite3_step(name);
    const char *given = rc == SQLITE_ROW ? (const char *)sqlite3_column_text(name, 0) : NULL;
    int ok = obj != NULL && (rc == SQLITE_ROW || rc == SQLITE_DONE) &&
             cJSON_AddStringToObject(obj, "dev", d->dev) != NULL &&
             cJSON_AddStringToObject(obj, "uuid", d->uuid) != NULL &&
             cJSON_AddStringToObject(obj, "fstype", d->fstype) != NULL &&
             add_text(obj, "label", d->label) != NULL && add_text(obj, "vendor", d->vendor) != NULL &&
             add_text(obj, "model", d->model) != NULL &&
             cJSON_AddNumberToObject(obj, "size", (double)d->size) != NULL &&
             add_text(obj, "mounted", d->mounted) != NULL &&
             (given != NULL ? cJSON_AddStringToObject(obj, "name", given)
                            : cJSON_AddNullToObject(obj, "name")) != NULL;
    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
        db_log_error(music_db, "dap: name");
    sqlite3_reset(name);
    return ok ? obj : NULL;
}

/*
 * GET /api/music/dap: the mount point, the USB drives with a FAT or exFAT
 * file system (drives, as drive_json() says), the one mounted at the mount
 * point (mounted: its drive, with free and total bytes; null if none),
 * whether something else is mounted there (other), whether a sync runs
 * (syncing) and the latest 10 syncs (syncs: id, uuid, requested, started,
 * finished, state, files, missing, copied, kept, deleted, failed, bytes,
 * total, note).
 */
void music_dap(struct request *req, struct response *res)
{
    (void)req;
    struct state s;
    if (look(&s, res) != 0)
        return;
    sqlite3_stmt *name = db_prepare(music_db, "SELECT name FROM daps WHERE uuid = ?");
    cJSON *obj = cJSON_CreateObject();
    cJSON *list = obj != NULL && name != NULL ? cJSON_AddArrayToObject(obj, "drives") : NULL;
    int ok = list != NULL && cJSON_AddStringToObject(obj, "mount", DAP_MOUNT) != NULL &&
             cJSON_AddBoolToObject(obj, "other", s.other) != NULL;
    for (int i = 0; ok && i < s.n; i++) {
        cJSON *d = drive_json(&s.drives[i], name);
        ok = d != NULL && cJSON_AddItemToArray(list, d);
    }
    if (ok && s.mounted >= 0) {
        cJSON *m = drive_json(&s.drives[s.mounted], name);
        struct statvfs fs;
        int got = statvfs(DAP_MOUNT, &fs) == 0;
        if (!got)
            perror("dap: statvfs " DAP_MOUNT);
        ok = m != NULL && cJSON_AddItemToObject(obj, "mounted", m) &&
             (got ? cJSON_AddNumberToObject(m, "free", (double)fs.f_bavail * (double)fs.f_frsize)
                  : cJSON_AddNullToObject(m, "free")) != NULL &&
             (got ? cJSON_AddNumberToObject(m, "total", (double)fs.f_blocks * (double)fs.f_frsize)
                  : cJSON_AddNullToObject(m, "total")) != NULL;
    } else if (ok) {
        ok = cJSON_AddNullToObject(obj, "mounted") != NULL;
    }
    sqlite3_finalize(name);
    int lock = ok ? music_dap_lock(0) : -1;
    music_unlock(lock);
    cJSON *syncs = lock != -1 && cJSON_AddBoolToObject(obj, "syncing", lock == MUSIC_BUSY) != NULL
                       ? cJSON_AddArrayToObject(obj, "syncs")
                       : NULL;
    sqlite3_stmt *st = syncs != NULL ? db_prepare(music_db,
        "SELECT id, uuid, requested, started, finished, state, files, missing, copied, kept,"
        " deleted, failed, bytes, total, note FROM dap_syncs ORDER BY id DESC LIMIT 10") : NULL;
    int rc = st != NULL ? SQLITE_ROW : SQLITE_ERROR;
    while (rc == SQLITE_ROW && (rc = sqlite3_step(st)) == SQLITE_ROW) {
        cJSON *row = json_row(st, 15);
        if (row == NULL || !cJSON_AddItemToArray(syncs, row))
            rc = SQLITE_NOMEM;
    }
    if (rc != SQLITE_DONE && st != NULL)
        db_log_error(music_db, "dap: syncs");
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, obj);
}

/* Reads body's "uuid" (a FAT or exFAT UUID); replies 400 and returns NULL
 * if it is not one. */
static const char *read_uuid(const cJSON *body, struct response *res)
{
    const char *uuid;
    if (json_get_string(body, "uuid", DAP_UUID_LEN, DAP_UUID_LEN, &uuid) != NULL ||
        !dap_uuid_valid(uuid)) {
        json_error(res, 400, "'uuid' must be a FAT or exFAT file system's UUID, like ABCD-1234");
        return NULL;
    }
    return uuid;
}

/* Runs a root action (dap-mount with uuid, or dap-umount) and records it
 * in the audit table. -> 200, or 500/502. */
static void run_action(struct request *req, struct response *res, const char *action,
                       const char *uuid)
{
    char detail[64] = "{}";
    if (uuid != NULL)
        snprintf(detail, sizeof detail, "{\"uuid\":\"%s\"}", uuid);
    long long audit = audit_begin(music_db, req, action, detail);
    if (audit < 0) {
        json_error(res, 500, "can not write the audit log; nothing was done");
        return;
    }
    int ok = action_run(action, uuid) == 0;
    audit_end(music_db, audit, ok ? "ok" : "failed: see the server log");
    if (!ok) {
        json_error(res, 502, uuid != NULL ? "could not mount it; see the server log"
                                          : "could not unmount it; see the server log");
        return;
    }
    json_reply(res, 200, cJSON_CreateObject());
}

/*
 * POST /api/music/dap/mount {uuid}: mounts the USB drive with that file
 * system at the mount point (root action dap-mount, which checks it all
 * again): it must be listed, not mounted, and the mount point free (409).
 */
void music_dap_mount(struct request *req, struct response *res)
{
    cJSON *body = json_body(req, res);
    const char *uuid = body != NULL ? read_uuid(body, res) : NULL;
    struct state s;
    if (uuid == NULL || look(&s, res) != 0)
        return;
    const struct dap_drive *d = find(&s, uuid);
    if (d == NULL) {
        json_error(res, 404, "no USB drive plugged in has a FAT or exFAT file system with this "
                             "UUID");
        return;
    }
    if (s.mounted >= 0 || s.other) {
        json_error(res, 409, s.mounted >= 0 && d == &s.drives[s.mounted]
                                 ? "it is mounted already"
                                 : "a drive is mounted already: unmount it first");
        return;
    }
    if (d->mounted[0] != '\0') {
        json_error(res, 409, "it is mounted somewhere else on the server: unmount it there first");
        return;
    }
    run_action(req, res, "dap-mount", d->uuid);
}

/* POST /api/music/dap/unmount {}: unmounts the drive at the mount point
 * (root action dap-umount; 409 if none is). */
void music_dap_unmount(struct request *req, struct response *res)
{
    struct state s;
    if (json_body(req, res) == NULL || !sync_idle(res) || look(&s, res) != 0)
        return;
    if (s.mounted < 0 && !s.other) {
        json_error(res, 409, "no drive is mounted at " DAP_MOUNT);
        return;
    }
    run_action(req, res, "dap-umount", NULL);
}

/* POST /api/music/dap/name {uuid, name}: the name nylm shows for a DAP (1
 * to 64 bytes of text), kept whether it is plugged in or not. */
void music_dap_name(struct request *req, struct response *res)
{
    cJSON *body = json_body(req, res);
    const char *uuid = body != NULL ? read_uuid(body, res) : NULL;
    const char *name;
    if (uuid == NULL)
        return;
    if (json_get_text(body, "name", 1, DAP_MAX_TEXT, 0, &name) != NULL) {
        json_error(res, 400, "'name' must be 1 to 64 bytes of text");
        return;
    }
    sqlite3_stmt *st = db_prepare(music_db,
        "INSERT INTO daps (uuid, name) VALUES (?, ?)"
        " ON CONFLICT (uuid) DO UPDATE SET name = excluded.name");
    int ok = st != NULL && sqlite3_bind_text(st, 1, uuid, -1, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_bind_text(st, 2, name, -1, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_step(st) == SQLITE_DONE;
    if (st != NULL && !ok)
        db_log_error(music_db, "dap: keep a name");
    sqlite3_finalize(st);
    if (!ok) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, cJSON_CreateObject());
}

/* ---- what goes on a DAP ---------------------------------------------------- */

/* Reads query parameter uuid; replies 400 and returns NULL if it is not one. */
static const char *query_uuid(const struct request *req, struct response *res)
{
    const char *uuid = NULL;
    if (http_query(req, "uuid", &uuid) != 0 || !dap_uuid_valid(uuid)) {
        json_error(res, 400, "query parameter 'uuid' must be a FAT or exFAT file system's UUID, "
                             "like ABCD-1234");
        return NULL;
    }
    return uuid;
}

/* Runs sql with ?1 = uuid and gives its first row's two numbers. 0 or -1. */
static int two_numbers(const char *sql, const char *uuid, long long *a, long long *b)
{
    sqlite3_stmt *st = db_prepare(music_db, sql);
    int ok = st != NULL && sqlite3_bind_text(st, 1, uuid, -1, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_step(st) == SQLITE_ROW;
    if (ok) {
        *a = sqlite3_column_int64(st, 0);
        *b = sqlite3_column_int64(st, 1);
    } else {
        db_log_error(music_db, "dap: count");
    }
    sqlite3_finalize(st);
    return ok ? 0 : -1;
}

/*
 * GET /api/music/dap/items?uuid=U: what goes on the DAP U, in the order it
 * was added: id, kind ("album", "track", "playlist"), album, albumartist
 * (an album's, a track's), title and artist (a track's), playlist and name
 * (a playlist's id and name), track (one of the album's tracks, or the
 * track; null if the library no longer has it), tracks and size (the
 * library's now). Then the tracks and size of the albums and tracks
 * together (a playlist's songs are known when it syncs).
 */
void music_dap_items(struct request *req, struct response *res)
{
    const char *uuid = query_uuid(req, res);
    if (uuid == NULL)
        return;
    sqlite3_stmt *st = db_prepare(music_db,
        "SELECT i.id, i.kind, coalesce(i.album, t.album) AS album,"
        "  coalesce(i.albumartist, t.albumartist) AS albumartist, t.title, t.artist,"
        "  i.playlist, i.name,"
        "  CASE i.kind WHEN 'track' THEN t.id WHEN 'album' THEN (SELECT min(a.id) FROM tracks a"
        "    WHERE a.album IS i.album AND a.albumartist IS i.albumartist) END AS track,"
        "  CASE i.kind WHEN 'track' THEN count(t.id) WHEN 'album' THEN (SELECT count(*)"
        "    FROM tracks a WHERE a.album IS i.album AND a.albumartist IS i.albumartist)"
        "    ELSE 0 END AS tracks,"
        "  CASE i.kind WHEN 'track' THEN coalesce(t.size, 0) WHEN 'album' THEN (SELECT"
        "    coalesce(sum(a.size), 0) FROM tracks a WHERE a.album IS i.album"
        "    AND a.albumartist IS i.albumartist) ELSE 0 END AS size"
        " FROM dap_items i LEFT JOIN tracks t ON i.kind = 'track' AND t.id = i.track_id"
        " WHERE i.uuid = ? GROUP BY i.id ORDER BY i.id");
    cJSON *obj = cJSON_CreateObject();
    cJSON *list = obj != NULL && st != NULL ? cJSON_AddArrayToObject(obj, "items") : NULL;
    int rc = list != NULL && sqlite3_bind_text(st, 1, uuid, -1, SQLITE_STATIC) == SQLITE_OK
                 ? SQLITE_ROW
                 : SQLITE_ERROR;
    while (rc == SQLITE_ROW && (rc = sqlite3_step(st)) == SQLITE_ROW) {
        cJSON *row = json_row(st, 11);
        if (row == NULL || !cJSON_AddItemToArray(list, row))
            rc = SQLITE_NOMEM;
    }
    if (rc != SQLITE_DONE)
        db_log_error(music_db, "dap: items");
    sqlite3_finalize(st);
    long long tracks = 0, size = 0;
    if (rc != SQLITE_DONE ||
        two_numbers("SELECT count(*), coalesce(sum(t.size), 0) FROM tracks t WHERE EXISTS"
                    " (SELECT 1 FROM dap_items i WHERE i.uuid = ?1 AND ((i.kind = 'album'"
                    " AND t.album IS i.album AND t.albumartist IS i.albumartist)"
                    " OR (i.kind = 'track' AND t.id = i.track_id)))",
                    uuid, &tracks, &size) != 0 ||
        cJSON_AddNumberToObject(obj, "tracks", (double)tracks) == NULL ||
        cJSON_AddNumberToObject(obj, "size", (double)size) == NULL) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, obj);
}

/*
 * POST /api/music/dap/items/add {uuid, album: TRACK} (the album of that
 * track, by its album and album artist in the files) | {uuid, track:
 * TRACK} | {uuid, playlist: ID, name} (a Navidrome playlist): adds it to
 * what goes on the DAP uuid (at most 2000). -> 200 {added: false when it
 * was there already}
 */
void music_dap_item_add(struct request *req, struct response *res)
{
    cJSON *body = json_body(req, res);
    const char *uuid = body != NULL ? read_uuid(body, res) : NULL;
    if (uuid == NULL)
        return;
    int is_album = cJSON_GetObjectItemCaseSensitive(body, "album") != NULL;
    int is_track = cJSON_GetObjectItemCaseSensitive(body, "track") != NULL;
    int is_playlist = cJSON_GetObjectItemCaseSensitive(body, "playlist") != NULL;
    if (is_album + is_track + is_playlist != 1) {
        json_error(res, 400, "give one of 'album' (a track of it), 'track' or 'playlist'");
        return;
    }
    long id = 0;
    const char *playlist = NULL, *name = NULL;
    if (!is_playlist &&
        json_get_int(body, is_album ? "album" : "track", 1, (long)ID_MAX, &id) != NULL) {
        json_error(res, 400, is_album ? "'album' must be the id of one of its tracks"
                                      : "'track' must be a track's id");
        return;
    }
    if (is_playlist && (json_get_string(body, "playlist", 1, NAVIDROME_MAX_ID, &playlist) != NULL ||
                        !navidrome_id_valid(playlist))) {
        json_error(res, 400, "'playlist' must be a Navidrome playlist id (1 to 64 of A-Z a-z 0-9 "
                             "- _)");
        return;
    }
    if (is_playlist && json_get_text(body, "name", 1, 200, 0, &name) != NULL) {
        json_error(res, 400, "'name' must be the playlist's name (1 to 200 bytes of text)");
        return;
    }
    if (is_playlist && navidrome_url() == NULL) {
        json_error(res, 503, "Navidrome is not set up: set NYLM_NAVIDROME in /etc/nylm.conf");
        return;
    }
    long long count = 0, found = 1;
    if (two_numbers("SELECT count(*), 1 FROM dap_items WHERE uuid = ?1", uuid, &count, &found) !=
        0) {
        json_error(res, 500, "internal error");
        return;
    }
    if (!is_playlist) {
        sqlite3_stmt *st = db_prepare(music_db, "SELECT count(*) FROM tracks WHERE id = ?");
        found = st != NULL && sqlite3_bind_int64(st, 1, id) == SQLITE_OK &&
                        sqlite3_step(st) == SQLITE_ROW
                    ? sqlite3_column_int64(st, 0)
                    : -1;
        sqlite3_finalize(st);
    }
    if (found < 0) {
        json_error(res, 500, "internal error");
        return;
    }
    if (found == 0) {
        json_error(res, 404, "track not found");
        return;
    }
    if (count >= MAX_ITEMS) {
        json_error(res, 409, "a DAP has at most 2000 albums, tracks and playlists: remove some");
        return;
    }
    sqlite3_stmt *st = db_prepare(music_db, is_album
        ? "INSERT OR IGNORE INTO dap_items (uuid, kind, album, albumartist)"
          " SELECT ?1, 'album', album, albumartist FROM tracks WHERE id = ?2"
        : is_track ? "INSERT OR IGNORE INTO dap_items (uuid, kind, track_id) VALUES (?1, 'track', ?2)"
                   : "INSERT OR IGNORE INTO dap_items (uuid, kind, playlist, name)"
                     " VALUES (?1, 'playlist', ?3, ?4)");
    int ok = st != NULL && sqlite3_bind_text(st, 1, uuid, -1, SQLITE_STATIC) == SQLITE_OK &&
             (is_playlist ? sqlite3_bind_text(st, 3, playlist, -1, SQLITE_STATIC) == SQLITE_OK &&
                                sqlite3_bind_text(st, 4, name, -1, SQLITE_STATIC) == SQLITE_OK
                          : sqlite3_bind_int64(st, 2, id) == SQLITE_OK) &&
             sqlite3_step(st) == SQLITE_DONE;
    if (st != NULL && !ok)
        db_log_error(music_db, "dap: add an item");
    sqlite3_finalize(st);
    cJSON *out = ok ? cJSON_CreateObject() : NULL;
    if (out == NULL || cJSON_AddBoolToObject(out, "added", sqlite3_changes(music_db) > 0) == NULL) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, out);
}

/* POST /api/music/dap/items/remove {id}: takes it off what goes on its
 * DAP (the next sync deletes its files there). -> 200 */
void music_dap_item_remove(struct request *req, struct response *res)
{
    cJSON *body = json_body(req, res);
    long id;
    if (body == NULL)
        return;
    if (json_get_int(body, "id", 1, (long)ID_MAX, &id) != NULL) {
        json_error(res, 400, "'id' must be the id of an album, track or playlist on a DAP");
        return;
    }
    sqlite3_stmt *st = db_prepare(music_db, "DELETE FROM dap_items WHERE id = ?");
    int ok = st != NULL && sqlite3_bind_int64(st, 1, id) == SQLITE_OK &&
             sqlite3_step(st) == SQLITE_DONE;
    if (st != NULL && !ok)
        db_log_error(music_db, "dap: remove an item");
    sqlite3_finalize(st);
    if (!ok) {
        json_error(res, 500, "internal error");
        return;
    }
    if (sqlite3_changes(music_db) == 0) {
        json_error(res, 404, "not on a DAP");
        return;
    }
    json_reply(res, 200, cJSON_CreateObject());
}

/*
 * POST /api/music/dap/sync {password}: queues a sync of the DAP mounted
 * now (it must be one nylm lists) and starts nylm-dap-sync.service (root
 * action dap-sync), which copies its selection and deletes what is not in
 * it, in DAP_FOLDER only. Not while a sync or a library service runs (409).
 * The password again; audited. -> 202
 */
void music_dap_sync_start(struct request *req, struct response *res)
{
    cJSON *body = json_body(req, res);
    if (body == NULL || !audit_password_ok(body, res))
        return;
    if (music_root() == NULL) {
        json_error(res, 503, "music is not set up: set NYLM_MUSIC in /etc/nylm.conf");
        return;
    }
    struct state s;
    if (look(&s, res) != 0)
        return;
    if (s.mounted < 0) {
        json_error(res, 409, "mount the DAP first");
        return;
    }
    const char *uuid = s.drives[s.mounted].uuid;
    int error;
    if (music_busy(&error) != NULL || error) {
        json_error(res, error ? 500 : 409, error ? "internal error"
                                                 : "the library is busy: a scan, write or move "
                                                   "is running; try again when it is done");
        return;
    }
    /* Held while the sync is queued and started: the service waits for it. */
    int lock = music_dap_lock(0);
    if (lock < 0) {
        json_error(res, lock == MUSIC_BUSY ? 409 : 500,
                   lock == MUSIC_BUSY ? "a DAP sync is running: try again when it is done"
                                      : "internal error");
        return;
    }
    char detail[64];
    snprintf(detail, sizeof detail, "{\"uuid\":\"%s\"}", uuid);
    /* One that never ran (the service could not start it) gives way. */
    int ok = db_exec(music_db, "UPDATE dap_syncs SET state = 'failed', finished = unixepoch(),"
                               " note = 'it did not start; a newer sync took its place'"
                               " WHERE state = 'queued'") == 0;
    sqlite3_stmt *st = ok ? db_prepare(music_db, "INSERT INTO dap_syncs (uuid) VALUES (?)") : NULL;
    ok = st != NULL && sqlite3_bind_text(st, 1, uuid, -1, SQLITE_STATIC) == SQLITE_OK &&
         sqlite3_step(st) == SQLITE_DONE;
    if (st != NULL && !ok)
        db_log_error(music_db, "dap: queue a sync");
    sqlite3_finalize(st);
    long long id = ok ? sqlite3_last_insert_rowid(music_db) : -1;
    long long audit = ok ? audit_begin(music_db, req, "dap-sync", detail) : -1;
    int started = audit >= 0 && action_run("dap-sync", NULL) == 0;
    if (audit >= 0)
        audit_end(music_db, audit, started ? "ok: started" : "failed: the action did not start it");
    if (id > 0 && !started) {
        st = db_prepare(music_db, "UPDATE dap_syncs SET state = 'failed', finished = unixepoch(),"
                                  " note = 'the service did not start' WHERE id = ?");
        if (st == NULL || sqlite3_bind_int64(st, 1, id) != SQLITE_OK ||
            sqlite3_step(st) != SQLITE_DONE)
            db_log_error(music_db, "dap: a sync that did not start");
        sqlite3_finalize(st);
    }
    music_unlock(lock);
    if (!started) {
        json_error(res, 500, audit < 0 ? "can not write the audit log; nothing was started"
                                       : "could not start it; see the server log");
        return;
    }
    json_reply(res, 202, cJSON_CreateObject());
}
