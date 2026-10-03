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
#include "tags.h"

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

/* root + "/" + rel into out (TAGS_MAX_PATH bytes). 0, or -1 if too long. */
static int full_path(const char *rel, char *out)
{
    int n = snprintf(out, TAGS_MAX_PATH, "%s%s%s", root, rel[0] != '\0' ? "/" : "", rel);
    return n < 0 || n >= TAGS_MAX_PATH ? -1 : 0;
}

/* Binds text, or NULL for a NULL pointer. */
static int bind_text(sqlite3_stmt *st, int i, const char *s)
{
    return s != NULL ? sqlite3_bind_text(st, i, s, -1, SQLITE_TRANSIENT)
                     : sqlite3_bind_null(st, i);
}

/*
 * The file columns, bound from parameter 1 on by bind_file():
 * size, mtime, seconds, pictures, multi, then the tags in tags.h order.
 */
#define FILE_COLUMN_COUNT (5 + TAG_FIELDS)

static int bind_file(sqlite3_stmt *st, const struct stat *sb, const struct tags *t)
{
    int rc = sqlite3_bind_int64(st, 1, (sqlite3_int64)sb->st_size);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(st, 2, tags_mtime(sb));
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int(st, 3, t->seconds);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int(st, 4, t->pictures);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(st, 5, (sqlite3_int64)t->multi);
    for (int i = 0; i < TAG_FIELDS && rc == SQLITE_OK; i++)
        rc = bind_text(st, 6 + i, t->value[i]);
    return rc;
}

/* ---- scan --------------------------------------------------------------- */

struct scan {
    long id;
    long files, parsed, failed;
    int incomplete;            /* a folder could not be read */
    double batch_start;
    sqlite3_stmt *find, *seen, *upsert, *album_add, *album_id;
};

