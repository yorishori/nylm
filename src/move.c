/*
 * Moving music files to where the naming rule puts them (src/move.h).
 *
 * The plan comes from the cache only, so the server can show it without
 * opening the library. The service (`nylm music-move`) holds the library
 * lock, moves each file with a rename that never replaces anything, and
 * records the new path in the cache in the same step; a file that can not
 * be recorded is moved back.
 */
#define _GNU_SOURCE /* renameat2, RENAME_NOREPLACE */

#include "move.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "arena.h"
#include "db.h"
#include "music.h"
#include "tags.h"

#define MOVE_MAX_OTHERS 10000 /* entries of one folder that go along with its tracks */

/* ---- naming ------------------------------------------------------------- */

/* 1 if c may not be in a name: a path separator, a character some file
 * systems refuse, or a control character. */
static int unsafe(unsigned char c)
{
    return c < 0x20 || c == 0x7f || (c != '\0' && strchr("/\\:*?\"<>|", c) != NULL);
}

/*
 * s made safe as one name of at most max bytes (see move_target), into out
 * (room for max + 1). Its length.
 */
static size_t safe_name(const char *s, char *out, size_t max)
{
    while (*s == ' ')
        s++;
    size_t n = 0;
    while (s[n] != '\0' && n < max) {
        out[n] = unsafe((unsigned char)s[n]) ? '_' : s[n];
        n++;
    }
    /* Cut inside a character: drop the rest of it. */
    if (s[n] != '\0' && ((unsigned char)s[n] & 0xc0) == 0x80) {
        while (n > 0 && ((unsigned char)out[n - 1] & 0xc0) == 0x80)
            n--;
        if (n > 0)
            n--;
    }
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '.'))
        n--;
    if (n == 0)
        out[n++] = '_';
    if (out[0] == '.')
        out[0] = '_';
    out[n] = '\0';
    return n;
}

static int digits(int n)
{
    int d = 1;
    while (n >= 10) {
        n /= 10;
        d++;
    }
    return d;
}

const char *move_target(const char *albumartist, const char *album, const char *title,
                        const char *tracknumber, const char *discnumber, const char *ext,
                        char *out, size_t outlen)
{
    if (albumartist == NULL || albumartist[0] == '\0')
        return "it has no album artist";
    if (album == NULL || album[0] == '\0')
        return "it has no album";
    if (title == NULL || title[0] == '\0')
        return "it has no title";
    int track, tracks, disc = 1, discs = 1;
    if (tracknumber == NULL || tags_number(tracknumber, &track, &tracks) != 0)
        return "its track number is not X/Y";
    if (discnumber != NULL && discnumber[0] != '\0' && tags_number(discnumber, &disc, &discs) != 0)
        return "its disc number is not X/Y";
    size_t extlen = ext != NULL ? strlen(ext) : 0;
    if (extlen == 0 || extlen > TAGS_MAX_EXT)
        return "its file has no music extension";

    char number[32];
    int width = digits(tracks) > 2 ? digits(tracks) : 2;
    if (discs > 1)
        snprintf(number, sizeof number, "%d-%0*d - ", disc, width, track);
    else
        snprintf(number, sizeof number, "%0*d - ", width, track);
    size_t fixed = strlen(number) + 1 + extlen; /* number, title, ".", ext */

    char artist_part[MOVE_MAX_NAME + 1], album_part[MOVE_MAX_NAME + 1];
    char title_part[MOVE_MAX_NAME + 1];
    safe_name(albumartist, artist_part, MOVE_MAX_NAME);
    safe_name(album, album_part, MOVE_MAX_NAME);
    safe_name(title, title_part, MOVE_MAX_NAME - fixed);
    int len = snprintf(out, outlen, "%s/%s/%s%s.%s", artist_part, album_part, number,
                       title_part, ext);
    if (len < 0 || (size_t)len >= outlen)
        return "its path would be too long";
    return NULL;
}

/* ---- the plan ----------------------------------------------------------- */

/* Column i of st's row in the arena (NULL if NULL); *oom set when out of
 * memory. */
