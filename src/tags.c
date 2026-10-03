/*
 * Music file tags through TagLib's C API (see tags.h for the write steps).
 *
 * What a write may change is checked by reading the copy back: every
 * property TagLib knows must be exactly as before, except the ones asked
 * for; the embedded pictures and the audio properties must be the same.
 * If anything differs the copy is deleted and the original is untouched.
 *
 * TagLib's C API always saves MP3 tags as ID3v2.4 and adds an ID3v1 tag at
 * the end. The ID3v1 tag is removed again when the original had none.
 */
#define _POSIX_C_SOURCE 200809L

#include "tags.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <taglib/tag_c.h>

#include "arena.h"
#include "json.h"

const char *const tags_key[TAG_FIELDS] = {
    "TITLE", "ARTIST", "ALBUM", "ALBUMARTIST", "GENRE", "DATE", "TRACKNUMBER", "DISCNUMBER",
    "COMPILATION",
};

const char *const tags_name[TAG_FIELDS] = {
    "title", "artist", "album", "albumartist", "genre", "date", "tracknumber", "discnumber",
    "compilation",
};

#define MAX_FILE     ((off_t)4 << 30) /* 4 GiB: larger files are refused */
#define MAX_KEYS     1000             /* properties in one file */
#define MAX_PICTURES 32
#define ID3V1_SIZE   128

enum tags_format tags_format_of(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL)
        return TAGS_NONE;
    if (strcasecmp(dot, ".mp3") == 0)
        return TAGS_MP3;
    if (strcasecmp(dot, ".flac") == 0)
        return TAGS_FLAC;
    return TAGS_NONE;
}

long long tags_mtime(const struct stat *sb)
{
    return (long long)sb->st_mtim.tv_sec * 1000000000LL + (long long)sb->st_mtim.tv_nsec;
}

static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

/*
 * Opens path with TagLib as the given format; NULL unless it is valid and
 * has audio (TagLib accepts any file as MPEG, even one without a frame).
 */
static TagLib_File *open_file(const char *path, enum tags_format format)
{
    TagLib_File *f = taglib_file_new_type(path, format == TAGS_MP3 ? TagLib_File_MPEG
                                                                   : TagLib_File_FLAC);
    const TagLib_AudioProperties *p = f != NULL ? taglib_file_audioproperties(f) : NULL;
    if (f != NULL && (!taglib_file_is_valid(f) || p == NULL ||
                      taglib_audioproperties_samplerate(p) <= 0 ||
                      taglib_audioproperties_channels(p) <= 0)) {
        taglib_file_free(f);
        return NULL;
    }
    return f;
}

static size_t count(char **list)
{
    size_t n = 0;
    while (list != NULL && list[n] != NULL)
        n++;
    return n;
}

/* The n values joined by "; " in the arena; NULL when out of memory. */
static const char *join(char **values, size_t n)
{
    size_t len = 0;
    for (size_t i = 0; i < n; i++)
        len += strlen(values[i]) + 2;
    char *s = arena_alloc(len + 1);
    if (s == NULL)
        return NULL;
    size_t at = 0;
    for (size_t i = 0; i < n; i++) {
        size_t l = strlen(values[i]);
        if (i > 0) {
            memcpy(s + at, "; ", 2);
            at += 2;
        }
        memcpy(s + at, values[i], l);
        at += l;
    }
    s[at] = '\0';
    return s;
}

/* What can be compared about a file besides its properties. */
struct audio {
    int length, bitrate, samplerate, channels;
    int pictures;
    unsigned picture_size[MAX_PICTURES];
};

/* Reads f's audio properties and embedded pictures. 0 or -1. */
static int read_audio(const TagLib_File *f, struct audio *a)
{
    memset(a, 0, sizeof *a);
    const TagLib_AudioProperties *p = taglib_file_audioproperties(f);
    if (p == NULL)
        return -1;
    a->length = taglib_audioproperties_length(p);
    a->bitrate = taglib_audioproperties_bitrate(p);
    a->samplerate = taglib_audioproperties_samplerate(p);
    a->channels = taglib_audioproperties_channels(p);

    TagLib_Complex_Property_Attribute ***pics = taglib_complex_property_get(f, "PICTURE");
    for (int i = 0; pics != NULL && pics[i] != NULL; i++) {
        if (i == MAX_PICTURES) {
            a->pictures = -1;
            break;
        }
        a->pictures = i + 1;
        for (int k = 0; pics[i][k] != NULL; k++)
            if (strcmp(pics[i][k]->key, "data") == 0)
                a->picture_size[i] = pics[i][k]->value.size;
    }
    if (pics != NULL)
        taglib_complex_property_free(pics);
    return a->pictures < 0 ? -1 : 0;
}

