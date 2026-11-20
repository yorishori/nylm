/*
 * Copying music to the DAP (src/dapsync.h).
 */
#define _GNU_SOURCE /* O_NOFOLLOW, O_DIRECTORY, statfs */

#include "dapsync.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "arena.h"
#include "conn.h"
#include "dap.h"
#include "db.h"
#include "json.h"
#include "move.h"
#include "music.h"
#include "navidrome.h"
#include "sysinfo.h"
#include "tags.h"

#define COPY_BUFFER    (1024 * 1024)
#define MAX_PICTURE    (50 * 1024 * 1024) /* bytes in a folder's picture copied along */
#define MAX_NOTE       2000               /* bytes of failures kept in a sync's note */
#define MSDOS_MAGIC    0x4d44             /* statfs f_type of FAT */
#define EXFAT_MAGIC    0x2011BAB0         /* and of exFAT */

/* ---- names ----------------------------------------------------------------- */

/* 1 if part (len bytes) is a name: not empty, not "." or "..". */
static int is_name(const char *part, size_t len)
{
    return len > 0 && !(len == 1 && part[0] == '.') &&
           !(len == 2 && part[0] == '.' && part[1] == '.');
}

int dap_dest_path(const char *rel, char *out, size_t outlen)
{
    size_t o = 0;
    if (rel == NULL || rel[0] == '/' || !text_valid(rel, 0))
        return -1;
    for (const char *p = rel;;) {
        size_t len = strcspn(p, "/");
        if (!is_name(p, len) || len > 1024)
            return -1;
        char part[1025], safe[MOVE_MAX_NAME + 1];
        memcpy(part, p, len);
        part[len] = '\0';
        int last = p[len] == '\0';
        const char *dot = last ? strrchr(part, '.') : NULL;
        size_t n;
        if (dot != NULL && dot != part && strlen(dot) <= TAGS_MAX_EXT + 1) {
            /* The name without its extension, then the extension. */
            char ext[TAGS_MAX_EXT + 2], extsafe[TAGS_MAX_EXT + 2];
            snprintf(ext, sizeof ext, "%s", dot + 1);
            part[dot - part] = '\0';
            size_t e = move_safe_name(ext, extsafe, TAGS_MAX_EXT);
            n = move_safe_name(part, safe, MOVE_MAX_NAME - 1 - e);
            snprintf(safe + n, sizeof safe - n, ".%s", extsafe);
            n += 1 + e;
        } else {
            n = move_safe_name(part, safe, MOVE_MAX_NAME);
        }
        if (o + n + 1 >= outlen)
            return -1;
        if (o > 0)
            out[o++] = '/';
        memcpy(out + o, safe, n);
        o += n;
        out[o] = '\0';
        if (last)
            return 0;
        p += len + 1;
    }
}

void dap_fold(const char *s, char *out, size_t outlen)
{
    size_t i = 0;
    for (; s[i] != '\0' && i + 1 < outlen; i++)
        out[i] = s[i] >= 'A' && s[i] <= 'Z' ? (char)(s[i] - 'A' + 'a') : s[i];
    if (outlen > 0)
        out[i] = '\0';
}

/* s with newlines and carriage returns as spaces (for an #EXTINF line). */
static void one_line(char *s)
{
    for (; *s != '\0'; s++)
        if (*s == '\n' || *s == '\r')
            *s = ' ';
}

char *dap_m3u8(const struct dap_song *songs, size_t n)
{
    size_t cap = 16;
    for (size_t i = 0; i < n; i++)
        cap += 64 + strlen(songs[i].dest) +
               (songs[i].artist != NULL ? strlen(songs[i].artist) : 0) +
               (songs[i].title != NULL ? strlen(songs[i].title) : 0);
    char *text = arena_alloc(cap);
    if (text == NULL)
        return NULL;
    size_t o = (size_t)snprintf(text, cap, "#EXTM3U\n");
    for (size_t i = 0; i < n; i++) {
        const struct dap_song *s = &songs[i];
        char info[1200];
        snprintf(info, sizeof info, "%s%s%s", s->artist != NULL ? s->artist : "",
                 s->artist != NULL && s->title != NULL ? " - " : "",
                 s->title != NULL ? s->title : "");
        one_line(info);
        int w = snprintf(text + o, cap - o, "#EXTINF:%ld,%s\n%s\n",
                         s->duration >= 0 ? s->duration : -1L, info, s->dest);
        if (w < 0 || (size_t)w >= cap - o)
            return NULL;
        o += (size_t)w;
    }
    return text;
}

/* ---- the folder on the DAP ---------------------------------------------------- */

/*
 * Opens the folder of rel below root (each part a name, no link followed;
 * create: folders that are missing are made) and points *leaf at rel's
 * last part (in the arena). The handle, or -1 (errno).
 */
