/*
 * The music library folder, its cache, and the two services that work on
 * the files (see music.h): the scan and the write. Each runs as its own
 * process (`nylm music-scan`, `nylm music-write`, by hand or as a systemd
 * service) so the single-threaded server never waits for them and never
 * opens a music file. The scan commits about once a second so its progress
 * shows in the web app.
 */
#define _POSIX_C_SOURCE 200809L

#include "music.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "arena.h"
#include "conn.h"
#include "db.h"
#include "json.h"

static char root[MUSIC_MAX_ROOT + 1];
static size_t root_len;
static char lock_path[DB_MAX_PATH];

int music_configure(const char *dir, const char *data_dir)
{
    root[0] = '\0';
    root_len = 0;
    int n = snprintf(lock_path, sizeof lock_path, "%s/music/library.lock", data_dir);
    if (n < 0 || (size_t)n >= sizeof lock_path) {
        fprintf(stderr, "music: data folder path is too long\n");
        return -1;
    }
    if (dir == NULL || *dir == '\0')
        return 0;
    size_t len = strlen(dir);
    while (len > 1 && dir[len - 1] == '/')
        len--;
    if (dir[0] != '/' || len < 2 || len > MUSIC_MAX_ROOT || !text_valid(dir, 0)) {
        fprintf(stderr, "NYLM_MUSIC must be the absolute path of the music folder (not /, "
                        "at most %d bytes)\n", MUSIC_MAX_ROOT);
        return -1;
    }
    memcpy(root, dir, len);
    root[len] = '\0';
    root_len = len;
    return 0;
}

const char *music_root(void)
{
    return root[0] != '\0' ? root : NULL;
}

int music_available(void)
{
    struct stat sb;
    if (stat(root, &sb) != 0) {
        fprintf(stderr, "music: folder %s: %s\n", root, strerror(errno));
        return 0;
    }
    if (!S_ISDIR(sb.st_mode)) {
        fprintf(stderr, "music: %s is not a folder\n", root);
        return 0;
    }
    return 1;
}

int music_inside(const char *path)
{
    size_t len = strlen(path);
    if (root_len == 0 || len >= TAGS_MAX_PATH || strncmp(path, root, root_len) != 0 ||
        !text_valid(path, 0))
        return 0;
    const char *p = path + root_len;
    if (*p == '\0')
        return 1;
    if (*p != '/')
        return 0;
    while (*p == '/') {
        p++;
        size_t n = strcspn(p, "/");
        if (n == 0 || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.'))
            return 0;
        p += n;
    }
    return 1;
}

/* Binds text, or NULL for a NULL pointer. */
static int bind_text(sqlite3_stmt *st, int i, const char *s)
{
    return s != NULL ? sqlite3_bind_text(st, i, s, -1, SQLITE_TRANSIENT)
                     : sqlite3_bind_null(st, i);
}

/* Runs a prepared statement once and resets it. 0 or -1 (logged). */
static int step_once(sqlite3_stmt *st)
{
    int rc = sqlite3_step(st);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW)
        db_log_error(music_db, sqlite3_sql(st));
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    return rc == SQLITE_DONE || rc == SQLITE_ROW ? 0 : -1;
}

/* Runs sql (no parameters) once with the id bound as ?1. 0 or -1. */
static int run_with_id(const char *sql, long long id)
{
    sqlite3_stmt *st = db_prepare(music_db, sql);
    int rc = st != NULL && sqlite3_bind_int64(st, 1, id) == SQLITE_OK ? step_once(st) : -1;
    sqlite3_finalize(st);
    return rc;
}

/* ---- the cache as struct tags ------------------------------------------- */

int music_values_parse(const char *json, struct tag_values *out)
{
    out->n = 0;
    out->v = NULL;
    cJSON *list = cJSON_Parse(json);
    if (!cJSON_IsArray(list))
        return -1;
    int n = cJSON_GetArraySize(list);
    if (n == 0)
        return 0;
    out->v = arena_alloc((size_t)n * sizeof *out->v);
    if (out->v == NULL)
        return -1;
    const cJSON *item;
    cJSON_ArrayForEach(item, list) {
        if (!cJSON_IsString(item))
            return -1;
        out->v[out->n++] = item->valuestring;
    }
    return 0;
}

const char *music_values_json(const struct tag_values *v)
{
    cJSON *list = cJSON_CreateArray();
    for (size_t i = 0; list != NULL && i < v->n; i++) {
        cJSON *s = cJSON_CreateString(v->v[i]);
        if (s == NULL || !cJSON_AddItemToArray(list, s))
            list = NULL; /* the arena frees what was built */
    }
    return list != NULL ? cJSON_PrintUnformatted(list) : NULL;
}