int tags_read(const char *path, enum tags_format format, struct tags *out, char *err,
              size_t errlen)
{
    memset(out, 0, sizeof *out);
    TagLib_File *f = open_file(path, format);
    if (f == NULL) {
        set_err(err, errlen, "TagLib can not read it");
        return -1;
    }
    int rc = 0;
    for (int i = 0; i < TAG_FIELDS && rc == 0; i++) {
        char **values = taglib_property_get(f, tags_key[i]);
        size_t n = count(values);
        if (n > 1)
            out->multi |= 1u << i;
        if (n > 0 && (out->value[i] = join(values, n)) == NULL) {
            set_err(err, errlen, "out of memory");
            rc = -1;
        }
        if (values != NULL)
            taglib_property_free(values);
    }
    struct audio a;
    if (rc == 0 && read_audio(f, &a) != 0) {
        set_err(err, errlen, "TagLib can not read its audio properties or pictures");
        rc = -1;
    }
    out->pictures = a.pictures;
    out->seconds = a.length;
    taglib_file_free(f);
    return rc;
}

/* ---- validation --------------------------------------------------------- */

/* The value of n decimal digits at s (n <= 4); -1 if one is not a digit. */
static int digits(const char *s, size_t n)
{
    int v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

/* "YYYY", "YYYY-MM" or "YYYY-MM-DD", a real day. */
static int date_valid(const char *s)
{
    static const int month_days[] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    size_t len = strlen(s);
    if (len != 4 && len != 7 && len != 10)
        return 0;
    int year = digits(s, 4);
    if (year < 0)
        return 0;
    if (len == 4)
        return 1;
    int month = digits(s + 5, 2);
    if (s[4] != '-' || month < 1 || month > 12)
        return 0;
    if (len == 7)
        return 1;
    int day = digits(s + 8, 2);
    int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (s[7] != '-' || day < 1 || day > month_days[month - 1] ||
        (month == 2 && day == 29 && !leap))
        return 0;
    return 1;
}

/* "N" or "N/M": 1 to 4 digits each, N at least 1, M at least N. */
static int number_valid(const char *s)
{
    size_t a = strspn(s, "0123456789");
    if (a < 1 || a > 4)
        return 0;
    int n = digits(s, a);
    if (n < 1)
        return 0;
    if (s[a] == '\0')
        return 1;
    const char *t = s + a + 1;
    size_t b = strspn(t, "0123456789");
    return s[a] == '/' && b >= 1 && b <= 4 && t[b] == '\0' && digits(t, b) >= n;
}

const char *tags_check_value(enum tag_field field, const char *value)
{
    size_t len = strlen(value);
    if (len > TAGS_MAX_VALUE)
        return "is too long (at most 500 bytes)";
    if (!text_valid(value, 0))
        return "must be one line of UTF-8 text";
    if (len == 0) {
        int required = field == TAG_TITLE || field == TAG_ARTIST || field == TAG_ALBUM ||
                       field == TAG_ALBUMARTIST || field == TAG_TRACKNUMBER;
        return required ? "can not be empty" : NULL;
    }
    if (value[0] == ' ' || value[len - 1] == ' ')
        return "must not start or end with a space";
    switch (field) {
    case TAG_DATE:
        return date_valid(value) ? NULL : "must be a date: YYYY, YYYY-MM or YYYY-MM-DD";
    case TAG_TRACKNUMBER:
    case TAG_DISCNUMBER:
        return number_valid(value) ? NULL : "must be a number like 3 or 3/12";
    case TAG_COMPILATION:
        return strcmp(value, "1") == 0 ? NULL : "must be 1 or empty";
    default:
        return NULL;
    }
}

/* ---- property maps ------------------------------------------------------ */

struct prop {
    const char *key;
    size_t n;
    const char **values;
};

struct propmap {
    size_t n;
    struct prop *props;
};

/* Copies every property of f into the arena. 0 or -1 (err set). */
static int load_map(const TagLib_File *f, struct propmap *m, char *err, size_t errlen)
{
    m->n = 0;
    m->props = NULL;
    char **keys = taglib_property_keys(f);
    size_t nkeys = count(keys);
    if (nkeys > MAX_KEYS) {
        taglib_property_free(keys);
        set_err(err, errlen, "has more than %d tags", MAX_KEYS);
        return -1;
    }
    m->props = arena_alloc((nkeys + 1) * sizeof *m->props);
    int rc = m->props == NULL ? -1 : 0;
    for (size_t i = 0; i < nkeys && rc == 0; i++) {
        char **values = taglib_property_get(f, keys[i]);
        size_t n = count(values);
        struct prop *p = &m->props[i];
        p->key = arena_strndup(keys[i], strlen(keys[i]));
        p->values = arena_alloc((n + 1) * sizeof *p->values);
        p->n = n;
        if (p->key == NULL || p->values == NULL)
            rc = -1;
        for (size_t j = 0; j < n && rc == 0; j++)
            if ((p->values[j] = arena_strndup(values[j], strlen(values[j]))) == NULL)
                rc = -1;
        if (values != NULL)
            taglib_property_free(values);
        m->n = i + 1;
    }
    if (keys != NULL)
        taglib_property_free(keys);
    if (rc != 0)
        set_err(err, errlen, "out of memory");
    return rc;
}

static const struct prop *find(const struct propmap *m, const char *key)
{
    for (size_t i = 0; i < m->n; i++)
        if (strcmp(m->props[i].key, key) == 0)
            return &m->props[i];
    return NULL;
}

/* 1 if key has the values it should have after the edit e. */
static int key_as_expected(const char *key, const struct propmap *before,
                           const struct propmap *after, const struct tags_edit *e)
{
    const char *one[1];
    const char **want = NULL;
    size_t want_n = 0;
    int field = -1;
    for (int i = 0; i < TAG_FIELDS; i++)
        if (strcmp(tags_key[i], key) == 0)
            field = i;
    if (field >= 0 && e->set[field] != NULL) {
        if (e->set[field][0] != '\0') {
            one[0] = e->set[field];
            want = one;
            want_n = 1;
        }
    } else {
        const struct prop *p = find(before, key);
        if (p != NULL) {
            want = p->values;
            want_n = p->n;
        }
    }
    const struct prop *have = find(after, key);
    size_t have_n = have != NULL ? have->n : 0;
    if (have_n != want_n)
        return 0;
    for (size_t i = 0; i < want_n; i++)
        if (strcmp(have->values[i], want[i]) != 0)
            return 0;
    return 1;
}

/* Checks the edited copy against the original's properties. 0 or -1. */
static int verify(const struct propmap *before, const struct audio *audio_before,
                  const TagLib_File *copy, const struct tags_edit *e, char *err, size_t errlen)
{
    struct propmap after;
    if (load_map(copy, &after, err, errlen) != 0)
        return -1;
    for (size_t i = 0; i < before->n; i++)
        if (!key_as_expected(before->props[i].key, before, &after, e)) {
            set_err(err, errlen, "TagLib would also change %s; nothing was written", before->props[i].key);
            return -1;
        }
    for (size_t i = 0; i < after.n; i++)
        if (!key_as_expected(after.props[i].key, before, &after, e)) {
            set_err(err, errlen, "TagLib would also change %s; nothing was written", after.props[i].key);
            return -1;
        }
    for (int i = 0; i < TAG_FIELDS; i++)
        if (!key_as_expected(tags_key[i], before, &after, e)) {
            set_err(err, errlen, "TagLib would also change %s; nothing was written", tags_key[i]);
            return -1;
        }

    struct audio a;
    if (read_audio(copy, &a) != 0 || memcmp(&a, audio_before, sizeof a) != 0) {
        set_err(err, errlen, "check failed: audio properties or pictures changed");
        return -1;
    }
    return 0;
}

/* ---- writing ------------------------------------------------------------ */

/* 1 if the size-byte file ends with an ID3v1 tag, 0 if not, -1 on error. */
static int has_id3v1(int fd, off_t size)
{
    if (size < ID3V1_SIZE)
        return 0;
    char tag[3];
    if (pread(fd, tag, sizeof tag, size - ID3V1_SIZE) != (ssize_t)sizeof tag)
        return -1;
    return memcmp(tag, "TAG", 3) == 0;
}

/* Removes the ID3v1 tag TagLib added at the end of path. 0 or -1. */
static int strip_id3v1(const char *path)
{
    int fd = open(path, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct stat sb;
    int rc = fstat(fd, &sb) == 0 ? has_id3v1(fd, sb.st_size) : -1;
    if (rc == 1)
        rc = ftruncate(fd, sb.st_size - ID3V1_SIZE);
    if (close(fd) != 0)
        rc = -1;
    return rc;
}

/* 1 if sb is a regular file with the scanned size and mtime. */
static int unchanged(const struct stat *sb, const struct tags_edit *e)
{
    return S_ISREG(sb->st_mode) && (long long)sb->st_size == e->size &&
           tags_mtime(sb) == e->mtime;
}

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

/* Copies exactly size bytes from in to out. 0, or -1 (also if it grew). */
static int copy_file(int in, int out, off_t size)
{
    static char buf[64 * 1024];
    off_t done = 0;
    for (;;) {
        ssize_t n = read(in, buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return -1;
        if (n == 0)
            break;
        done += n;
        if (done > size || write_all(out, buf, (size_t)n) != 0)
            return -1;
    }
    return done == size ? 0 : -1;
}

/*
 * Makes e->tmp a copy of the original with its mode (and group, if nylm
 * may set it). had_v1: whether the original ends with an ID3v1 tag.
 */
static int make_copy(struct tags_edit *e, const struct stat *sb, int *had_v1, char *err,
                     size_t errlen)
{
    const char *slash = strrchr(e->path, '/');
    int n = slash != NULL ? snprintf(e->tmp, sizeof e->tmp, "%.*s/" TAGS_TMP_PREFIX "XXXXXX",
                                     (int)(slash - e->path), e->path)
                          : -1;
    if (n < 0 || (size_t)n >= sizeof e->tmp) {
        e->tmp[0] = '\0';
        set_err(err, errlen, "path is too long");
        return -1;
    }
    int in = open(e->path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (in < 0) {
        e->tmp[0] = '\0';
        set_err(err, errlen, "can not open it: %s", strerror(errno));
        return -1;
    }
    int out = mkstemp(e->tmp);
    if (out < 0) {
        set_err(err, errlen, "can not create a copy in its folder: %s", strerror(errno));
        e->tmp[0] = '\0';
        close(in);
        return -1;
    }

    struct stat insb;
    int rc = 0;
    if (fstat(in, &insb) != 0 || insb.st_dev != sb->st_dev || insb.st_ino != sb->st_ino) {
        set_err(err, errlen, "changed while opening it");
        rc = -1;
    } else if (copy_file(in, out, sb->st_size) != 0) {
        set_err(err, errlen, "copying it failed: %s", errno ? strerror(errno) : "size changed");
        rc = -1;
    } else if (fchmod(out, sb->st_mode & 0777) != 0) {
        set_err(err, errlen, "can not set the copy's mode: %s", strerror(errno));
        rc = -1;
    } else if ((*had_v1 = e->format == TAGS_MP3 ? has_id3v1(in, sb->st_size) : 0) < 0) {
        set_err(err, errlen, "can not read it: %s", strerror(errno));
        rc = -1;
    } else if (fsync(out) != 0) {
        set_err(err, errlen, "can not write the copy: %s", strerror(errno));
        rc = -1;
    }
    /* Keep the group so others sharing the library keep their access. Not
     * fatal: the folder's setgid bit or ACLs may already take care of it. */
    if (rc == 0 && insb.st_gid != getegid() && fchown(out, (uid_t)-1, insb.st_gid) != 0)
        fprintf(stderr, "tags: %s: can not keep group %u: %s\n", e->path,
                (unsigned)insb.st_gid, strerror(errno));
    if (close(in) != 0 || close(out) != 0) {
        if (rc == 0)
            set_err(err, errlen, "closing the copy failed: %s", strerror(errno));
        rc = -1;
    }
    return rc;
}

/* fsyncs path (a file or folder). 0 or -1. */
static int sync_path(const char *path, int flags)
{
    int fd = open(path, flags | O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int rc = fsync(fd);
    if (close(fd) != 0)
        rc = -1;
    return rc;
}

/* Edits the copy and checks it. 0, TAGS_LOCKED or -1. */
static int edit_copy(struct tags_edit *e, int had_v1, char *err, size_t errlen)
{
    TagLib_File *f = open_file(e->tmp, e->format);
    if (f == NULL) {
        set_err(err, errlen, "TagLib can not read it");
        return -1;
    }
    struct propmap before;
    struct audio audio_before;
    if (load_map(f, &before, err, errlen) != 0) {
        taglib_file_free(f);
        return -1;
    }
    if (read_audio(f, &audio_before) != 0) {
        taglib_file_free(f);
        set_err(err, errlen, "TagLib can not read its audio properties or pictures");
        return -1;
    }
    for (int i = 0; i < TAG_FIELDS; i++) {
        const struct prop *p = find(&before, tags_key[i]);
        if (e->set[i] != NULL && p != NULL && p->n > 1) {
            taglib_file_free(f);
            set_err(err, errlen, "%s has several values; nylm does not change those",
                    tags_name[i]);
            return TAGS_LOCKED;
        }
    }
    for (int i = 0; i < TAG_FIELDS; i++)
        if (e->set[i] != NULL)
            taglib_property_set(f, tags_key[i], e->set[i][0] != '\0' ? e->set[i] : NULL);
    int saved = taglib_file_save(f);
    taglib_file_free(f);
    if (!saved) {
        set_err(err, errlen, "TagLib could not save the copy");
        return -1;
    }
    if (e->format == TAGS_MP3 && !had_v1 && strip_id3v1(e->tmp) != 0) {
        set_err(err, errlen, "can not remove the ID3v1 tag TagLib added");
        return -1;
    }

    f = open_file(e->tmp, e->format);
    if (f == NULL) {
        set_err(err, errlen, "check failed: TagLib can not read the edited copy");
        return -1;
    }
    int rc = verify(&before, &audio_before, f, e, err, errlen);
    taglib_file_free(f);
    if (rc == 0 && sync_path(e->tmp, O_NOFOLLOW) != 0) {
        set_err(err, errlen, "can not write the copy: %s", strerror(errno));
        rc = -1;
    }
    return rc;
}

int tags_prepare(struct tags_edit *e, char *err, size_t errlen)
{
    e->tmp[0] = '\0';
    struct stat sb;
    if (lstat(e->path, &sb) != 0) {
        set_err(err, errlen, "can not find it: %s", strerror(errno));
        return errno == ENOENT ? TAGS_STALE : -1;
    }
    if (!unchanged(&sb, e)) {
        set_err(err, errlen, "changed since the last scan");
        return TAGS_STALE;
    }
    if (sb.st_size > MAX_FILE) {
        set_err(err, errlen, "is larger than 4 GiB");
        return -1;
    }
    int had_v1 = 0;
    int rc = make_copy(e, &sb, &had_v1, err, errlen);
    if (rc == 0)
        rc = edit_copy(e, had_v1, err, errlen);
    if (rc != 0)
        tags_discard(e);
    return rc;
}

int tags_commit(struct tags_edit *e, char *err, size_t errlen)
{
    struct stat sb;
    if (lstat(e->path, &sb) != 0 || !unchanged(&sb, e)) {
        set_err(err, errlen, "changed while it was being edited");
        tags_discard(e);
        return TAGS_STALE;
    }
    if (rename(e->tmp, e->path) != 0) {
        set_err(err, errlen, "can not replace it: %s", strerror(errno));
        tags_discard(e);
        return -1;
    }
    e->tmp[0] = '\0';
    /* The rename is done; syncing the folder only makes it survive a crash
     * sooner. A failure is logged, not reported as a failed write. */
    char dir[TAGS_MAX_PATH];
    const char *slash = strrchr(e->path, '/');
    snprintf(dir, sizeof dir, "%.*s", (int)(slash - e->path), e->path);
    if (sync_path(dir[0] != '\0' ? dir : "/", O_DIRECTORY) != 0)
        fprintf(stderr, "tags: can not sync folder %s: %s\n", dir, strerror(errno));
    return 0;
}

void tags_discard(struct tags_edit *e)
{
    if (e->tmp[0] != '\0' && unlink(e->tmp) != 0)
        fprintf(stderr, "tags: can not delete %s: %s\n", e->tmp, strerror(errno));
    e->tmp[0] = '\0';
}