static const char *column(sqlite3_stmt *st, int i, int *oom)
{
    const char *s = (const char *)sqlite3_column_text(st, i);
    if (s == NULL)
        return NULL;
    const char *copy = arena_strndup(s, (size_t)sqlite3_column_bytes(st, i));
    if (copy == NULL)
        *oom = 1;
    return copy;
}

static int by_target(const void *a, const void *b)
{
    const struct move_item *x = *(const struct move_item *const *)a;
    const struct move_item *y = *(const struct move_item *const *)b;
    return strcmp(x->to, y->to);
}

/* The item whose file is at path (items are sorted by path), or NULL. */
static struct move_item *at_path(const struct move_plan *plan, const char *path)
{
    size_t lo = 0, hi = plan->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(plan->items[mid].from, path);
        if (c == 0)
            return &plan->items[mid];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

int move_plan(struct move_plan *plan)
{
    memset(plan, 0, sizeof *plan);
    const char *root = music_root();
    sqlite3_stmt *count = db_prepare(music_db, "SELECT count(*) FROM tracks");
    long long total = count != NULL && sqlite3_step(count) == SQLITE_ROW
                          ? sqlite3_column_int64(count, 0) : -1;
    sqlite3_finalize(count);
    if (root == NULL || total < 0) {
        db_log_error(music_db, "move: count tracks");
        return -1;
    }
    /* BINARY order is strcmp() order: at_path() searches it. */
    sqlite3_stmt *st = db_prepare(music_db,
        "SELECT id, path, ext, albumartist, album, title, tracknumber, discnumber"
        " FROM tracks ORDER BY path COLLATE BINARY");
    plan->items = arena_alloc(((size_t)total + 1) * sizeof *plan->items);
    struct move_item **targets = arena_alloc(((size_t)total + 1) * sizeof *targets);
    char *rel = arena_alloc(TAGS_MAX_PATH);
    int rc = SQLITE_ERROR;
    int oom = plan->items == NULL || targets == NULL || rel == NULL;
    size_t ntargets = 0;
    while (st != NULL && !oom && plan->n < (size_t)total &&
           (rc = sqlite3_step(st)) == SQLITE_ROW) {
        struct move_item *it = &plan->items[plan->n++];
        it->id = sqlite3_column_int64(st, 0);
        it->to = NULL;
        it->from = column(st, 1, &oom);
        const char *ext = (const char *)sqlite3_column_text(st, 2);
        it->problem = move_target((const char *)sqlite3_column_text(st, 3),
                                  (const char *)sqlite3_column_text(st, 4),
                                  (const char *)sqlite3_column_text(st, 5),
                                  (const char *)sqlite3_column_text(st, 6),
                                  (const char *)sqlite3_column_text(st, 7), ext, rel,
                                  TAGS_MAX_PATH - strlen(root) - 1);
        if (it->problem == NULL && it->from != NULL && !music_inside(it->from))
            it->problem = "it is not in the music folder: scan the library";
        if (it->problem != NULL || oom)
            continue;
        char *to = arena_alloc(strlen(root) + 1 + strlen(rel) + 1);
        if (to == NULL) {
            oom = 1;
            continue;
        }
        snprintf(to, strlen(root) + 1 + strlen(rel) + 1, "%s/%s", root, rel);
        if (strcmp(to, it->from) != 0) {
            it->to = to;
            targets[ntargets++] = it;
        }
    }
    /* A track added since the count is not planned: the next run moves it. */
    int ok = st != NULL && !oom && (rc == SQLITE_DONE || plan->n == (size_t)total);
    if (!ok && oom)
        fprintf(stderr, "move: out of memory planning %lld tracks\n", total);
    else if (!ok)
        db_log_error(music_db, "move: tracks");
    sqlite3_finalize(st);
    if (!ok)
        return -1;

    /* Two tracks with one target: neither moves. */
    qsort(targets, ntargets, sizeof *targets, by_target);
    for (size_t i = 0; i + 1 < ntargets; i++) {
        if (strcmp(targets[i]->to, targets[i + 1]->to) != 0)
            continue;
        for (size_t j = i; j < ntargets && strcmp(targets[j]->to, targets[i]->to) == 0; j++)
            targets[j]->problem = "another track would get the same name";
    }
    for (size_t i = 0; i < ntargets; i++)
        if (targets[i]->problem != NULL)
            targets[i]->to = NULL;
    /* A target where a track stays: that one would have to move first.
     * (Repeated: a track that can not move can block another.) */
    for (int changed = 1; changed;) {
        changed = 0;
        for (size_t i = 0; i < ntargets; i++) {
            struct move_item *it = targets[i];
            struct move_item *there = it->to != NULL ? at_path(plan, it->to) : NULL;
            if (there != NULL && there->to == NULL) {
                it->to = NULL;
                it->problem = "a track that stays has its name";
                changed = 1;
            }
        }
    }
    for (size_t i = 0; i < plan->n; i++) {
        plan->moves += plan->items[i].to != NULL;
        plan->problems += plan->items[i].problem != NULL;
    }
    return 0;
}

/* ---- the service -------------------------------------------------------- */

/* What the service did, for its summary line. */
struct move_counts {
    long moved, failed, others, kept, folders;
};

/* Records one file moved (to set) or not (to NULL: state failed or kept)
 * in the moves table; track 0 for a file that is not a track. 0 or -1
 * (logged). */
static int record(long long track, const char *from, const char *to, const char *state,
                  const char *note)
{
    sqlite3_stmt *st = db_prepare(music_db,
        "INSERT INTO moves (track_id, from_path, to_path, state, note) VALUES (?, ?, ?, ?, ?)");
    int ok = st != NULL &&
             (track > 0 ? sqlite3_bind_int64(st, 1, track) : sqlite3_bind_null(st, 1)) ==
                 SQLITE_OK &&
             sqlite3_bind_text(st, 2, from, -1, SQLITE_STATIC) == SQLITE_OK &&
             (to != NULL ? sqlite3_bind_text(st, 3, to, -1, SQLITE_STATIC)
                         : sqlite3_bind_null(st, 3)) == SQLITE_OK &&
             sqlite3_bind_text(st, 4, state, -1, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_bind_text(st, 5, note, -1, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_step(st) == SQLITE_DONE;
    if (!ok)
        db_log_error(music_db, "move: record");
    sqlite3_finalize(st);
    return ok ? 0 : -1;
}

int move_rename_new(const char *from, const char *to)
{
    if (renameat2(AT_FDCWD, from, AT_FDCWD, to, RENAME_NOREPLACE) == 0)
        return 0;
    if (errno != EINVAL && errno != ENOSYS)
        return -1;
    struct stat sb;
    if (lstat(to, &sb) == 0) {
        errno = EEXIST;
        return -1;
    }
    return errno == ENOENT ? rename(from, to) : -1;
}

int move_make_folders(const char *path)
{
    char dir[TAGS_MAX_PATH];
    size_t start = strlen(music_root()) + 1;
    snprintf(dir, sizeof dir, "%s", path);
    for (char *p = strchr(dir + start, '/'); p != NULL; p = strchr(p + 1, '/')) {
        *p = '\0';
        struct stat sb;
        if (mkdir(dir, 0755) != 0 && errno != EEXIST)
            return -1;
        if (lstat(dir, &sb) != 0)
            return -1;
        if (!S_ISDIR(sb.st_mode)) {
            errno = ENOTDIR;
            return -1;
        }
        *p = '/';
    }
    return 0;
}

/* The folder of path, in the arena; NULL when out of memory. */
static const char *folder_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return arena_strndup(path, slash != NULL ? (size_t)(slash - path) : 0);
}

/* 1 if path is below folder. */
static int below(const char *path, const char *folder)
{
    size_t n = strlen(folder);
    return strncmp(path, folder, n) == 0 && path[n] == '/';
}

/*
 * Moves one track and records its new path in the cache, in one
 * transaction: if the cache can not be changed the file goes back.
 * 1 moved, 0 not (recorded), 2 not yet (its target is taken and this is
 * not the last try), -1 database error.
 */
static int move_track(const struct move_item *it, int last_try, struct move_counts *n)
{
    struct stat sb;
    const char *why = NULL;
    int taken = 0;
    if (lstat(it->from, &sb) != 0 || !S_ISREG(sb.st_mode)) {
        why = "the file is no longer there; scan the library";
    } else if (move_make_folders(it->to) != 0) {
        why = strerror(errno);
    } else if (move_rename_new(it->from, it->to) != 0) {
        taken = errno == EEXIST;
        why = taken ? "a file is already there" : strerror(errno);
    }
    if (taken && !last_try)
        return 2;
    if (why != NULL) {
        char note[TAGS_MAX_NOTE];
        snprintf(note, sizeof note, "not moved: %s", why);
        fprintf(stderr, "music: %s: %s\n", it->from, note);
        n->failed++;
        return record(it->id, it->from, NULL, "failed", note) == 0 ? 0 : -1;
    }
    sqlite3_stmt *st = db_prepare(music_db, "UPDATE tracks SET path = ? WHERE id = ?");
    int ok = st != NULL && db_exec(music_db, "BEGIN IMMEDIATE") == 0;
    ok = ok && sqlite3_bind_text(st, 1, it->to, -1, SQLITE_STATIC) == SQLITE_OK &&
         sqlite3_bind_int64(st, 2, it->id) == SQLITE_OK && sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    ok = ok && record(it->id, it->from, it->to, "done", "") == 0 &&
         db_exec(music_db, "COMMIT") == 0;
    if (!ok) {
        db_log_error(music_db, "move: new path");
        if (sqlite3_get_autocommit(music_db) == 0)
            db_exec(music_db, "ROLLBACK");
        if (move_rename_new(it->to, it->from) != 0)
            fprintf(stderr, "music: %s is now %s, but the cache still has the old path: "
                            "scan the library (%s)\n", it->from, it->to, strerror(errno));
        return -1;
    }
    n->moved++;
    return 1;
}

/* A folder tracks left, and where they went (NULL: several folders). */
struct source {
    const char *from, *to;
};

/* Reverse order: a folder comes after the folders inside it. */
static int by_folder_last(const void *a, const void *b)
{
    return strcmp(((const struct source *)b)->from, ((const struct source *)a)->from);
}

/* The number of tracks below folder, or -1 (logged). */
static long long tracks_below(const char *folder)
{
    sqlite3_stmt *st = db_prepare(music_db,
        "SELECT count(*) FROM tracks WHERE substr(path, 1, length(?1) + 1) = ?1 || '/'");
    long long n = st != NULL &&
                          sqlite3_bind_text(st, 1, folder, -1, SQLITE_STATIC) == SQLITE_OK &&
                          sqlite3_step(st) == SQLITE_ROW
                      ? sqlite3_column_int64(st, 0)
                      : -1;
    if (n < 0)
        db_log_error(music_db, "move: tracks below a folder");
    sqlite3_finalize(st);
    return n;
}

/*
 * A folder the tracks left: when no track is left below it and they all
 * went to one folder (not inside it), its other files and folders go there
 * too, unless the name is taken there. Then it is removed if empty, and so
 * are the folders above it, up to the library folder. 0 or -1 (database
 * error).
 */
static int tidy_folder(const struct source *s, struct move_counts *n)
{
    long long left = tracks_below(s->from);
    if (left != 0)
        return left < 0 ? -1 : 0;
    if (s->to == NULL || below(s->to, s->from)) {
        n->kept++;
        return record(0, s->from, NULL, "kept",
                      s->to == NULL ? "its other files stay: its tracks went to several folders"
                                    : "its other files stay: its tracks went into a folder in it");
    }
    DIR *d = opendir(s->from);
    if (d == NULL) {
        fprintf(stderr, "music: %s: %s\n", s->from, strerror(errno));
        return 0;
    }
    /* The names first: a folder changed while it is read may skip some. */
    const char **names = arena_alloc(MOVE_MAX_OTHERS * sizeof *names);
    size_t count = 0;
    struct dirent *e;
    while (names != NULL && count < MOVE_MAX_OTHERS && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0 &&
            (names[count++] = arena_strndup(e->d_name, strlen(e->d_name))) == NULL)
            names = NULL;
    }
    closedir(d);
    if (names == NULL) {
        fprintf(stderr, "move: out of memory reading %s\n", s->from);
        return -1;
    }
    int rc = 0;
    for (size_t i = 0; rc == 0 && i < count; i++) {
        char from[TAGS_MAX_PATH], to[TAGS_MAX_PATH];
        int a = snprintf(from, sizeof from, "%s/%s", s->from, names[i]);
        int b = snprintf(to, sizeof to, "%s/%s", s->to, names[i]);
        const char *why = NULL;
        if (a < 0 || (size_t)a >= sizeof from || b < 0 || (size_t)b >= sizeof to)
            why = "the path is too long";
        else if (move_rename_new(from, to) != 0)
            why = errno == EEXIST ? "that name is taken there" : strerror(errno);
        if (why == NULL) {
            n->others++;
            rc = record(0, from, to, "done", "");
        } else {
            char note[TAGS_MAX_NOTE];
            snprintf(note, sizeof note, "stays: %s (its tracks went to %s)", why, s->to);
            fprintf(stderr, "music: %s %s\n", from, note);
            n->kept++;
            rc = record(0, from, NULL, "kept", note);
        }
    }
    if (rc != 0)
        return -1;
    char dir[TAGS_MAX_PATH];
    snprintf(dir, sizeof dir, "%s", s->from);
    size_t root_len = strlen(music_root());
    while (strlen(dir) > root_len && rmdir(dir) == 0) {
        n->folders++;
        *strrchr(dir, '/') = '\0';
    }
    return 0;
}

int music_move(void)
{
    int lock = music_start_service("move");
    if (lock < 0)
        return 1;
    struct move_plan plan;
    struct move_counts n = { 0 };
    int rc = move_plan(&plan);
    struct source *sources = rc == 0 ? arena_alloc((plan.moves + 1) * sizeof *sources) : NULL;
    if (rc == 0 && sources == NULL) {
        fprintf(stderr, "move: out of memory\n");
        rc = -1;
    }
    /* Pass after pass while tracks move: a target may be the old place of
     * a track that moves later. The last pass records what is left. */
    size_t nsources = 0, waiting = rc == 0 ? plan.moves : 0;
    for (int last = 0; rc == 0 && waiting > 0;) {
        size_t before = waiting;
        for (size_t i = 0; rc == 0 && i < plan.n; i++) {
            struct move_item *it = &plan.items[i];
            if (it->to == NULL)
                continue;
            int r = move_track(it, last, &n);
            if (r < 0) {
                rc = -1;
            } else if (r != 2) {
                waiting--;
                if (r == 1) {
                    sources[nsources].from = folder_of(it->from);
                    sources[nsources].to = folder_of(it->to);
                    if (sources[nsources].from == NULL || sources[nsources].to == NULL)
                        rc = -1;
                    nsources++;
                }
                it->to = NULL;
            }
        }
        last = waiting == before;
    }
    /* Each folder once, with where all its tracks went (NULL: several). */
    if (rc == 0)
        qsort(sources, nsources, sizeof *sources, by_folder_last);
    size_t nfolders = 0;
    for (size_t i = 0; rc == 0 && i < nsources; i++) {
        struct source *prev = nfolders > 0 ? &sources[nfolders - 1] : NULL;
        if (prev != NULL && strcmp(prev->from, sources[i].from) == 0) {
            if (prev->to != NULL && strcmp(prev->to, sources[i].to) != 0)
                prev->to = NULL;
        } else {
            sources[nfolders++] = sources[i];
        }
    }
    /* A folder whose tracks only changed names stays as it is. */
    for (size_t i = 0; rc == 0 && i < nfolders; i++)
        if (sources[i].to == NULL || strcmp(sources[i].from, sources[i].to) != 0)
            rc = tidy_folder(&sources[i], &n);
    if (rc == 0)
        printf("music: %ld tracks moved, %ld failed, %zu can not move; %ld other files moved, "
               "%ld kept; %ld empty folders removed\n",
               n.moved, n.failed, plan.problems, n.others, n.kept, n.folders);
    else
        fprintf(stderr, "music: move stopped (database error)\n");
    close(lock);
    return rc == 0 ? 0 : 1;
}