static int open_parent(int root, const char *rel, int create, const char **leaf)
{
    char *copy = arena_strndup(rel, strlen(rel));
    if (copy == NULL) {
        errno = ENOMEM;
        return -1;
    }
    int fd = openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    char *part = copy;
    for (char *slash = strchr(part, '/'); fd >= 0 && slash != NULL; slash = strchr(part, '/')) {
        *slash = '\0';
        if (!is_name(part, strlen(part))) {
            close(fd);
            errno = EINVAL;
            return -1;
        }
        if (create && mkdirat(fd, part, 0755) != 0 && errno != EEXIST) {
            int e = errno;
            close(fd);
            errno = e;
            return -1;
        }
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int e = errno;
        close(fd);
        errno = e;
        fd = next;
        part = slash + 1;
    }
    if (fd >= 0 && !is_name(part, strlen(part))) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    *leaf = part;
    return fd;
}

/* A file or folder found below DAP_FOLDER. */
struct found {
    const char *path; /* below DAP_FOLDER */
    const char *key;  /* folded */
    int depth;
};

struct listing {
    struct found *files, *dirs;
    size_t nfiles, ndirs, max;
};

/* Lists what is below the folder fd (path: its path below DAP_FOLDER, ""
 * for DAP_FOLDER itself) into l. 0, or -1 (err set). */