/* Records the counts, commits what was done so far and starts a new batch. */
static int new_batch(struct scan *s)
{
    sqlite3_stmt *st = db_prepare(music_db,
        "UPDATE scans SET files = ?, parsed = ?, failed = ? WHERE id = ?");
    int ok = st != NULL && sqlite3_bind_int64(st, 1, s->files) == SQLITE_OK &&
             sqlite3_bind_int64(st, 2, s->parsed) == SQLITE_OK &&
             sqlite3_bind_int64(st, 3, s->failed) == SQLITE_OK &&
             sqlite3_bind_int64(st, 4, s->id) == SQLITE_OK && sqlite3_step(st) == SQLITE_DONE;
    if (st != NULL && !ok)
        db_log_error(music_db, "scan progress");
    sqlite3_finalize(st);
    if (!ok || db_exec(music_db, "COMMIT") != 0 || db_exec(music_db, "BEGIN IMMEDIATE") != 0)
        return -1;
    s->batch_start = now_seconds();
    return 0;
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

/* The id of the album for folder dir, created if new. -1 on error. */
static long album_for(struct scan *s, const char *dir)
{
    if (bind_text(s->album_add, 1, dir) != SQLITE_OK || step_once(s->album_add) != 0 ||
        bind_text(s->album_id, 1, dir) != SQLITE_OK)
        return -1;
    long id = sqlite3_step(s->album_id) == SQLITE_ROW ? (long)sqlite3_column_int64(s->album_id, 0)
                                                     : -1;
    if (id < 0)
        db_log_error(music_db, "album id");
    sqlite3_reset(s->album_id);
    sqlite3_clear_bindings(s->album_id);
    return id;
}

/* One music file: reads its tags unless the cache has it unchanged. 0, or
 * -1 on a database error (the scan stops). */
static int scan_file(struct scan *s, long album_id, const char *rel, enum tags_format format,
                     const struct stat *sb)
{
    s->files++;
    if (bind_text(s->find, 1, rel) != SQLITE_OK)
        return -1;
    int rc = sqlite3_step(s->find);
    int unchanged = rc == SQLITE_ROW && sqlite3_column_int64(s->find, 0) == sb->st_size &&
                    sqlite3_column_int64(s->find, 1) == tags_mtime(sb) &&
                    sqlite3_column_int64(s->find, 2) == album_id;
    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
        db_log_error(music_db, "scan lookup");
    sqlite3_reset(s->find);
    sqlite3_clear_bindings(s->find);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
        return -1;

    if (unchanged) {
        if (sqlite3_bind_int64(s->seen, 1, s->id) != SQLITE_OK ||
            bind_text(s->seen, 2, rel) != SQLITE_OK || step_once(s->seen) != 0)
            return -1;
    } else {
        char path[TAGS_MAX_PATH], err[256];
        struct tags t;
        if (full_path(rel, path) != 0 || tags_read(path, format, &t, err, sizeof err) != 0) {
            fprintf(stderr, "music: skipped %s/%s: %s\n", root, rel,
                    path[0] ? err : "path too long");
            s->failed++;
            arena_reset();
            return 0;
        }
        rc = bind_file(s->upsert, sb, &t);
        if (rc == SQLITE_OK)
            rc = sqlite3_bind_int64(s->upsert, FILE_COLUMN_COUNT + 1, album_id);
        if (rc == SQLITE_OK)
            rc = bind_text(s->upsert, FILE_COLUMN_COUNT + 2, rel);
        if (rc == SQLITE_OK)
            rc = bind_text(s->upsert, FILE_COLUMN_COUNT + 3, format == TAGS_MP3 ? "mp3" : "flac");
        if (rc == SQLITE_OK)
            rc = sqlite3_bind_int64(s->upsert, FILE_COLUMN_COUNT + 4, s->id);
        arena_reset();
        if (rc != SQLITE_OK || step_once(s->upsert) != 0)
            return -1;
        s->parsed++;
    }
    if (s->files % 1000 == 0) {
        printf("music: %ld files, %ld read\n", s->files, s->parsed);
        fflush(stdout);
    }
    return now_seconds() - s->batch_start >= 1.0 ? new_batch(s) : 0;
}

/* A folder that can not be fully read: its missing files are not removed
 * from the cache, as they may still be there. */
static void folder_failed(struct scan *s, const char *path, const char *why)
{
    fprintf(stderr, "music: can not read folder %s: %s\n", path, why);
    s->failed++;
    s->incomplete = 1;
}

/*
 * Scans folder rel (relative to root, "" for root itself; rel is a buffer
 * of TAGS_MAX_PATH bytes, used for the paths below it and restored).
 * 0, or -1 on a database error.
 */
static int walk(struct scan *s, char *rel, size_t rel_len, int depth)
{
    char path[TAGS_MAX_PATH];
    if (full_path(rel, path) != 0) {
        folder_failed(s, rel, "path too long");
        return 0;
    }
    DIR *d = opendir(path);
    if (d == NULL) {
        folder_failed(s, path, strerror(errno));
        return 0;
    }
    long album_id = 0;
    int rc = 0;
    while (rc == 0) {
        errno = 0;
        struct dirent *de = readdir(d);
        if (de == NULL) {
            if (errno != 0)
                folder_failed(s, path, strerror(errno));
            break;
        }
        const char *name = de->d_name;
        if (name[0] == '.')
            continue; /* hidden files, nylm's temporary copies, "." and ".." */
        struct stat sb;
        if (fstatat(dirfd(d), name, &sb, AT_SYMLINK_NOFOLLOW) != 0) {
            fprintf(stderr, "music: skipped %s/%s: %s\n", path, name, strerror(errno));
            s->failed++;
            continue;
        }
        int is_dir = S_ISDIR(sb.st_mode);
        enum tags_format format = S_ISREG(sb.st_mode) ? tags_format_of(name) : TAGS_NONE;
        if (!is_dir && format == TAGS_NONE)
            continue;
        size_t len = strlen(name);
        if (!text_valid(name, 0) || root_len + 1 + rel_len + 1 + len >= TAGS_MAX_PATH) {
            fprintf(stderr, "music: skipped %s/%s: name is not UTF-8 text or path too long\n",
                    path, name);
            if (is_dir)
                s->incomplete = 1;
            s->failed++;
            continue;
        }
        if (!is_dir && album_id == 0 && (album_id = album_for(s, rel)) < 0) {
            rc = -1;
            break;
        }

        size_t at = rel_len;
        if (at > 0)
            rel[at++] = '/';
        memcpy(rel + at, name, len + 1);
        if (!is_dir)
            rc = scan_file(s, album_id, rel, format, &sb);
        else if (depth == MUSIC_MAX_DEPTH)
            folder_failed(s, rel, "too deep");
        else
            rc = walk(s, rel, at + len, depth + 1);
        rel[rel_len] = '\0';
    }
    closedir(d);
    return rc;
}

/* Prepares the statements a scan uses. 0 or -1. */
static int scan_prepare(struct scan *s)
{
    s->find = db_prepare(music_db, "SELECT size, mtime, album_id FROM tracks WHERE path = ?");
    s->seen = db_prepare(music_db, "UPDATE tracks SET scan = ? WHERE path = ?");
    s->upsert = db_prepare(music_db,
        "INSERT INTO tracks (size, mtime, seconds, pictures, multi, title, artist, album,"
        "  albumartist, genre, date, tracknumber, discnumber, compilation, composer,"
        "  album_id, path, format, scan)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
        " ON CONFLICT (path) DO UPDATE SET size = excluded.size, mtime = excluded.mtime,"
        "  seconds = excluded.seconds, pictures = excluded.pictures, multi = excluded.multi,"
        "  title = excluded.title, artist = excluded.artist, album = excluded.album,"
        "  albumartist = excluded.albumartist, genre = excluded.genre, date = excluded.date,"
        "  tracknumber = excluded.tracknumber, discnumber = excluded.discnumber,"
        "  compilation = excluded.compilation, composer = excluded.composer,"
        "  album_id = excluded.album_id,"
        "  format = excluded.format, scan = excluded.scan");
    s->album_add = db_prepare(music_db, "INSERT INTO albums (dir) VALUES (?)"
                                        " ON CONFLICT (dir) DO NOTHING");
    s->album_id = db_prepare(music_db, "SELECT id FROM albums WHERE dir = ?");
    return s->find && s->seen && s->upsert && s->album_add && s->album_id ? 0 : -1;
}

static void scan_finalize(struct scan *s)
{
    sqlite3_finalize(s->find);
    sqlite3_finalize(s->seen);
    sqlite3_finalize(s->upsert);
    sqlite3_finalize(s->album_add);
    sqlite3_finalize(s->album_id);
}

/* Removes cached files the scan did not see, and albums left empty.
 * Returns the number of tracks removed, or -1. */
static long remove_missing(struct scan *s)
{
    sqlite3_stmt *st = db_prepare(music_db, "DELETE FROM tracks WHERE scan <> ?");
    if (st == NULL || sqlite3_bind_int64(st, 1, s->id) != SQLITE_OK || step_once(st) != 0) {
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    long removed = (long)sqlite3_changes(music_db);
    if (db_exec(music_db, "DELETE FROM albums WHERE id NOT IN (SELECT album_id FROM tracks)") != 0)
        return -1;
    return removed;
}

/* Records the end of the scan. ok: it saw the whole library. 0 or -1. */
static int scan_finished(struct scan *s, int ok)
{
    sqlite3_stmt *st = db_prepare(music_db,
        "UPDATE scans SET files = ?, parsed = ?, failed = ?, finished = unixepoch(), ok = ?"
        " WHERE id = ?");
    int done = st != NULL && sqlite3_bind_int64(st, 1, s->files) == SQLITE_OK &&
               sqlite3_bind_int64(st, 2, s->parsed) == SQLITE_OK &&
               sqlite3_bind_int64(st, 3, s->failed) == SQLITE_OK &&
               sqlite3_bind_int(st, 4, ok) == SQLITE_OK &&
               sqlite3_bind_int64(st, 5, s->id) == SQLITE_OK && step_once(st) == 0;
    sqlite3_finalize(st);
    return done ? 0 : -1;
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

/* ---- scan, continued ---------------------------------------------------- */

int music_scan(void)
{
    if (root[0] == '\0') {
        fprintf(stderr, "music: NYLM_MUSIC is not set\n");
        return 1;
    }
    int lock = lock_service("scan");
    if (lock < 0)
        return 1;
    if (!music_available()) {
        close(lock);
        return 1;
    }

    struct scan s = { 0 };
    if (db_exec(music_db, "INSERT INTO scans DEFAULT VALUES") != 0) {
        close(lock);
        return 1;
    }
    s.id = (long)sqlite3_last_insert_rowid(music_db);
    printf("music: scanning %s\n", root);
    fflush(stdout);

    char rel[TAGS_MAX_PATH] = "";
    s.batch_start = now_seconds();
    int rc = scan_prepare(&s) == 0 && db_exec(music_db, "BEGIN IMMEDIATE") == 0 ? 0 : -1;
    if (rc == 0)
        rc = walk(&s, rel, 0, 0);
    long removed = 0;
    if (rc == 0 && !s.incomplete && (removed = remove_missing(&s)) < 0)
        rc = -1;
    scan_finalize(&s);
    int ok = rc == 0 && !s.incomplete;
    if (rc == 0 && scan_finished(&s, ok) == 0 && db_exec(music_db, "COMMIT") == 0) {
        printf("music: %ld files, %ld read, %ld removed, %ld failed%s\n", s.files, s.parsed,
               removed, s.failed, s.incomplete ? " (some folders could not be read: "
                                                   "nothing was removed)" : "");
    } else {
        ok = 0;
        if (sqlite3_get_autocommit(music_db) == 0)
            db_exec(music_db, "ROLLBACK");
        scan_finished(&s, 0);
        fprintf(stderr, "music: scan failed (database error)\n");
    }
    close(lock); /* releases the lock */
    return ok ? 0 : 1;
}

/* ---- write -------------------------------------------------------------- */

/* Reads a track's file again into its cache row. 0 or -1 (logged). */
static int reread(long track_id, const char *path, enum tags_format format)
{
    char err[TAGS_MAX_NOTE];
    struct stat sb;
    struct tags t;
    if (lstat(path, &sb) != 0 || !S_ISREG(sb.st_mode) ||
        tags_read(path, format, &t, err, sizeof err) != 0) {
        fprintf(stderr, "music: can not read %s again after writing it\n", path);
        return -1;
    }
    sqlite3_stmt *st = db_prepare(music_db,
        "UPDATE tracks SET size = ?, mtime = ?, seconds = ?, pictures = ?, multi = ?,"
        " title = ?, artist = ?, album = ?, albumartist = ?, genre = ?, date = ?,"
        " tracknumber = ?, discnumber = ?, compilation = ?, composer = ? WHERE id = ?");
    int rc = st != NULL && bind_file(st, &sb, &t) == SQLITE_OK &&
                     sqlite3_bind_int64(st, FILE_COLUMN_COUNT + 1, track_id) == SQLITE_OK &&
                     step_once(st) == 0
                 ? 0
                 : -1;
    sqlite3_finalize(st);
    return rc;
}

/* Copies a text column into the arena; NULL if NULL or out of memory. */
static const char *column_dup(sqlite3_stmt *st, int col)
{
    const char *s = (const char *)sqlite3_column_text(st, col);
    return s != NULL ? arena_strndup(s, (size_t)sqlite3_column_bytes(st, col)) : NULL;
}

struct write_counts {
    long files, done, warnings, failed;
};

/* Records what became of a change. 0 or -1 (logged). */
static int change_result(long id, const struct tags_change *c)
{
    const char *state = c->failed ? "failed" : c->note[0] != '\0' ? "warning" : "done";
    sqlite3_stmt *st = db_prepare(music_db,
        "UPDATE changes SET state = ?, note = ?, finished = unixepoch() WHERE id = ?");
    int rc = st != NULL && sqlite3_bind_text(st, 1, state, -1, SQLITE_STATIC) == SQLITE_OK &&
                     sqlite3_bind_text(st, 2, c->note, -1, SQLITE_STATIC) == SQLITE_OK &&
                     sqlite3_bind_int64(st, 3, id) == SQLITE_OK && step_once(st) == 0
                 ? 0
                 : -1;
    sqlite3_finalize(st);
    return rc;
}

/* Writes one track's pending changes and records each result. 0, or -1 on
 * a database error (the service stops). */
static int write_track(long track_id, struct write_counts *n)
{
    sqlite3_stmt *st = db_prepare(music_db, "SELECT path, format FROM tracks WHERE id = ?");
    if (st == NULL || sqlite3_bind_int64(st, 1, track_id) != SQLITE_OK ||
        sqlite3_step(st) != SQLITE_ROW) {
        db_log_error(music_db, "write: track");
        sqlite3_finalize(st);
        return -1;
    }
    const char *rel = column_dup(st, 0);
    enum tags_format format =
        strcmp((const char *)sqlite3_column_text(st, 1), "mp3") == 0 ? TAGS_MP3 : TAGS_FLAC;
    sqlite3_finalize(st);

    /* At most one pending change per tag: at most TAG_FIELDS rows. */
    struct tags_change c[TAG_FIELDS];
    long ids[TAG_FIELDS];
    int count = 0, rc;
    st = db_prepare(music_db, "SELECT id, field, old, new FROM changes"
                              " WHERE state = 'pending' AND track_id = ? ORDER BY id");
    if (st == NULL || rel == NULL || sqlite3_bind_int64(st, 1, track_id) != SQLITE_OK) {
        sqlite3_finalize(st);
        return -1;
    }
    while ((rc = sqlite3_step(st)) == SQLITE_ROW && count < TAG_FIELDS) {
        int field = tags_field_of((const char *)sqlite3_column_text(st, 1));
        memset(&c[count], 0, sizeof c[count]);
        ids[count] = (long)sqlite3_column_int64(st, 0);
        c[count].field = field >= 0 ? (enum tag_field)field : TAG_TITLE;
        c[count].old = column_dup(st, 2);
        c[count].value = column_dup(st, 3);
        if (field < 0 || c[count].value == NULL) {
            sqlite3_finalize(st);
            fprintf(stderr, "music: change %ld is not readable\n", ids[count]);
            return -1;
        }
        count++;
    }
    if (rc != SQLITE_DONE)
        db_log_error(music_db, "write: changes");
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        return -1;

    char path[TAGS_MAX_PATH];
    int saved = 0;
    if (full_path(rel, path) == 0) {
        saved = tags_write(path, format, c, count);
    } else {
        for (int i = 0; i < count; i++) {
            c[i].failed = 1;
            snprintf(c[i].note, sizeof c[i].note, "the path is too long");
        }
    }

    n->files += saved;
    if (db_exec(music_db, "BEGIN IMMEDIATE") != 0)
        return -1;
    for (int i = 0; i < count; i++) {
        if (change_result(ids[i], &c[i]) != 0) {
            db_exec(music_db, "ROLLBACK");
            return -1;
        }
        if (c[i].failed) {
            n->failed++;
            fprintf(stderr, "music: %s: %s not changed: %s\n", rel, tags_name[c[i].field],
                    c[i].note);
        } else if (c[i].note[0] != '\0') {
            n->warnings++;
            fprintf(stderr, "music: %s: %s changed; %s\n", rel, tags_name[c[i].field],
                    c[i].note);
        } else {
            n->done++;
        }
    }
    /* The changes are recorded even if the cache can not be refreshed;
     * the next scan does that. */
    if (saved)
        reread(track_id, path, format);
    return db_exec(music_db, "COMMIT") == 0 ? 0 : -1;
}

int music_write(void)
{
    if (root[0] == '\0') {
        fprintf(stderr, "music: NYLM_MUSIC is not set\n");
        return 1;
    }
    int lock = lock_service("write");
    if (lock < 0)
        return 1;
    if (!music_available()) {
        close(lock); /* the changes stay pending */
        return 1;
    }
    struct write_counts n = { 0 };
    int rc = db_exec(music_db,
        "UPDATE changes SET state = 'failed', finished = unixepoch(),"
        " note = 'the file is no longer in the library'"
        " WHERE state = 'pending' AND track_id IS NULL");
    n.failed += rc == 0 ? sqlite3_changes(music_db) : 0;

    /* One track at a time, in id order: each turn moves past one id. */
    sqlite3_int64 last = 0;
    while (rc == 0) {
        arena_reset();
        sqlite3_stmt *st = db_prepare(music_db,
            "SELECT min(track_id) FROM changes WHERE state = 'pending' AND track_id > ?");
        int step = st != NULL && sqlite3_bind_int64(st, 1, last) == SQLITE_OK
                       ? sqlite3_step(st) : SQLITE_ERROR;
        int found = step == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL;
        if (found)
            last = sqlite3_column_int64(st, 0);
        if (step != SQLITE_ROW)
            db_log_error(music_db, "write: next track");
        sqlite3_finalize(st);
        if (step != SQLITE_ROW)
            rc = -1;
        else if (!found)
            break;
        else
            rc = write_track((long)last, &n);
    }
    if (rc == 0)
        printf("music: %ld files written; %ld changes done, %ld with warnings, %ld failed\n",
               n.files, n.done, n.warnings, n.failed);
    else
        fprintf(stderr, "music: write stopped (database error)\n");
    close(lock);
    return rc == 0 ? 0 : 1;
}
