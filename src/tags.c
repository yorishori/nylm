/*
 * Music file tags through TagLib's C API (see tags.h). TagLib reads and
 * writes the files; nylm never touches their bytes itself.
 *
 * After a write the file is read back: the changed tags must read as asked,
 * and every other property, the pictures and the audio properties are
 * compared with before; what differs is reported (TagLib normalises some
 * tags on save, e.g. drops a track number "0/0").
 *
 * TagLib's C API saves MP3 tags as ID3v2.4 (upgrading ID3v2.3) and adds an
 * ID3v1 tag at the end. Both are kept as TagLib writes them.
 */
#define _POSIX_C_SOURCE 200809L

#include "tags.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

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

#define MAX_KEYS     1000 /* properties in one file */
#define MAX_PICTURES 32

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

int tags_field_of(const char *name)
{
    for (int i = 0; i < TAG_FIELDS; i++)
        if (strcmp(tags_name[i], name) == 0)
            return i;
    return -1;
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

/* Genres: lowercase a-z and '-', several separated by "; " ("rock; pop-punk"). */
static int genre_valid(const char *s)
{
    for (;;) {
        size_t n = strspn(s, "abcdefghijklmnopqrstuvwxyz-");
        if (n == 0)
            return 0;
        s += n;
        if (*s == '\0')
            return 1;
        if (s[0] != ';' || s[1] != ' ')
            return 0;
        s += 2;
    }
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
                       field == TAG_ALBUMARTIST || field == TAG_TRACKNUMBER ||
                       field == TAG_DISCNUMBER;
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
    case TAG_GENRE:
        return genre_valid(value) ? NULL
                                  : "must be lowercase a-z and -, several separated by \"; \"";
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

/*
 * 1 if key has, in after, the values it should have: the new value if set[]
 * changes it ("" = absent), else the values it had before.
 */
static int key_as_expected(const char *key, const struct propmap *before,
                           const struct propmap *after, const char *const *set)
{
    const char *one[1];
    const char **want = NULL;
    size_t want_n = 0;
    int field = -1;
    for (int i = 0; i < TAG_FIELDS; i++)
        if (strcmp(tags_key[i], key) == 0)
            field = i;
    if (field >= 0 && set[field] != NULL) {
        if (set[field][0] != '\0') {
            one[0] = set[field];
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

/* ---- writing ------------------------------------------------------------ */

/* Marks a change failed with a note. */
static void fail(struct tags_change *c, const char *fmt, const char *a, const char *b)
{
    c->failed = 1;
    snprintf(c->note, sizeof c->note, fmt, a, b);
}

/* Fails every change not failed yet with the same note. */
static void fail_rest(struct tags_change *c, int n, const char *why, const char *detail)
{
    for (int i = 0; i < n; i++)
        if (!c[i].failed)
            fail(&c[i], "%s%s", why, detail);
}

/* Appends ", what" to the list in buf. */
static void list_add(char *buf, size_t size, const char *what)
{
    size_t len = strlen(buf);
    if (len < size)
        snprintf(buf + len, size - len, "%s%s", len > 0 ? ", " : "", what);
}

/* The changes not failed, as set[field] = value. Their count. */
static int to_set(const struct tags_change *c, int n, const char **set)
{
    int count = 0;
    for (int i = 0; i < TAG_FIELDS; i++)
        set[i] = NULL;
    for (int i = 0; i < n; i++)
        if (!c[i].failed) {
            set[c[i].field] = c[i].value;
            count++;
        }
    return count;
}

/* Fails the changes whose value is invalid, or whose tag is changed twice. */
static void check_values(struct tags_change *c, int n)
{
    unsigned seen = 0;
    for (int i = 0; i < n; i++) {
        const char *why = tags_check_value(c[i].field, c[i].value);
        if (why != NULL)
            fail(&c[i], "invalid value: %s %s", tags_name[c[i].field], why);
        else if (seen & (1u << c[i].field))
            fail(&c[i], "%s is changed twice%s", tags_name[c[i].field], "");
        seen |= 1u << c[i].field;
    }
}

/* 1 if old is p's values joined by "; " (as tags_read shows them); an
 * absent tag matches only NULL. */
static int same_as_queued(const char *old, const struct prop *p)
{
    if (p == NULL || p->n == 0)
        return old == NULL;
    if (old == NULL)
        return 0;
    for (size_t i = 0; i < p->n; i++) {
        size_t len = strlen(p->values[i]);
        if (strncmp(old, p->values[i], len) != 0)
            return 0;
        old += len;
        if (i + 1 < p->n) {
            if (strncmp(old, "; ", 2) != 0)
                return 0;
            old += 2;
        }
    }
    return *old == '\0';
}

/*
 * Fails the changes whose tag no longer has the value it was queued
 * against, or has several values (a genre may: its values are replaced by
 * the one new string).
 */
static void check_old_values(struct tags_change *c, int n, const struct propmap *before)
{
    for (int i = 0; i < n; i++) {
        if (c[i].failed)
            continue;
        const struct prop *p = find(before, tags_key[c[i].field]);
        if (p != NULL && p->n > 1 && c[i].field != TAG_GENRE)
            fail(&c[i], "%s has several values in the file; nylm does not change those%s",
                 tags_name[c[i].field], "");
        else if (!same_as_queued(c[i].old, p))
            fail(&c[i], "the file changed since this was queued: it now has %.180s%s",
                 p != NULL && p->n > 0 ? p->values[0] : "no value",
                 p != NULL && p->n > 1 ? "; …" : "");
    }
}

int tags_write(const char *path, enum tags_format format, struct tags_change *c, int n)
{
    for (int i = 0; i < n; i++) {
        c[i].failed = 0;
        c[i].note[0] = '\0';
    }
    check_values(c, n);

    struct stat sb;
    if (lstat(path, &sb) != 0) {
        fail_rest(c, n, "file not found: ", strerror(errno));
        return 0;
    }
    if (!S_ISREG(sb.st_mode)) {
        fail_rest(c, n, "not a regular file", "");
        return 0;
    }
    TagLib_File *f = open_file(path, format);
    if (f == NULL) {
        fail_rest(c, n, "TagLib can not read the file", "");
        return 0;
    }
    char err[TAGS_MAX_NOTE];
    struct propmap before;
    struct audio audio_before;
    if (load_map(f, &before, err, sizeof err) != 0 || read_audio(f, &audio_before) != 0) {
        taglib_file_free(f);
        fail_rest(c, n, "TagLib can not read the file's tags", "");
        return 0;
    }
    check_old_values(c, n, &before);

    const char *set[TAG_FIELDS];
    if (to_set(c, n, set) == 0) {
        taglib_file_free(f);
        return 0;
    }
    for (int i = 0; i < TAG_FIELDS; i++)
        if (set[i] != NULL)
            taglib_property_set(f, tags_key[i], set[i][0] != '\0' ? set[i] : NULL);
    int saved = taglib_file_save(f);
    taglib_file_free(f);
    if (!saved) {
        /* TagLib may have written part of the file before failing. */
        fail_rest(c, n, "TagLib could not save the file; check that it still plays", "");
        fprintf(stderr, "tags: TagLib could not save %s\n", path);
        return 0;
    }

    /* Read it back: the changes as asked, and what else changed. */
    struct propmap after;
    struct audio audio_after;
    f = open_file(path, format);
    if (f == NULL || load_map(f, &after, err, sizeof err) != 0 ||
        read_audio(f, &audio_after) != 0) {
        if (f != NULL)
            taglib_file_free(f);
        fail_rest(c, n, "after saving, TagLib can not read the file; check that it still plays",
                  "");
        fprintf(stderr, "tags: can not read %s after saving it\n", path);
        return 1;
    }
    taglib_file_free(f);
    for (int i = 0; i < n; i++) {
        if (c[i].failed || key_as_expected(tags_key[c[i].field], &before, &after, set))
            continue;
        const struct prop *p = find(&after, tags_key[c[i].field]);
        fail(&c[i], "after saving, the tag reads %.200s%s",
             p == NULL || p->n == 0 ? "no value" : p->values[0], p != NULL && p->n > 1 ? ", …" : "");
    }

    char also[TAGS_MAX_NOTE - 32] = "";
    for (size_t i = 0; i < before.n; i++)
        if (!key_as_expected(before.props[i].key, &before, &after, set))
            list_add(also, sizeof also, before.props[i].key);
    for (size_t i = 0; i < after.n; i++)
        if (find(&before, after.props[i].key) == NULL &&
            !key_as_expected(after.props[i].key, &before, &after, set))
            list_add(also, sizeof also, after.props[i].key);
    if (memcmp(&audio_before, &audio_after, sizeof audio_before) != 0)
        list_add(also, sizeof also, "audio properties or pictures");
    if (also[0] != '\0')
        for (int i = 0; i < n; i++)
            if (!c[i].failed)
                snprintf(c[i].note, sizeof c[i].note, "TagLib also changed: %s", also);
    return 1;
}