int music_track_tags(sqlite3_stmt *st, int col, struct tags *out)
{
    memset(out, 0, sizeof *out);
    out->has_art = sqlite3_column_int(st, col) != 0;
    for (int i = 0; i < TAG_FIELDS; i++) {
        int c = col + 1 + i;
        const char *s = (const char *)sqlite3_column_text(st, c);
        if (s == NULL)
            continue;
        const char *copy = arena_strndup(s, (size_t)sqlite3_column_bytes(st, c));
        if (copy == NULL)
            return -1;
        if (tags_is_multi((enum tag_field)i)) {
            if (music_values_parse(copy, &out->value[i]) != 0)
                return -1;
        } else if (tags_set_one(out, (enum tag_field)i, copy) != 0) {
            return -1;
        }
    }
    return 0;
}

/* ---- scan --------------------------------------------------------------- */

#define MAX_OTHER_EXT 200 /* extensions counted apart; the rest are "[more]" */

struct ext_count {
    char ext[TAGS_MAX_EXT + 1];
    long count;
};

struct scan {
    long long id;             /* the scans row; 0 for the writer's reads */
    long files, parsed, failed;
    int incomplete;           /* something could not be read: nothing is removed */
    double batch_start;
    struct ext_count other[MAX_OTHER_EXT + 1];
    int nother;
    sqlite3_stmt *find, *seen, *mark_seen, *upsert, *clear_values, *add_value;
};

/* Prepares the statements a scan uses, and the table of the tracks it saw
 * (temporary: this connection's own). 0 or -1. */
static int scan_prepare(struct scan *s)
{
    if (db_exec(music_db, "CREATE TEMP TABLE IF NOT EXISTS seen (id INTEGER PRIMARY KEY)") != 0)
        return -1;
    s->find = db_prepare(music_db, "SELECT id, size, scanned FROM tracks WHERE path = ?");
    s->seen = db_prepare(music_db, "UPDATE tracks SET scanned = ? WHERE id = ?");
    s->mark_seen = db_prepare(music_db, "INSERT OR IGNORE INTO temp.seen (id) VALUES (?)");
    s->upsert = db_prepare(music_db,
        "INSERT INTO tracks (path, size, ext, scanned, has_art, title, album, artist,"
        "  albumartist, tracknumber, discnumber, date, compilation, isrc, asin, bpm,"
        "  copyright, encodedby, mood, media, label, catalognumber, barcode, titlesort,"
        "  albumsort, artistsort, albumartistsort, composersort, musicbrainz_trackid,"
        "  musicbrainz_albumid, navidrome_id)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,"
        "  ?, ?, ?, ?, ?)"
        " ON CONFLICT (path) DO UPDATE SET size = excluded.size, ext = excluded.ext,"
        "  scanned = excluded.scanned, has_art = excluded.has_art, title = excluded.title,"
        "  album = excluded.album, artist = excluded.artist,"
        "  albumartist = excluded.albumartist, tracknumber = excluded.tracknumber,"
        "  discnumber = excluded.discnumber, date = excluded.date,"
        "  compilation = excluded.compilation, isrc = excluded.isrc, asin = excluded.asin,"
        "  bpm = excluded.bpm, copyright = excluded.copyright,"
        "  encodedby = excluded.encodedby, mood = excluded.mood, media = excluded.media,"
        "  label = excluded.label, catalognumber = excluded.catalognumber,"
        "  barcode = excluded.barcode, titlesort = excluded.titlesort,"
        "  albumsort = excluded.albumsort, artistsort = excluded.artistsort,"
        "  albumartistsort = excluded.albumartistsort, composersort = excluded.composersort,"
        "  musicbrainz_trackid = excluded.musicbrainz_trackid,"
        "  musicbrainz_albumid = excluded.musicbrainz_albumid,"
        "  navidrome_id = excluded.navidrome_id"
        " RETURNING id");
    s->clear_values = db_prepare(music_db, "DELETE FROM track_values WHERE track_id = ?");
    s->add_value = db_prepare(music_db,
        "INSERT INTO track_values (track_id, field, position, value) VALUES (?, ?, ?, ?)");
    return s->find && s->seen && s->mark_seen && s->upsert && s->clear_values && s->add_value
               ? 0
               : -1;
}

static void scan_finalize(struct scan *s)
{
    sqlite3_finalize(s->find);
    sqlite3_finalize(s->seen);
    sqlite3_finalize(s->mark_seen);
    sqlite3_finalize(s->upsert);
    sqlite3_finalize(s->clear_values);
    sqlite3_finalize(s->add_value);
}

/* Records the counts of the running scan. 0 or -1 (logged). */
static int scan_progress(const struct scan *s)
{
    if (s->id == 0)
        return 0;
    sqlite3_stmt *st = db_prepare(music_db,
        "UPDATE scans SET files = ?, parsed = ?, failed = ? WHERE id = ?");
    int ok = st != NULL && sqlite3_bind_int64(st, 1, s->files) == SQLITE_OK &&
             sqlite3_bind_int64(st, 2, s->parsed) == SQLITE_OK &&
             sqlite3_bind_int64(st, 3, s->failed) == SQLITE_OK &&
             sqlite3_bind_int64(st, 4, s->id) == SQLITE_OK && step_once(st) == 0;
    sqlite3_finalize(st);
    return ok ? 0 : -1;
}