static int walk(int fd, const char *path, int depth, struct listing *l, char *err, size_t errlen)
{
    int dfd = dup(fd);
    DIR *d = dfd >= 0 ? fdopendir(dfd) : NULL;
    /* The copy shares the handle's place in the folder: from the start. */
    if (d != NULL)
        rewinddir(d);
    if (d == NULL) {
        snprintf(err, errlen, "can not read %s on the DAP: %s", path[0] ? path : DAP_FOLDER,
                 strerror(errno));
        if (dfd >= 0)
            close(dfd);
        return -1;
    }
    int rc = 0;
    struct dirent *e;
    while (rc == 0 && (e = readdir(d)) != NULL) {
        if (!is_name(e->d_name, strlen(e->d_name)))
            continue;
        struct stat sb;
        size_t len = strlen(path) + strlen(e->d_name) + 2;
        char *child = arena_alloc(len), *key = arena_alloc(len);
        if (child == NULL || key == NULL) {
            snprintf(err, errlen, "out of memory listing the DAP");
            rc = -1;
            break;
        }
        snprintf(child, len, "%s%s%s", path, path[0] ? "/" : "", e->d_name);
        dap_fold(child, key, len);
        if (fstatat(fd, e->d_name, &sb, AT_SYMLINK_NOFOLLOW) != 0)
            continue;
        if (S_ISDIR(sb.st_mode)) {
            if (depth >= DAP_MAX_DEPTH || l->ndirs >= l->max) {
                snprintf(err, errlen, "%s on the DAP is too deep or has too many folders", child);
                rc = -1;
                break;
            }
            l->dirs[l->ndirs++] = (struct found){ child, key, depth };
            int sub = openat(fd, e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (sub < 0) {
                snprintf(err, errlen, "can not open %s on the DAP: %s", child, strerror(errno));
                rc = -1;
                break;
            }
            rc = walk(sub, child, depth + 1, l, err, errlen);
            close(sub);
        } else {
            if (l->nfiles >= l->max) {
                snprintf(err, errlen, "the DAP's %s folder has more than %d files", DAP_FOLDER,
                         DAP_MAX_FILES);
                rc = -1;
                break;
            }
            l->files[l->nfiles++] = (struct found){ child, key, depth };
        }
    }
    closedir(d);
    return rc;
}

static int by_key(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Deepest first, so a folder comes after the folders in it. */
static int by_depth(const void *a, const void *b)
{
    return ((const struct found *)b)->depth - ((const struct found *)a)->depth;
}

/* Writes all of buf to fd. 0, or -1 (errno). */
static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/*
 * Copies src (open, size bytes, its time mtime) or text (src < 0) to dest
 * below root: to DAP_PART in its folder, then renamed. 0, or -1 with why in
 * why; *full set when the DAP has no room left.
 */
static int put_file(int root, const char *dest, int src, const char *text, long long mtime,
                    char *buf, char *why, size_t whylen, int *full)
{
    const char *leaf;
    int dir = open_parent(root, dest, 1, &leaf);
    if (dir < 0) {
        snprintf(why, whylen, "can not make its folder: %s", strerror(errno));
        *full = errno == ENOSPC;
        return -1;
    }
    unlinkat(dir, DAP_PART, 0);
    int out = openat(dir, DAP_PART, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    int rc = out >= 0 ? 0 : -1;
    const char *step = "can not write it";
    if (rc == 0 && src >= 0) {
        for (;;) {
            ssize_t n = read(src, buf, COPY_BUFFER);
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0) {
                step = "can not read it";
                rc = -1;
            } else if (n > 0 && write_all(out, buf, (size_t)n) != 0) {
                rc = -1;
            }
            if (n <= 0 || rc != 0)
                break;
        }
    } else if (rc == 0) {
        rc = write_all(out, text, strlen(text));
    }
    int e = errno;
    if (rc == 0 && src >= 0) {
        const struct timespec times[2] = { { (time_t)mtime, 0 }, { (time_t)mtime, 0 } };
        if (futimens(out, times) != 0)
            fprintf(stderr, "dap: %s: can not set its time: %s\n", dest, strerror(errno));
    }
    if (out >= 0 && close(out) != 0 && rc == 0) {
        rc = -1;
        e = errno;
    }
    if (rc == 0 && renameat(dir, DAP_PART, dir, leaf) != 0) {
        step = "can not rename it";
        rc = -1;
        e = errno;
    }
    if (rc != 0) {
        snprintf(why, whylen, "%s: %s", step, strerror(e));
        *full = e == ENOSPC;
        unlinkat(dir, DAP_PART, 0);
    }
    close(dir);
    return rc;
}

/* 1 if the file dest below root holds text already. */
static int same_text(int root, const char *dest, const char *text)
{
    const char *leaf;
    size_t len = strlen(text);
    int dir = open_parent(root, dest, 0, &leaf);
    int fd = dir >= 0 ? openat(dir, leaf, O_RDONLY | O_NOFOLLOW | O_CLOEXEC) : -1;
    char *buf = fd >= 0 ? arena_alloc(len + 1) : NULL;
    size_t got = 0;
    while (buf != NULL && got <= len) {
        ssize_t n = read(fd, buf + got, len + 1 - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    int same = buf != NULL && got == len && memcmp(buf, text, len) == 0;
    if (fd >= 0)
        close(fd);
    if (dir >= 0)
        close(dir);
    return same;
}

/* A planned file or text by its folded name (to find clashes). */
struct planned {
    const char *key;
    size_t index;
};

static int by_planned(const void *a, const void *b)
{
    const struct planned *x = a, *y = b;
    int c = strcmp(x->key, y->key);
    return c != 0 ? c : x->index < y->index ? -1 : x->index > y->index;
}

/* Tells fn; 0, or -1 (err set) if it says to stop. */
static int tell(dap_report_fn fn, void *ctx, const struct dap_progress *p, const char *what,
                const char *path, const char *why, char *err, size_t errlen)
{
    if (fn == NULL || fn(p, what, path, why, ctx) == 0)
        return 0;
    snprintf(err, errlen, "stopped");
    return -1;
}

int dap_apply(int dir, const struct dap_plan *plan, int fat, struct dap_progress *p,
              dap_report_fn fn, void *ctx, char *err, size_t errlen)
{
    memset(p, 0, sizeof *p);
    size_t n = plan->nfiles + plan->ntexts;
    struct planned *names = arena_alloc((n + 1) * sizeof *names);
    const char **keys = arena_alloc((n + 1) * sizeof *keys);
    char *clash = arena_alloc(n + 1), *todo = arena_alloc(n + 1);
    struct listing l = { arena_alloc(DAP_MAX_FILES * sizeof *l.files),
                         arena_alloc(DAP_MAX_FILES * sizeof *l.dirs), 0, 0, DAP_MAX_FILES };
    if (names == NULL || keys == NULL || clash == NULL || todo == NULL || l.files == NULL ||
        l.dirs == NULL) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    memset(clash, 0, n + 1);
    memset(todo, 0, n + 1);
    for (size_t i = 0; i < n; i++) {
        const char *dest = i < plan->nfiles ? plan->files[i].dest : plan->texts[i - plan->nfiles].dest;
        size_t len = strlen(dest) + 1;
        char *key = arena_alloc(len);
        if (key == NULL) {
            snprintf(err, errlen, "out of memory");
            return -1;
        }
        dap_fold(dest, key, len);
        names[i] = (struct planned){ key, i };
    }
    qsort(names, n, sizeof *names, by_planned);
    size_t nkeys = 0;
    for (size_t i = 0; i < n; i++) {
        if (i > 0 && strcmp(names[i].key, names[i - 1].key) == 0)
            clash[names[i].index] = 1;
        else
            keys[nkeys++] = names[i].key;
    }

    /* What is there that the plan does not have goes, then the folders it
     * leaves empty. */
    if (walk(dir, "", 0, &l, err, errlen) != 0)
        return -1;
    for (size_t i = 0; i < l.nfiles; i++) {
        if (bsearch(&l.files[i].key, keys, nkeys, sizeof *keys, by_key) != NULL)
            continue;
        const char *leaf;
        int fd = open_parent(dir, l.files[i].path, 0, &leaf);
        int ok = fd >= 0 && unlinkat(fd, leaf, 0) == 0;
        int e = errno;
        if (fd >= 0)
            close(fd);
        if (ok)
            p->deleted++;
        else
            p->failed++;
        if (tell(fn, ctx, p, ok ? "deleted" : "failed", l.files[i].path,
                 ok ? NULL : strerror(e), err, errlen) != 0)
            return -1;
    }
    qsort(l.dirs, l.ndirs, sizeof *l.dirs, by_depth);
    for (size_t i = 0; i < l.ndirs; i++) {
        const char *leaf;
        int fd = open_parent(dir, l.dirs[i].path, 0, &leaf);
        if (fd >= 0) {
            unlinkat(fd, leaf, AT_REMOVEDIR); /* only when empty */
            close(fd);
        }
    }

    /* What is there already, as it should be, stays. */
    long long replaced = 0;
    for (size_t i = 0; i < plan->nfiles; i++) {
        const struct dap_file *f = &plan->files[i];
        const char *leaf;
        struct stat sb;
        if (clash[i])
            continue;
        int fd = open_parent(dir, f->dest, 0, &leaf);
        int there = fd >= 0 && fstatat(fd, leaf, &sb, AT_SYMLINK_NOFOLLOW) == 0 &&
                    S_ISREG(sb.st_mode);
        if (fd >= 0)
            close(fd);
        long long diff = there ? (long long)sb.st_mtime - f->mtime : 0;
        if (there && sb.st_size == f->size && diff >= -2 && diff <= 2) {
            p->kept++;
            if (tell(fn, ctx, p, "kept", f->dest, NULL, err, errlen) != 0)
                return -1;
            continue;
        }
        todo[i] = 1;
        if (fat && f->size > DAP_FAT_LIMIT)
            continue; /* it fails below; it needs no room */
        p->total += f->size;
        replaced += there ? (long long)sb.st_size : 0;
    }
    struct statvfs vs;
    if (fstatvfs(dir, &vs) != 0) {
        snprintf(err, errlen, "can not tell how much room the DAP has: %s", strerror(errno));
        return -1;
    }
    long long room = (long long)vs.f_bavail * (long long)vs.f_frsize;
    if (p->total - replaced > room) {
        snprintf(err, errlen, "the DAP has room for %lld more bytes, the copies need %lld: "
                              "take something off the selection", room, p->total - replaced);
        return -1;
    }

    char *buf = malloc(COPY_BUFFER);
    if (buf == NULL) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    int rc = 0;
    for (size_t i = 0; rc == 0 && i < n; i++) {
        int is_text = i >= plan->nfiles;
        const char *dest = is_text ? plan->texts[i - plan->nfiles].dest : plan->files[i].dest;
        char why[600] = "";
        int full = 0, ok = 0;
        if (clash[i]) {
            snprintf(why, sizeof why, "its name on the DAP is that of another file, but for "
                                      "upper and lower case (FAT does not tell them apart)");
        } else if (is_text && same_text(dir, dest, plan->texts[i - plan->nfiles].text)) {
            p->kept++;
            rc = tell(fn, ctx, p, "kept", dest, NULL, err, errlen);
            continue;
        } else if (is_text) {
            ok = put_file(dir, dest, -1, plan->texts[i - plan->nfiles].text, 0, buf, why,
                          sizeof why, &full) == 0;
        } else if (!todo[i]) {
            continue;
        } else if (fat && plan->files[i].size > DAP_FAT_LIMIT) {
            snprintf(why, sizeof why, "it is 4 GiB or more: FAT can not hold it");
        } else {
            const struct dap_file *f = &plan->files[i];
            int src = open(f->src, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            struct stat sb;
            if (src < 0 || fstat(src, &sb) != 0 || !S_ISREG(sb.st_mode))
                snprintf(why, sizeof why, "can not read %s: %s", f->src,
                         src < 0 ? strerror(errno) : "not a file");
            else
                ok = put_file(dir, dest, src, NULL, f->mtime, buf, why, sizeof why, &full) == 0;
            if (src >= 0)
                close(src);
            if (ok)
                p->bytes += f->size;
        }
        if (full) {
            snprintf(err, errlen, "the DAP is full (%s)", dest);
            rc = -1;
            break;
        }
        if (ok)
            p->copied++;
        else
            p->failed++;
        rc = tell(fn, ctx, p, ok ? "copied" : "failed", dest, ok ? NULL : why, err, errlen);
    }
    free(buf);
    return rc;
}

/* ---- the plan from the selection ------------------------------------------- */

/* What building the plan gathers. */
struct builder {
    struct dap_plan *plan;
    size_t max_texts;
    long long missing; /* selected, but not in the library or not readable */
};

/* Adds the file at src (in the music folder) to the plan; one nylm can not
 * read or name counts as missing. 0, or -1 with why in err. */
static int add_file(struct builder *b, const char *src, char *err, size_t errlen)
{
    struct stat sb;
    char dest[TAGS_MAX_PATH];
    const char *root = music_root();
    if (!music_inside(src) || strlen(src) <= strlen(root) || lstat(src, &sb) != 0 ||
        !S_ISREG(sb.st_mode) || dap_dest_path(src + strlen(root) + 1, dest, sizeof dest) != 0) {
        fprintf(stderr, "dap: %s: not in the library, or can not be read or named\n", src);
        b->missing++;
        return 0;
    }
    if (b->plan->nfiles >= DAP_MAX_FILES) {
        snprintf(err, errlen, "the selection has more than %d files", DAP_MAX_FILES);
        return -1;
    }
    struct dap_file *f = &b->plan->files[b->plan->nfiles];
    f->src = arena_strndup(src, strlen(src));
    f->dest = arena_strndup(dest, strlen(dest));
    f->size = (long long)sb.st_size;
    f->mtime = (long long)sb.st_mtime;
    if (f->src == NULL || f->dest == NULL) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    b->plan->nfiles++;
    return 0;
}

/* Adds the file at each path st gives (its first column). 0, or -1. */
static int add_rows(struct builder *b, sqlite3_stmt *st, char *err, size_t errlen)
{
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW)
        if (add_file(b, (const char *)sqlite3_column_text(st, 0), err, errlen) != 0)
            return -1;
    if (rc != SQLITE_DONE) {
        db_log_error(music_db, "dap: selection");
        snprintf(err, errlen, "database error (see the log)");
        return -1;
    }
    return 0;
}

/* 1 if name is a picture that goes along with its folder's tracks. */
static int picture(const char *name)
{
    const char *dot = strrchr(name, '.');
    return name[0] != '.' && dot != NULL &&
           (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0 ||
            strcasecmp(dot, ".png") == 0);
}

static int by_src(const void *a, const void *b)
{
    return strcmp(((const struct dap_file *)a)->src, ((const struct dap_file *)b)->src);
}

/* Sorts the files by source and drops repeats. */
static void unique_files(struct dap_plan *plan)
{
    qsort(plan->files, plan->nfiles, sizeof *plan->files, by_src);
    size_t n = 0;
    for (size_t i = 0; i < plan->nfiles; i++)
        if (n == 0 || strcmp(plan->files[n - 1].src, plan->files[i].src) != 0)
            plan->files[n++] = plan->files[i];
    plan->nfiles = n;
}

/* Adds the pictures (JPEG, PNG) of the folders of the planned tracks. */
static int add_pictures(struct builder *b, char *err, size_t errlen)
{
    unique_files(b->plan);
    size_t tracks = b->plan->nfiles;
    const char *last = NULL;
    size_t last_len = 0;
    for (size_t i = 0; i < tracks; i++) {
        const char *src = b->plan->files[i].src;
        size_t len = (size_t)(strrchr(src, '/') - src);
        if (last != NULL && len == last_len && strncmp(src, last, len) == 0)
            continue;
        last = src;
        last_len = len;
        char folder[TAGS_MAX_PATH];
        snprintf(folder, sizeof folder, "%.*s", (int)len, src);
        DIR *d = opendir(folder);
        struct dirent *e;
        while (d != NULL && (e = readdir(d)) != NULL) {
            char path[TAGS_MAX_PATH];
            struct stat sb;
            int w = snprintf(path, sizeof path, "%s/%s", folder, e->d_name);
            if (!picture(e->d_name) || w < 0 || (size_t)w >= sizeof path ||
                lstat(path, &sb) != 0 || !S_ISREG(sb.st_mode) || sb.st_size > MAX_PICTURE)
                continue;
            if (add_file(b, path, err, errlen) != 0) {
                closedir(d);
                return -1;
            }
        }
        if (d != NULL)
            closedir(d);
    }
    unique_files(b->plan);
    return 0;
}

/* The path below DAP_FOLDER of a track in the library, in the arena. */
static const char *dest_of(const char *src)
{
    char dest[TAGS_MAX_PATH];
    const char *root = music_root();
    if (strlen(src) <= strlen(root) ||
        dap_dest_path(src + strlen(root) + 1, dest, sizeof dest) != 0)
        return NULL;
    return arena_strndup(dest, strlen(dest));
}

/* A JSON string item of obj, or NULL. */
static const char *text_of(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) && text_valid(v->valuestring, 0) ? v->valuestring : NULL;
}

/*
 * Adds Navidrome's playlist id: the songs the library has, and the
 * playlist as DAP_FOLDER/NAME.m3u8 (NAME: its name now, else name). 0, or
 * -1 with why in err (Navidrome does not answer: nothing must change).
 */
static int add_playlist(struct builder *b, sqlite3_stmt *known, const char *id,
                        const char *name, char *err, size_t errlen)
{
    char extra[NAVIDROME_MAX_ID + 8], why[512];
    if (!navidrome_id_valid(id)) {
        snprintf(err, errlen, "a playlist's id is not valid");
        return -1;
    }
    snprintf(extra, sizeof extra, "&id=%s", id);
    cJSON *r = navidrome_call("getPlaylist", extra, NULL, NULL, NULL, why, sizeof why);
    if (r == NULL) {
        snprintf(err, errlen, "playlist %s: %s", name, why);
        return -1;
    }
    const cJSON *pl = cJSON_GetObjectItemCaseSensitive(r, "playlist");
    const cJSON *entries = cJSON_GetObjectItemCaseSensitive(pl, "entry");
    int count = cJSON_GetArraySize(entries);
    struct dap_song *songs = arena_alloc(((size_t)count + 1) * sizeof *songs);
    if (songs == NULL || b->plan->ntexts >= b->max_texts) {
        snprintf(err, errlen, songs == NULL ? "out of memory" : "too many playlists");
        return -1;
    }
    size_t n = 0;
    const cJSON *s;
    cJSON_ArrayForEach(s, entries) {
        const char *path = text_of(s, "path");
        char local[TAGS_MAX_PATH];
        int rc = path != NULL && navidrome_local_path(navidrome_music(), music_root(), path,
                                                      local, sizeof local) == 0
                     ? sqlite3_bind_text(known, 1, local, -1, SQLITE_STATIC)
                     : SQLITE_MISUSE;
        rc = rc == SQLITE_OK ? sqlite3_step(known) : rc;
        sqlite3_reset(known);
        if (rc != SQLITE_ROW) {
            b->missing++;
            continue;
        }
        size_t before = b->plan->nfiles;
        if (add_file(b, local, err, errlen) != 0)
            return -1;
        const char *dest = b->plan->nfiles > before ? dest_of(local) : NULL;
        if (dest == NULL)
            continue;
        const cJSON *duration = cJSON_GetObjectItemCaseSensitive(s, "duration");
        songs[n++] = (struct dap_song){ dest, text_of(s, "artist"), text_of(s, "title"),
                                        cJSON_IsNumber(duration) ? (long)duration->valuedouble
                                                                 : -1 };
    }
    const char *now = text_of(pl, "name");
    char safe[MOVE_MAX_NAME + 1], dest[MOVE_MAX_NAME + 8];
    move_safe_name(now != NULL ? now : name, safe, MOVE_MAX_NAME - 5);
    snprintf(dest, sizeof dest, "%s.m3u8", safe);
    struct dap_text *t = &b->plan->texts[b->plan->ntexts];
    t->dest = arena_strndup(dest, strlen(dest));
    t->text = dap_m3u8(songs, n);
    if (t->dest == NULL || t->text == NULL) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    b->plan->ntexts++;
    return 0;
}

/*
 * The plan for the DAP uuid from its selection (dap_items): each album's
 * and track's file, each playlist's songs and its M3U8, the pictures of
 * their folders. 0, or -1 with why in err.
 */
static int build_plan(const char *uuid, struct builder *b, char *err, size_t errlen)
{
    sqlite3_stmt *items = db_prepare(music_db,
        "SELECT kind, album, albumartist, track_id, playlist, name FROM dap_items"
        " WHERE uuid = ? ORDER BY id");
    sqlite3_stmt *album = db_prepare(music_db,
        "SELECT path FROM tracks WHERE album IS ? AND albumartist IS ? ORDER BY path");
    sqlite3_stmt *track = db_prepare(music_db, "SELECT path FROM tracks WHERE id = ?");
    sqlite3_stmt *known = db_prepare(music_db, "SELECT 1 FROM tracks WHERE path = ?");
    int rc = items != NULL && album != NULL && track != NULL && known != NULL &&
                     sqlite3_bind_text(items, 1, uuid, -1, SQLITE_STATIC) == SQLITE_OK
                 ? 0
                 : -1;
    if (rc != 0)
        snprintf(err, errlen, "database error (see the log)");
    int step;
    while (rc == 0 && (step = sqlite3_step(items)) == SQLITE_ROW) {
        const char *kind = (const char *)sqlite3_column_text(items, 0);
        if (strcmp(kind, "album") == 0) {
            rc = sqlite3_bind_value(album, 1, sqlite3_column_value(items, 1)) == SQLITE_OK &&
                         sqlite3_bind_value(album, 2, sqlite3_column_value(items, 2)) == SQLITE_OK
                     ? add_rows(b, album, err, errlen)
                     : -1;
            sqlite3_reset(album);
        } else if (strcmp(kind, "track") == 0) {
            long long before = b->missing;
            size_t files = b->plan->nfiles;
            rc = sqlite3_bind_int64(track, 1, sqlite3_column_int64(items, 3)) == SQLITE_OK
                     ? add_rows(b, track, err, errlen)
                     : -1;
            if (rc == 0 && b->plan->nfiles == files && b->missing == before)
                b->missing++; /* no longer in the library */
            sqlite3_reset(track);
        } else if (navidrome_url() == NULL) {
            snprintf(err, errlen, "the selection has a playlist, but Navidrome is not set up");
            rc = -1;
        } else {
            rc = add_playlist(b, known, (const char *)sqlite3_column_text(items, 4),
                              (const char *)sqlite3_column_text(items, 5), err, errlen);
        }
    }
    if (rc == 0 && step != SQLITE_DONE) {
        db_log_error(music_db, "dap: selection");
        snprintf(err, errlen, "database error (see the log)");
        rc = -1;
    }
    sqlite3_finalize(items);
    sqlite3_finalize(album);
    sqlite3_finalize(track);
    sqlite3_finalize(known);
    return rc == 0 ? add_pictures(b, err, errlen) : -1;
}

/* ---- the service ------------------------------------------------------------ */

/* What the sync records as it goes. */
struct run {
    long long id;
    double saved; /* when the progress was last recorded */
    char note[MAX_NOTE];
};

/* Records the progress of sync id. 0 or -1 (logged). */
static int save_progress(long long id, const struct dap_progress *p)
{
    sqlite3_stmt *st = db_prepare(music_db,
        "UPDATE dap_syncs SET copied = ?, kept = ?, deleted = ?, failed = ?, bytes = ?,"
        " total = ? WHERE id = ?");
    int ok = st != NULL && sqlite3_bind_int64(st, 1, p->copied) == SQLITE_OK &&
             sqlite3_bind_int64(st, 2, p->kept) == SQLITE_OK &&
             sqlite3_bind_int64(st, 3, p->deleted) == SQLITE_OK &&
             sqlite3_bind_int64(st, 4, p->failed) == SQLITE_OK &&
             sqlite3_bind_int64(st, 5, p->bytes) == SQLITE_OK &&
             sqlite3_bind_int64(st, 6, p->total) == SQLITE_OK &&
             sqlite3_bind_int64(st, 7, id) == SQLITE_OK && sqlite3_step(st) == SQLITE_DONE;
    if (!ok)
        db_log_error(music_db, "dap: progress");
    sqlite3_finalize(st);
    return ok ? 0 : -1;
}

/* dap_apply()'s report: logs failures, keeps the first in the note, and
 * records the progress about once a second. */
static int report(const struct dap_progress *p, const char *what, const char *path,
                  const char *why, void *ctx)
{
    struct run *r = ctx;
    if (why != NULL) {
        fprintf(stderr, "dap: %s: %s\n", path, why);
        size_t len = strlen(r->note);
        if (len + 8 < sizeof r->note)
            snprintf(r->note + len, sizeof r->note - len, "%s%s: %s", len > 0 ? "; " : "", path,
                     why);
    }
    (void)what;
    if (now_seconds() - r->saved < 1.0)
        return 0;
    r->saved = now_seconds();
    return save_progress(r->id, p);
}

/*
 * Checks that DAP_MOUNT is the FAT or exFAT mount of the DAP uuid (not the
 * music folder's file system) and opens DAP_FOLDER on it (made if missing).
 * The handle, with *fat set; or -1 with why in err.
 */
static int open_dap(const char *uuid, int *fat, char *err, size_t errlen)
{
    struct stat mount, parent, library;
    struct statfs fs;
    if (stat(DAP_MOUNT, &mount) != 0 || stat(DAP_MOUNT "/..", &parent) != 0 ||
        mount.st_dev == parent.st_dev) {
        snprintf(err, errlen, "no DAP is mounted at %s", DAP_MOUNT);
        return -1;
    }
    if (statfs(DAP_MOUNT, &fs) != 0 ||
        (fs.f_type != MSDOS_MAGIC && (unsigned long)fs.f_type != EXFAT_MAGIC)) {
        snprintf(err, errlen, "%s is not a FAT or exFAT file system", DAP_MOUNT);
        return -1;
    }
    if (stat(music_root(), &library) != 0 || library.st_dev == mount.st_dev) {
        snprintf(err, errlen, "%s is the music folder's file system", DAP_MOUNT);
        return -1;
    }
    char majmin[24], path[64];
    struct dap_drive d;
    const char *mounts = sysinfo_read_file("/proc/self/mountinfo", 1024 * 1024);
    const char *udev = NULL;
    if (mounts != NULL && dap_device_at(mounts, DAP_MOUNT, majmin, sizeof majmin)) {
        snprintf(path, sizeof path, "/run/udev/data/b%s", majmin);
        udev = sysinfo_read_file(path, 64 * 1024);
    }
    if (udev == NULL || !dap_udev_parse(udev, &d) || strcmp(d.uuid, uuid) != 0) {
        snprintf(err, errlen, "the drive mounted at %s is not the DAP this sync is for (%s)",
                 DAP_MOUNT, uuid);
        return -1;
    }
    *fat = fs.f_type == MSDOS_MAGIC;
    int top = open(DAP_MOUNT, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat sb;
    if (top < 0 || fstat(top, &sb) != 0 || sb.st_dev != mount.st_dev) {
        snprintf(err, errlen, "can not open %s", DAP_MOUNT);
        if (top >= 0)
            close(top);
        return -1;
    }
    int dir = -1;
    if (mkdirat(top, DAP_FOLDER, 0755) == 0 || errno == EEXIST)
        dir = openat(top, DAP_FOLDER, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0 || fstat(dir, &sb) != 0 || sb.st_dev != mount.st_dev) {
        snprintf(err, errlen, "can not open %s/%s: %s", DAP_MOUNT, DAP_FOLDER, strerror(errno));
        if (dir >= 0)
            close(dir);
        dir = -1;
    }
    close(top);
    return dir;
}

/* Records how sync id ended. */
static void finish(long long id, const char *state, const char *note)
{
    sqlite3_stmt *st = db_prepare(music_db,
        "UPDATE dap_syncs SET state = ?, note = ?, finished = unixepoch() WHERE id = ?");
    if (st == NULL || sqlite3_bind_text(st, 1, state, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_text(st, 2, note, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_int64(st, 3, id) != SQLITE_OK || sqlite3_step(st) != SQLITE_DONE)
        db_log_error(music_db, "dap: finish a sync");
    sqlite3_finalize(st);
}

/* Runs sync id for the DAP uuid. 0, or -1 (recorded). */
static int run_sync(long long id, const char *uuid)
{
    char err[700] = "";
    int fat = 0;
    int dir = open_dap(uuid, &fat, err, sizeof err);
    struct dap_plan plan = { arena_alloc(DAP_MAX_FILES * sizeof *plan.files), 0,
                             arena_alloc(DAP_MAX_FILES * sizeof *plan.texts), 0 };
    struct builder b = { &plan, DAP_MAX_FILES, 0 };
    if (dir >= 0 && (plan.files == NULL || plan.texts == NULL)) {
        snprintf(err, sizeof err, "out of memory");
        close(dir);
        dir = -1;
    }
    if (dir >= 0 && build_plan(uuid, &b, err, sizeof err) != 0) {
        size_t len = strlen(err);
        snprintf(err + len, sizeof err - len, ": nothing on the DAP was changed");
        close(dir);
        dir = -1;
    }
    if (dir < 0) {
        fprintf(stderr, "dap: sync %lld: %s\n", id, err);
        finish(id, "failed", err);
        return -1;
    }
    sqlite3_stmt *st = db_prepare(music_db, "UPDATE dap_syncs SET files = ?, missing = ?"
                                            " WHERE id = ?");
    if (st == NULL ||
        sqlite3_bind_int64(st, 1, (long long)(plan.nfiles + plan.ntexts)) != SQLITE_OK ||
        sqlite3_bind_int64(st, 2, b.missing) != SQLITE_OK ||
        sqlite3_bind_int64(st, 3, id) != SQLITE_OK || sqlite3_step(st) != SQLITE_DONE)
        db_log_error(music_db, "dap: plan");
    sqlite3_finalize(st);
    printf("dap: sync %lld: %zu files and %zu playlists for %s; %lld selected files are not "
           "in the library\n", id, plan.nfiles, plan.ntexts, uuid, b.missing);
    fflush(stdout);

    struct run r = { id, now_seconds(), "" };
    struct dap_progress p;
    int rc = dap_apply(dir, &plan, fat, &p, report, &r, err, sizeof err);
    /* What is written goes to the drive now, before anyone unplugs it. */
    if (syncfs(dir) != 0)
        fprintf(stderr, "dap: writing out: %s\n", strerror(errno));
    close(dir);
    save_progress(id, &p);
    printf("dap: sync %lld: %lld copied, %lld already there, %lld deleted, %lld failed; "
           "%lld bytes copied\n", id, p.copied, p.kept, p.deleted, p.failed, p.bytes);
    if (rc != 0) {
        fprintf(stderr, "dap: sync %lld stopped: %s\n", id, err);
        finish(id, "failed", err);
        return -1;
    }
    finish(id, p.failed > 0 ? "warning" : "done", r.note);
    return 0;
}

int dap_sync(void)
{
    if (music_root() == NULL) {
        fprintf(stderr, "dap: NYLM_MUSIC is not set\n");
        return 1;
    }
    int lock = music_dap_lock(1);
    if (lock < 0)
        return 1;
    /* No service changes the library's files while they are copied. */
    int library = music_lock_shared();
    if (library < 0) {
        fprintf(stderr, "dap: %s\n", library == MUSIC_BUSY
                                         ? "the library is busy: a scan, write or move runs"
                                         : "can not take the library lock");
        music_unlock(lock);
        return 1;
    }
    /* A sync still "running" was cut off (the lock says none runs now). */
    int rc = db_exec(music_db, "UPDATE dap_syncs SET state = 'failed', finished = unixepoch(),"
                               " note = 'cut off: check the DAP and sync again'"
                               " WHERE state = 'running'");
    sqlite3_stmt *st = rc == 0 ? db_prepare(music_db,
        "SELECT id, uuid FROM dap_syncs WHERE state = 'queued' ORDER BY id LIMIT 1") : NULL;
    int step = st != NULL ? sqlite3_step(st) : SQLITE_ERROR;
    long long id = step == SQLITE_ROW ? sqlite3_column_int64(st, 0) : 0;
    char uuid[DAP_UUID_LEN + 1] = "";
    if (step == SQLITE_ROW)
        snprintf(uuid, sizeof uuid, "%s", (const char *)sqlite3_column_text(st, 1));
    else if (step != SQLITE_DONE)
        db_log_error(music_db, "dap: next sync");
    sqlite3_finalize(st);
    if (step == SQLITE_ROW) {
        st = db_prepare(music_db, "UPDATE dap_syncs SET state = 'running', started = unixepoch()"
                                  " WHERE id = ?");
        if (st == NULL || sqlite3_bind_int64(st, 1, id) != SQLITE_OK ||
            sqlite3_step(st) != SQLITE_DONE) {
            db_log_error(music_db, "dap: start a sync");
            step = SQLITE_ERROR;
        }
        sqlite3_finalize(st);
    }
    if (step == SQLITE_ROW)
        rc = run_sync(id, uuid);
    else if (step == SQLITE_DONE)
        printf("dap: no sync is queued\n");
    else
        rc = -1;
    music_unlock(library);
    music_unlock(lock);
    return rc == 0 ? 0 : 1;
}