/* Commits what was done so far (with the counts) and starts a new
 * transaction, about once a second. 0 or -1. */
static int maybe_commit(struct scan *s)
{
    if (now_seconds() - s->batch_start < 1.0)
        return 0;
    if (scan_progress(s) != 0 || db_exec(music_db, "COMMIT") != 0 ||
        db_exec(music_db, "BEGIN IMMEDIATE") != 0)
        return -1;
    s->batch_start = now_seconds();
    return 0;
}

/* Stores the tags read from the file at path as its cache row; its id in
 * *out. 0 or -1. */
static int store_track(struct scan *s, const char *path, const char *ext,
                       const struct stat *sb, long long now, const struct tags *t,
                       long long *out)
{
    sqlite3_stmt *st = s->upsert;
    int rc = bind_text(st, 1, path);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(st, 2, (sqlite3_int64)sb->st_size);
    if (rc == SQLITE_OK)
        rc = bind_text(st, 3, ext);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(st, 4, now);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int(st, 5, t->has_art);
    for (int i = 0; i < TAG_SINGLE_FIELDS && rc == SQLITE_OK; i++)
        rc = bind_text(st, 6 + i, t->value[i].n > 0 ? t->value[i].v[0] : NULL);
    long long id = -1;
    if (rc == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        id = sqlite3_column_int64(st, 0);
    if (id < 0)
        db_log_error(music_db, "store track");
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    if (id < 0 || sqlite3_bind_int64(s->clear_values, 1, id) != SQLITE_OK ||
        step_once(s->clear_values) != 0)
        return -1;
    for (int f = TAG_SINGLE_FIELDS; f < TAG_FIELDS; f++) {
        for (size_t k = 0; k < t->value[f].n; k++) {
            st = s->add_value;
            if (sqlite3_bind_int64(st, 1, id) != SQLITE_OK ||
                sqlite3_bind_text(st, 2, tags_name[f], -1, SQLITE_STATIC) != SQLITE_OK ||
                sqlite3_bind_int64(st, 3, (sqlite3_int64)k) != SQLITE_OK ||
                bind_text(st, 4, t->value[f].v[k]) != SQLITE_OK || step_once(st) != 0)
                return -1;
        }
    }
    *out = id;
    return 0;
}

/*
 * One music file. Its tags are read unless force is 0 and the cache has it
 * with the same size, scanned after its last change (ctime). 0, or -1 on a
 * database error (the scan stops); an unreadable file is counted and
 * logged, and its cache row is left for the removal to drop. The tags read
 * stay in the arena: the caller resets it.
 */
static int scan_file(struct scan *s, const char *path, const char *ext, const struct stat *sb,
                     int force)
{
    s->files++;
    long long now = (long long)time(NULL);
    if (bind_text(s->find, 1, path) != SQLITE_OK)
        return -1;
    int rc = sqlite3_step(s->find);
    long long id = rc == SQLITE_ROW ? sqlite3_column_int64(s->find, 0) : 0;
    int unchanged = rc == SQLITE_ROW && sqlite3_column_int64(s->find, 1) == sb->st_size &&
                    (long long)sb->st_ctim.tv_sec < sqlite3_column_int64(s->find, 2);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
        db_log_error(music_db, "scan lookup");
    sqlite3_reset(s->find);
    sqlite3_clear_bindings(s->find);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
        return -1;

    if (unchanged && !force) {
        if (sqlite3_bind_int64(s->seen, 1, now) != SQLITE_OK ||
            sqlite3_bind_int64(s->seen, 2, id) != SQLITE_OK || step_once(s->seen) != 0)
            return -1;
    } else {
        char err[128];
        struct tags t;
        if (tags_read(path, &t, err, sizeof err) != 0) {
            fprintf(stderr, "music: skipped %s: %s\n", path, err);
            s->failed++;
            return 0;
        }
        if (store_track(s, path, ext, sb, now, &t, &id) != 0)
            return -1;
        s->parsed++;
    }
    if (sqlite3_bind_int64(s->mark_seen, 1, id) != SQLITE_OK || step_once(s->mark_seen) != 0)
        return -1;
    if (s->files % 1000 == 0) {
        printf("music: %ld files, %ld read\n", s->files, s->parsed);
        fflush(stdout);
    }
    return maybe_commit(s);
}

/* Counts a file that is not music under its extension; once there are
 * MAX_OTHER_EXT of them, new ones go under "[more]" (the extra slot). */
static void count_other(struct scan *s, const char *ext)
{
    if (s->nother >= MAX_OTHER_EXT)
        ext = "[more]";
    for (int i = 0; i < s->nother; i++) {
        if (strcmp(s->other[i].ext, ext) == 0) {
            s->other[i].count++;
            return;
        }
    }
    snprintf(s->other[s->nother].ext, sizeof s->other[s->nother].ext, "%s", ext);
    s->other[s->nother++].count = 1;
}

/*
 * A file name's extension, lower case, into out (TAGS_MAX_EXT + 1 bytes):
 * "[blank]" if it has none, "[long]" if it is longer than TAGS_MAX_EXT.
 */
static void extension_of(const char *name, char *out)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL || dot == name || dot[1] == '\0') {
        snprintf(out, TAGS_MAX_EXT + 1, "[blank]");
        return;
    }
    size_t len = strlen(dot + 1);
    if (len > TAGS_MAX_EXT) {
        snprintf(out, TAGS_MAX_EXT + 1, "[long]");
        return;
    }
    for (size_t i = 0; i <= len; i++) {
        char c = dot[1 + i];
        out[i] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
    }
}

/* A folder or file that can not be read: nothing is removed, as what is
 * missing from the scan may still be there. */
static void unreadable(struct scan *s, const char *path, const char *why)
{
    fprintf(stderr, "music: can not read %s: %s\n", path, why);
    s->failed++;
    s->incomplete = 1;
}

/*
 * Scans the folder path (a buffer of TAGS_MAX_PATH bytes, used for the
 * paths below it and restored). 0, or -1 on a database error.
 */
static int walk(struct scan *s, char *path, size_t len, int depth)
{
    DIR *d = opendir(path);
    if (d == NULL) {
        unreadable(s, path, strerror(errno));
        return 0;
    }
    int rc = 0;
    while (rc == 0) {
        errno = 0;
        struct dirent *de = readdir(d);
        if (de == NULL) {
            if (errno != 0)
                unreadable(s, path, strerror(errno));
            break;
        }
        const char *name = de->d_name;
        if (name[0] == '.')
            continue; /* hidden files and folders, "." and ".." */
        size_t n = strlen(name);
        if (len + 1 + n >= TAGS_MAX_PATH || !text_valid(name, 0)) {
            fprintf(stderr, "music: skipped %s/%s: name is not UTF-8 text or path too long\n",
                    path, name);
            s->failed++;
            s->incomplete = 1;
            continue;
        }
        path[len] = '/';
        memcpy(path + len + 1, name, n + 1);
        struct stat sb;
        if (fstatat(dirfd(d), name, &sb, AT_SYMLINK_NOFOLLOW) != 0) {
            unreadable(s, path, strerror(errno));
        } else if (S_ISDIR(sb.st_mode)) {
            if (depth == MUSIC_MAX_DEPTH)
                unreadable(s, path, "too deep");
            else
                rc = walk(s, path, len + 1 + n, depth + 1);
        } else if (S_ISREG(sb.st_mode)) {
            char ext[TAGS_MAX_EXT + 1];
            extension_of(name, ext);
            if (tags_is_music(ext)) {
                rc = scan_file(s, path, ext, &sb, 0);
                arena_reset(); /* the file's tags */
            } else {
                count_other(s, ext);
            }
        } /* symlinks and special files are not followed or counted */
        path[len] = '\0';
    }
    closedir(d);
    return rc;
}

/* Folder levels of path below the library folder. */
static int depth_of(const char *path)
{
    int depth = 0;
    for (const char *p = path + root_len; *p != '\0'; p++)
        depth += *p == '/';
    return depth;
}

/*
 * Scans path (music_inside()): the library folder, a folder (new and
 * changed files) or a file (read again) in it, noting each track seen for
 * remove_missing(). A path that is not there is left for the removal. 0,
 * or -1 on a database error.
 */
static int scan_path(struct scan *s, const char *path)
{
    if (db_exec(music_db, "DELETE FROM temp.seen") != 0)
        return -1;
    struct stat sb;
    int is_root = strcmp(path, root) == 0; /* the library folder may be a symlink */
    if ((is_root ? stat(path, &sb) : lstat(path, &sb)) != 0) {
        if (errno != ENOENT)
            unreadable(s, path, strerror(errno));
        return 0;
    }
    if (S_ISDIR(sb.st_mode)) {
        char buf[TAGS_MAX_PATH];
        snprintf(buf, sizeof buf, "%s", path); /* music_inside(): it fits */
        return walk(s, buf, strlen(buf), depth_of(path));
    }
    if (!S_ISREG(sb.st_mode)) {
        unreadable(s, path, "not a file or folder");
        return 0;
    }
    char ext[TAGS_MAX_EXT + 1];
    const char *slash = strrchr(path, '/');
    extension_of(slash != NULL ? slash + 1 : path, ext);
    if (!tags_is_music(ext)) {
        fprintf(stderr, "music: %s is not a music file\n", path);
        s->failed++;
        return 0;
    }
    return scan_file(s, path, ext, &sb, 1);
}

/* Drops the cached tracks at or below path (NULL: the whole library) that
 * the last scan_path() did not see. Their count, or -1. */
static long remove_missing(const char *path)
{
    sqlite3_stmt *st = db_prepare(music_db, path == NULL
        ? "DELETE FROM tracks WHERE id NOT IN (SELECT id FROM temp.seen)"
        : "DELETE FROM tracks WHERE id NOT IN (SELECT id FROM temp.seen)"
          " AND (path = ?1 OR substr(path, 1, length(?1) + 1) = ?1 || '/')");
    int rc = st != NULL ? SQLITE_OK : SQLITE_ERROR;
    if (rc == SQLITE_OK && path != NULL)
        rc = bind_text(st, 1, path);
    rc = rc == SQLITE_OK ? step_once(st) : -1;
    sqlite3_finalize(st);
    return rc == 0 ? (long)sqlite3_changes(music_db) : -1;
}

/* Records the end of a scan: its counts, state and the other extensions.
 * 0 or -1. */
static int scan_finished(const struct scan *s, long removed)
{
    sqlite3_stmt *st = db_prepare(music_db,
        "UPDATE scans SET files = ?, parsed = ?, removed = ?, failed = ?, state = ?,"
        " finished = unixepoch() WHERE id = ?");
    int ok = st != NULL && sqlite3_bind_int64(st, 1, s->files) == SQLITE_OK &&
             sqlite3_bind_int64(st, 2, s->parsed) == SQLITE_OK &&
             sqlite3_bind_int64(st, 3, removed) == SQLITE_OK &&
             sqlite3_bind_int64(st, 4, s->failed) == SQLITE_OK &&
             sqlite3_bind_text(st, 5, s->incomplete ? "incomplete" : "done", -1,
                               SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_bind_int64(st, 6, s->id) == SQLITE_OK && step_once(st) == 0;
    sqlite3_finalize(st);
    st = ok ? db_prepare(music_db, "INSERT INTO scan_extensions (scan_id, ext, count)"
                                   " VALUES (?, ?, ?)") : NULL;
    for (int i = 0; st != NULL && ok && i < s->nother; i++)
        ok = sqlite3_bind_int64(st, 1, s->id) == SQLITE_OK &&
             bind_text(st, 2, s->other[i].ext) == SQLITE_OK &&
             sqlite3_bind_int64(st, 3, s->other[i].count) == SQLITE_OK && step_once(st) == 0;
    sqlite3_finalize(st);
    return ok ? 0 : -1;
}

/*
 * Runs the queued scan id of path (NULL: the whole library). 0 done, 1
 * done but incomplete (something could not be read: nothing removed), -1
 * on a database error (recorded as failed).
 */
static int run_scan(struct scan *s, long long id, const char *path)
{
    s->id = id;
    s->files = s->parsed = s->failed = 0;
    s->incomplete = 0;
    s->nother = 0;
    printf("music: scanning %s\n", path != NULL ? path : root);
    fflush(stdout);
    if (run_with_id("UPDATE scans SET state = 'running', started = unixepoch() WHERE id = ?",
                    id) != 0)
        return -1;
    s->batch_start = now_seconds();
    int rc = db_exec(music_db, "BEGIN IMMEDIATE");
    if (rc == 0)
        rc = scan_path(s, path != NULL ? path : root);
    long removed = 0;
    if (rc == 0 && !s->incomplete && (removed = remove_missing(path)) < 0)
        rc = -1;
    if (rc == 0 && (scan_finished(s, removed) != 0 || db_exec(music_db, "COMMIT") != 0))
        rc = -1;
    if (rc != 0) {
        if (sqlite3_get_autocommit(music_db) == 0)
            db_exec(music_db, "ROLLBACK");
        run_with_id("UPDATE scans SET state = 'failed', finished = unixepoch() WHERE id = ?",
                    id);
        fprintf(stderr, "music: scan failed (database error)\n");
        return -1;
    }
    printf("music: %ld files, %ld read, %ld removed, %ld failed%s\n", s->files, s->parsed,
           removed, s->failed,
           s->incomplete ? " (something could not be read: nothing was removed)" : "");
    return s->incomplete ? 1 : 0;
}

/* ---- the library lock --------------------------------------------------- */

#define LOCK_WAIT_SECONDS 3 /* a service waits this long for the server's writes */

/* Opens the lock file; -1 on error (logged). */
static int open_lock(void)
{
    int fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        fprintf(stderr, "music: %s: %s\n", lock_path, strerror(errno));
    return fd;
}

/*
 * Takes the lock for a service ("scan" or "write") and writes its name in
 * the file. Waits a moment for a write of the server to end; fails if a
 * service runs. The fd (closing it releases the lock), or -1 (logged).
 */
static int lock_service(const char *name)
{
    int fd = open_lock();
    if (fd < 0)
        return -1;
    const struct timespec tick = { 0, 100 * 1000 * 1000 };
    int locked = 0;
    for (int i = 0; i < LOCK_WAIT_SECONDS * 10 && !locked; i++) {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0)
            locked = 1;
        else if (errno == EWOULDBLOCK)
            nanosleep(&tick, NULL);
        else
            break;
    }
    if (!locked) {
        fprintf(stderr, "music: %s\n", errno == EWOULDBLOCK
                                           ? "the library is busy: a scan or write is running"
                                           : strerror(errno));
        close(fd);
        return -1;
    }
    size_t len = strlen(name);
    if (ftruncate(fd, 0) != 0 || pwrite(fd, name, len, 0) != (ssize_t)len) {
        fprintf(stderr, "music: can not write %s: %s\n", lock_path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

int music_lock_shared(void)
{
    int fd = open_lock();
    if (fd < 0)
        return -1;
    if (flock(fd, LOCK_SH | LOCK_NB) == 0)
        return fd;
    int busy = errno == EWOULDBLOCK;
    if (!busy)
        fprintf(stderr, "music: flock %s: %s\n", lock_path, strerror(errno));
    close(fd);
    return busy ? MUSIC_BUSY : -1;
}

void music_unlock(int fd)
{
    if (fd >= 0)
        close(fd);
}

const char *music_busy(int *error)
{
    *error = 0;
    int fd = music_lock_shared();
    if (fd >= 0) {
        music_unlock(fd);
        return NULL;
    }
    if (fd != MUSIC_BUSY) {
        *error = 1;
        return NULL;
    }
    /* The service writes its name right after locking; until then "busy". */
    char name[16] = "";
    fd = open_lock();
    ssize_t n = fd >= 0 ? pread(fd, name, sizeof name - 1, 0) : -1;
    if (fd >= 0)
        close(fd);
    name[n > 0 ? n : 0] = '\0';
    return strcmp(name, "scan") == 0 ? "scan" : strcmp(name, "write") == 0 ? "write" : "busy";
}

/* Takes the lock for a service once the library is set up and there. The
 * fd, or -1 (logged). */
static int start_service(const char *name)
{
    if (root[0] == '\0') {
        fprintf(stderr, "music: NYLM_MUSIC is not set\n");
        return -1;
    }
    int lock = lock_service(name);
    if (lock >= 0 && !music_available()) {
        close(lock);
        return -1;
    }
    return lock;
}

/* ---- scan, continued ---------------------------------------------------- */

/* Queues a scan of path (NULL: the whole library). 0 or -1. */
static int queue_scan(const char *path)
{
    sqlite3_stmt *st = db_prepare(music_db, "INSERT INTO scans (path) VALUES (?)");
    int rc = st != NULL && bind_text(st, 1, path) == SQLITE_OK ? step_once(st) : -1;
    sqlite3_finalize(st);
    return rc;
}

/* The oldest queued scan: 1 with its id and path ("" for the whole
 * library), 0 if none, -1 on error. */
static int next_scan(long long *id, char *path)
{
    sqlite3_stmt *st = db_prepare(music_db,
        "SELECT id, path FROM scans WHERE state = 'queued' ORDER BY id LIMIT 1");
    int rc = st != NULL ? sqlite3_step(st) : SQLITE_ERROR;
    int found = rc == SQLITE_ROW;
    if (found) {
        const char *p = (const char *)sqlite3_column_text(st, 1);
        *id = sqlite3_column_int64(st, 0);
        snprintf(path, TAGS_MAX_PATH, "%s", p != NULL ? p : "");
    } else if (rc != SQLITE_DONE) {
        db_log_error(music_db, "next scan");
    }
    sqlite3_finalize(st);
    return found ? 1 : rc == SQLITE_DONE ? 0 : -1;
}

int music_scan(const char *arg)
{
    if (arg != NULL && !music_inside(arg)) {
        fprintf(stderr, "music: %s is not the music folder (NYLM_MUSIC) or a path in it\n",
                arg);
        return 1;
    }
    int lock = start_service("scan");
    if (lock < 0)
        return 1;

    /* A scan still "running" was cut off (the lock says none runs now). */
    int rc = db_exec(music_db, "UPDATE scans SET state = 'failed', finished = unixepoch()"
                               " WHERE state = 'running'");
    long long queued = 0;
    if (rc == 0 && arg == NULL) {
        sqlite3_stmt *st = db_prepare(music_db,
            "SELECT count(*) FROM scans WHERE state = 'queued'");
        rc = st != NULL && sqlite3_step(st) == SQLITE_ROW ? 0 : -1;
        queued = rc == 0 ? sqlite3_column_int64(st, 0) : 0;
        sqlite3_finalize(st);
    }
    if (rc == 0 && (arg != NULL || queued == 0))
        rc = queue_scan(arg != NULL && strcmp(arg, root) != 0 ? arg : NULL);

    struct scan s;
    memset(&s, 0, sizeof s);
    int failed = rc != 0 || scan_prepare(&s) != 0;
    while (!failed) {
        long long id;
        char path[TAGS_MAX_PATH];
        int found = next_scan(&id, path);
        if (found <= 0) {
            failed = found < 0;
            break;
        }
        if (path[0] != '\0' && !music_inside(path)) {
            fprintf(stderr, "music: queued scan %lld: %s is not in the music folder\n", id,
                    path);
            run_with_id("UPDATE scans SET state = 'failed', finished = unixepoch()"
                        " WHERE id = ?", id);
            failed = 1;
            continue;
        }
        int r = run_scan(&s, id, path[0] != '\0' ? path : NULL);
        arena_reset();
        if (r != 0)
            failed = 1; /* incomplete or failed; the next queued scan still runs */
        if (r < 0)
            break;
    }
    scan_finalize(&s);
    close(lock); /* releases the lock */
    return failed ? 1 : 0;
}

/* ---- write -------------------------------------------------------------- */

struct write_counts {
    long tracks, done, warnings, failed;
};

/* Records the result of the track's running changes. 0 or -1 (logged). */
static int record_result(long long track_id, const char *state, const char *note)
{
    sqlite3_stmt *st = db_prepare(music_db,
        "UPDATE changes SET state = ?, done = 1, finished = unixepoch(), note = ?"
        " WHERE state = 'running' AND track_id = ?");
    int rc = st != NULL && sqlite3_bind_text(st, 1, state, -1, SQLITE_STATIC) == SQLITE_OK &&
                     sqlite3_bind_text(st, 2, note, -1, SQLITE_STATIC) == SQLITE_OK &&
                     sqlite3_bind_int64(st, 3, track_id) == SQLITE_OK
                 ? step_once(st)
                 : -1;
    sqlite3_finalize(st);
    return rc;
}

/* The track's path and cached tags into the arena. 0, or -1 (logged). */
static int load_track(long long track_id, const char **path, struct tags *now)
{
    sqlite3_stmt *st = db_prepare(music_db,
        "SELECT t.path, " MUSIC_TAG_COLUMNS " FROM tracks t WHERE t.id = ?");
    int ok = st != NULL && sqlite3_bind_int64(st, 1, track_id) == SQLITE_OK &&
             sqlite3_step(st) == SQLITE_ROW;
    if (ok) {
        *path = arena_strndup((const char *)sqlite3_column_text(st, 0),
                              (size_t)sqlite3_column_bytes(st, 0));
        ok = *path != NULL && music_track_tags(st, 1, now) == 0;
    }
    if (!ok)
        db_log_error(music_db, "write: load track");
    sqlite3_finalize(st);
    return ok ? 0 : -1;
}

/*
 * Applies the track's pending changes to want and marks them running.
 * *changed: their fields' bits; *rows: how many. 0, or -1 on a database
 * error; a change whose value can not be read leaves *bad set.
 */
static int apply_pending(long long track_id, struct tags *want, unsigned *changed, long *rows,
                         int *bad)
{
    *changed = 0;
    *rows = 0;
    *bad = 0;
    if (run_with_id("UPDATE changes SET state = 'running', started = unixepoch()"
                    " WHERE state = 'pending' AND track_id = ?", track_id) != 0)
        return -1;
    sqlite3_stmt *st = db_prepare(music_db,
        "SELECT field, value FROM changes WHERE state = 'running' AND track_id = ?");
    if (st == NULL || sqlite3_bind_int64(st, 1, track_id) != SQLITE_OK) {
        sqlite3_finalize(st);
        return -1;
    }
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        (*rows)++;
        int f = tags_field_of((const char *)sqlite3_column_text(st, 0));
        const char *value = arena_strndup((const char *)sqlite3_column_text(st, 1),
                                          (size_t)sqlite3_column_bytes(st, 1));
        if (f < 0 || value == NULL) {
            *bad = 1;
            continue;
        }
        if (tags_is_multi((enum tag_field)f)
                ? music_values_parse(value, &want->value[f]) != 0
                : tags_set_one(want, (enum tag_field)f, value) != 0)
            *bad = 1;
        *changed |= 1u << f;
    }
    if (rc != SQLITE_DONE)
        db_log_error(music_db, "write: changes");
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* Lists in note what is wrong with want ("DATE is required; ..."). 1 if
 * anything is. */
static int check_all(const struct tags *want, char *note, size_t size)
{
    int bad = 0;
    for (int i = 0; i < TAG_FIELDS; i++) {
        const char *why = tags_check((enum tag_field)i, &want->value[i]);
        if (why == NULL)
            continue;
        size_t len = strlen(note);
        if (len < size)
            snprintf(note + len, size - len, "%s%s %s", bad ? "; " : "not written: ",
                     tags_key[i], why);
        bad = 1;
    }
    return bad;
}

/*
 * Writes one track's pending changes (all at once) and records the result:
 * failed with the reason if the file no longer matches the cache, or a tag
 * would be invalid; done, or warning (what nylm also changed, e.g. a disc
 * number set to 1/1); failed if the file does not read back as written.
 * Then reads the file back into the cache. 0, or -1 on a database error
 * (the service stops).
 */
static int write_track(struct scan *s, long long track_id, struct write_counts *n)
{
    const char *path;
    struct tags now, want;
    unsigned changed;
    long rows;
    int bad;
    if (load_track(track_id, &path, &now) != 0)
        return -1;
    want = now;
    if (apply_pending(track_id, &want, &changed, &rows, &bad) != 0)
        return -1;

    char note[TAGS_MAX_NOTE] = "", warning[TAGS_MAX_NOTE] = "";
    const char *state = "failed";
    enum tags_result result = TAGS_NOT_WRITTEN;
    if (bad)
        snprintf(note, sizeof note, "not written: a change in the queue can not be read");
    else if (!music_inside(path))
        snprintf(note, sizeof note, "not written: the file is not in the music folder");
    else if (tags_prepare(&want, changed, warning, sizeof warning) != 0)
        snprintf(note, sizeof note, "not written: out of memory");
    else if (!check_all(&want, note, sizeof note)) {
        result = tags_write(path, &now, &want, note, sizeof note);
        if (result == TAGS_WRITTEN) {
            state = warning[0] != '\0' ? "warning" : "done";
            snprintf(note, sizeof note, "%s", warning);
        }
    }

    n->tracks += result != TAGS_NOT_WRITTEN;
    long *count = strcmp(state, "done") == 0 ? &n->done
                : strcmp(state, "warning") == 0 ? &n->warnings : &n->failed;
    *count += rows;
    if (strcmp(state, "done") != 0)
        fprintf(stderr, "music: %s: %s\n", path, note);

    /* The result and the file as it is now, together. */
    s->batch_start = now_seconds();
    s->incomplete = 0;
    int inside = music_inside(path);
    int rc = db_exec(music_db, "BEGIN IMMEDIATE");
    if (rc == 0)
        rc = record_result(track_id, state, note);
    if (rc == 0 && inside)
        rc = scan_path(s, path);
    if (rc == 0 && inside && !s->incomplete && remove_missing(path) < 0)
        rc = -1;
    if (rc == 0)
        rc = db_exec(music_db, "COMMIT");
    if (rc != 0 && sqlite3_get_autocommit(music_db) == 0)
        db_exec(music_db, "ROLLBACK");
    return rc;
}

int music_write(void)
{
    int lock = start_service("write");
    if (lock < 0)
        return 1; /* the changes stay pending */

    struct write_counts n = { 0 };
    /* Changes still "running" were cut off (the lock says none runs now):
     * the file may or may not have them. */
    int rc = db_exec(music_db,
        "UPDATE changes SET state = 'failed', done = 1, finished = unixepoch(),"
        " note = 'the write was cut off; check the file and scan it again'"
        " WHERE state = 'running'");
    if (rc == 0)
        rc = db_exec(music_db,
            "UPDATE changes SET state = 'failed', done = 1, started = unixepoch(),"
            " finished = unixepoch(), note = 'the track is no longer in the library'"
            " WHERE state = 'pending' AND track_id IS NULL");
    n.failed += rc == 0 ? sqlite3_changes(music_db) : 0;

    struct scan s;
    memset(&s, 0, sizeof s);
    if (rc == 0)
        rc = scan_prepare(&s);
    /* Track by track, in the order the batches were saved. */
    while (rc == 0) {
        arena_reset();
        sqlite3_stmt *st = db_prepare(music_db,
            "SELECT track_id FROM changes WHERE state = 'pending' ORDER BY batch, id LIMIT 1");
        int step = st != NULL ? sqlite3_step(st) : SQLITE_ERROR;
        long long track_id = step == SQLITE_ROW ? sqlite3_column_int64(st, 0) : 0;
        if (step != SQLITE_ROW && step != SQLITE_DONE)
            db_log_error(music_db, "write: next track");
        sqlite3_finalize(st);
        if (step == SQLITE_DONE)
            break;
        rc = step == SQLITE_ROW ? write_track(&s, track_id, &n) : -1;
    }
    scan_finalize(&s);
    if (rc == 0)
        printf("music: %ld tracks written; %ld changes done, %ld with warnings, %ld failed\n",
               n.tracks, n.done, n.warnings, n.failed);
    else
        fprintf(stderr, "music: write stopped (database error)\n");
    close(lock);
    return rc == 0 ? 0 : 1;
}
