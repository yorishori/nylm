/*
 * Music file tags through TagLib's C API (see tags.h). TagLib reads and
 * writes the files; nylm never touches their bytes itself, and only the
 * tags in tags_key[] are read or set.
 *
 * TagLib's C API saves MP3 tags as ID3v2.4 (upgrading ID3v2.3) and adds an
 * ID3v1 tag at the end. Both are kept as TagLib writes them.
 */
#define _POSIX_C_SOURCE 200809L

#include "tags.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <taglib/tag_c.h>

#include "arena.h"
#include "json.h"

const char *const tags_key[TAG_FIELDS] = {
    "TITLE", "ALBUM", "ARTIST", "ALBUMARTIST", "TRACKNUMBER", "DISCNUMBER", "DATE",
    "COMPILATION", "ISRC", "ASIN", "BPM", "COPYRIGHT", "ENCODEDBY", "MOOD", "MEDIA", "LABEL",
    "CATALOGNUMBER", "BARCODE", "TITLESORT", "ALBUMSORT", "ARTISTSORT", "ALBUMARTISTSORT",
    "COMPOSERSORT", "MUSICBRAINZ_TRACKID", "MUSICBRAINZ_ALBUMID", "NAVIDROME_ID", "GENRE",
    "COMPOSER",
};

const char *const tags_name[TAG_FIELDS] = {
    "title", "album", "artist", "albumartist", "tracknumber", "discnumber", "date",
    "compilation", "isrc", "asin", "bpm", "copyright", "encodedby", "mood", "media", "label",
    "catalognumber", "barcode", "titlesort", "albumsort", "artistsort", "albumartistsort",
    "composersort", "musicbrainz_trackid", "musicbrainz_albumid", "navidrome_id", "genre",
    "composer",
};

/* The extensions TagLib opens as music, lower case. */
static const char *const music_ext[] = {
    "mp3", "mp2", "flac", "ogg", "oga", "opus", "m4a", "m4b", "m4p", "mp4", "aac", "wav",
    "aif", "aiff", "aifc", "afc", "ape", "wv", "tta", "mpc", "spx", "wma", "asf", "shn",
    "mka", "dsf", "dff", "dsdiff",
};

/* Each sort tag and the tag it mirrors. */
static const struct {
    enum tag_field sort, source;
} sort_tags[] = {
    { TAG_TITLESORT, TAG_TITLE },
    { TAG_ALBUMSORT, TAG_ALBUM },
    { TAG_ARTISTSORT, TAG_ARTIST },
    { TAG_ALBUMARTISTSORT, TAG_ALBUMARTIST },
    { TAG_COMPOSERSORT, TAG_COMPOSER },
};
#define NSORT (sizeof sort_tags / sizeof sort_tags[0])

int tags_field_of(const char *name)
{
    for (int i = 0; i < TAG_FIELDS; i++)
        if (strcmp(tags_name[i], name) == 0)
            return i;
    return -1;
}

int tags_is_multi(enum tag_field f)
{
    return f == TAG_GENRE || f == TAG_COMPOSER;
}

int tags_is_editable(enum tag_field f)
{
    for (size_t i = 0; i < NSORT; i++)
        if (sort_tags[i].sort == f)
            return 0;
    return f != TAG_NAVIDROME_ID;
}

int tags_is_required(enum tag_field f)
{
    switch (f) {
    case TAG_TITLE:
    case TAG_ALBUM:
    case TAG_ARTIST:
    case TAG_ALBUMARTIST:
    case TAG_TRACKNUMBER:
    case TAG_DISCNUMBER:
    case TAG_DATE:
    case TAG_GENRE:
    case TAG_COMPOSER:
    case TAG_COMPILATION:
        return 1;
    default:
        return 0;
    }
}

int tags_is_music(const char *ext)
{
    for (size_t i = 0; i < sizeof music_ext / sizeof music_ext[0]; i++)
        if (strcmp(ext, music_ext[i]) == 0)
            return 1;
    return 0;
}

int tags_equal(const struct tag_values *a, const struct tag_values *b)
{
    if (a->n != b->n)
        return 0;
    for (size_t i = 0; i < a->n; i++)
        if (strcmp(a->v[i], b->v[i]) != 0)
            return 0;
    return 1;
}

int tags_set_one(struct tags *t, enum tag_field f, const char *s)
{
    t->value[f].n = 0;
    t->value[f].v = NULL;
    if (s == NULL || *s == '\0')
        return 0;
    const char **v = arena_alloc(sizeof *v);
    if (v == NULL)
        return -1;
    v[0] = s;
    t->value[f].n = 1;
    t->value[f].v = v;
    return 0;
}

static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

/* Appends "; text" (or text, if note is empty) to note. */
static void note_add(char *note, size_t size, const char *text)
{
    size_t len = strlen(note);
    if (len < size)
        snprintf(note + len, size - len, "%s%s", len > 0 ? "; " : "", text);
}

/* ---- validation --------------------------------------------------------- */

/* The value of s if it is 1 to max_digits decimal digits (leading zeros
 * allowed) and at least 1, else -1. */
static int positive(const char *s, size_t len, size_t max_digits)
{
    if (len < 1 || len > max_digits)
        return -1;
    int v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v >= 1 ? v : -1;
}

#define NUMBER_DIGITS 4 /* track and disc numbers, year, BPM: at most 9999 */

int tags_number(const char *s, int *x, int *y)
{
    const char *slash = strchr(s, '/');
    if (slash == NULL)
        return -1;
    int a = positive(s, (size_t)(slash - s), NUMBER_DIGITS);
    int b = positive(slash + 1, strlen(slash + 1), NUMBER_DIGITS);
    if (a < 0 || b < 0 || a > b)
        return -1;
    *x = a;
    *y = b;
    return 0;
}

/* A genre: lowercase a-z, 0-9 and '-'. */
static int genre_valid(const char *s)
{
    size_t n = strspn(s, "abcdefghijklmnopqrstuvwxyz0123456789-");
    return n > 0 && s[n] == '\0';
}

const char *tags_check(enum tag_field f, const struct tag_values *v)
{
    if (v->n == 0)
        return tags_is_required(f) ? "is required" : NULL;
    if (!tags_is_multi(f) && v->n > 1)
        return "must have one value";
    if (v->n > TAGS_MAX_VALUES)
        return "has too many values (at most 64)";
    for (size_t i = 0; i < v->n; i++) {
        const char *s = v->v[i];
        size_t len = strlen(s);
        if (len == 0)
            return tags_is_multi(f) ? "must not have an empty value" : "is required";
        if (len > TAGS_MAX_VALUE)
            return "is too long (at most 500 bytes)";
        if (!text_valid(s, 0))
            return "must be UTF-8 text without control characters";
        int x, y;
        switch (f) {
        case TAG_TRACKNUMBER:
        case TAG_DISCNUMBER:
            if (tags_number(s, &x, &y) != 0)
                return "must be two positive whole numbers like 3/12 (the first at most the "
                       "second)";
            break;
        case TAG_DATE:
            if (positive(s, len, NUMBER_DIGITS) < 0)
                return "must be a year: a positive whole number";
            break;
        case TAG_BPM:
            if (positive(s, len, NUMBER_DIGITS) < 0)
                return "must be a positive whole number";
            break;
        case TAG_COMPILATION:
            if (strcmp(s, "0") != 0 && strcmp(s, "1") != 0)
                return "must be 0 or 1";
            break;
        case TAG_GENRE:
            if (!genre_valid(s))
                return "may only use lowercase a-z, 0-9 and -";
            break;
        default:
            break;
        }
    }
    return NULL;
}

/* ---- preparing a write -------------------------------------------------- */

/* s cut to at most max bytes, at a UTF-8 character boundary, in the arena
 * (s itself if short enough); NULL when out of memory. */
static const char *cut(const char *s, size_t max)
{
    size_t len = strlen(s);
    if (len <= max)
        return s;
    len = max;
    while (len > 0 && ((unsigned char)s[len] & 0xc0) == 0x80)
        len--; /* s[len] continues a character: cut before that character */
    return arena_strndup(s, len);
}

/* The values of v joined by "; " in the arena; NULL when out of memory. */
static const char *join(const struct tag_values *v)
{
    size_t len = 0;
    for (size_t i = 0; i < v->n; i++)
        len += strlen(v->v[i]) + 2;
    char *s = arena_alloc(len + 1);
    if (s == NULL)
        return NULL;
    size_t at = 0;
    for (size_t i = 0; i < v->n; i++) {
        size_t l = strlen(v->v[i]);
        if (i > 0) {
            memcpy(s + at, "; ", 2);
            at += 2;
        }
        memcpy(s + at, v->v[i], l);
        at += l;
    }
    s[at] = '\0';
    return s;
}

int tags_prepare(struct tags *t, unsigned changed, char *note, size_t notelen)
{
    for (size_t i = 0; i < NSORT; i++) {
        enum tag_field sort = sort_tags[i].sort, source = sort_tags[i].source;
        if (changed & (1u << source)) {
            const struct tag_values *src = &t->value[source];
            const char *mirror = src->n == 0 ? "" : src->n == 1 ? src->v[0] : join(src);
            if (mirror == NULL || tags_set_one(t, sort, mirror) != 0)
                return -1;
        }
        const struct tag_values *v = &t->value[sort];
        if (v->n == 1 && strlen(v->v[0]) > TAGS_MAX_VALUE) {
            const char *shorter = cut(v->v[0], TAGS_MAX_VALUE);
            if (shorter == NULL || tags_set_one(t, sort, shorter) != 0)
                return -1;
            char what[64];
            snprintf(what, sizeof what, "%s cut to 500 bytes", tags_key[sort]);
            note_add(note, notelen, what);
        }
    }
    if (tags_check(TAG_DISCNUMBER, &t->value[TAG_DISCNUMBER]) != NULL) {
        if (tags_set_one(t, TAG_DISCNUMBER, "1/1") != 0)
            return -1;
        note_add(note, notelen, "disc number set to 1/1");
    }
    return 0;
}

/* ---- reading ------------------------------------------------------------ */

/*
 * Opens path with TagLib (the type from its extension, as FileRef does);
 * NULL unless it is valid and has audio (TagLib accepts any file as MPEG,
 * even one without a frame).
 */
static TagLib_File *open_file(const char *path)
{
    TagLib_File *f = taglib_file_new(path);
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

/* A picture's type or description as kept: at most 500 bytes, and "" if
 * absent or not UTF-8 text without control characters. In the arena; NULL
 * when out of memory. */
static const char *picture_text(const char *s)
{
    if (s == NULL)
        return "";
    const char *t = cut(s, TAGS_MAX_VALUE);
    if (t == s)
        t = arena_strndup(s, strlen(s));
    return t != NULL && !text_valid(t, 0) ? "" : t;
}

/*
 * Reads the first TAGS_MAX_PICTURES pictures of the open file f into out,
 * passing each to on_picture (if not NULL). 0, -1 when out of memory, -2
 * when on_picture failed.
 */
static int read_pictures(const TagLib_File *f, struct tags *out, tags_picture_fn on_picture,
                         void *ctx)
{
    TagLib_Complex_Property_Attribute ***pics = taglib_complex_property_get(f, "PICTURE");
    size_t n = 0;
    while (pics != NULL && pics[n] != NULL)
        n++;
    if (n > TAGS_MAX_PICTURES)
        n = TAGS_MAX_PICTURES;
    struct tag_picture *list = n > 0 ? arena_alloc(n * sizeof *list) : NULL;
    int rc = n > 0 && list == NULL ? -1 : 0;
    size_t kept = 0;
    for (size_t i = 0; i < n && rc == 0; i++) {
        const unsigned char *data = NULL;
        size_t size = 0;
        const char *type = NULL, *description = NULL;
        for (TagLib_Complex_Property_Attribute **a = pics[i]; *a != NULL; a++) {
            const TagLib_Variant *v = &(*a)->value;
            if (strcmp((*a)->key, "data") == 0 && v->type == TagLib_Variant_ByteVector) {
                data = (const unsigned char *)v->value.byteVectorValue;
                size = v->size;
            } else if (strcmp((*a)->key, "pictureType") == 0 && v->type == TagLib_Variant_String) {
                type = v->value.stringValue;
            } else if (strcmp((*a)->key, "description") == 0 &&
                       v->type == TagLib_Variant_String) {
                description = v->value.stringValue;
            }
        }
        if (data == NULL)
            continue; /* no picture in it */
        struct tag_picture *p = &list[kept];
        if (art_hash(data, size, p->hash) != 0 || (p->type = picture_text(type)) == NULL ||
            (p->description = picture_text(description)) == NULL)
            rc = -1;
        else if (on_picture != NULL && on_picture(ctx, p, data, size) != 0)
            rc = -2;
        else
            kept++;
    }
    if (pics != NULL)
        taglib_complex_property_free(pics);
    out->pictures = list;
    out->npictures = rc == 0 ? kept : 0;
    return rc;
}

/* Reads every tag and the pictures of the open file f into out. 0, -1 when
 * out of memory, -2 when on_picture failed. */
static int read_open(const TagLib_File *f, struct tags *out, tags_picture_fn on_picture,
                     void *ctx)
{
    memset(out, 0, sizeof *out);
    int rc = 0;
    for (int i = 0; i < TAG_FIELDS && rc == 0; i++) {
        char **values = taglib_property_get(f, tags_key[i]);
        size_t n = count(values);
        struct tag_values *v = &out->value[i];
        if (n > 0 && (v->v = arena_alloc(n * sizeof *v->v)) == NULL)
            rc = -1;
        for (size_t k = 0; k < n && rc == 0; k++)
            if ((v->v[k] = arena_strndup(values[k], strlen(values[k]))) == NULL)
                rc = -1;
        v->n = rc == 0 ? n : 0;
        if (values != NULL)
            taglib_property_free(values);
        /* Several values of a single-valued tag: shown joined, and replaced
         * by the one value nylm writes. */
        if (rc == 0 && v->n > 1 && i < TAG_SINGLE_FIELDS) {
            const char *joined = join(v);
            if (joined == NULL)
                rc = -1;
            v->n = 1;
            v->v[0] = joined;
        }
    }
    if (rc == 0 && out->value[TAG_COMPILATION].n == 0)
        rc = tags_set_one(out, TAG_COMPILATION, "0");
    return rc == 0 ? read_pictures(f, out, on_picture, ctx) : rc;
}

int tags_read(const char *path, struct tags *out, tags_picture_fn on_picture, void *ctx,
              char *err, size_t errlen)
{
    memset(out, 0, sizeof *out);
    TagLib_File *f = open_file(path);
    if (f == NULL) {
        set_err(err, errlen, "TagLib can not read it");
        return -1;
    }
    int rc = read_open(f, out, on_picture, ctx);
    taglib_file_free(f);
    if (rc != 0)
        set_err(err, errlen, rc == -2 ? "a picture could not be stored" : "out of memory");
    return rc != 0 ? -1 : 0;
}

int tags_same_pictures(const struct tags *a, const struct tags *b)
{
    if (a->npictures != b->npictures)
        return 0;
    for (size_t i = 0; i < a->npictures; i++)
        if (strcmp(a->pictures[i].hash, b->pictures[i].hash) != 0)
            return 0;
    return 1;
}

/* ---- writing ------------------------------------------------------------ */

/* The first field where a and b differ (the pictures: TAG_FIELDS), or -1. */
static int first_difference(const struct tags *a, const struct tags *b)
{
    for (int i = 0; i < TAG_FIELDS; i++)
        if (!tags_equal(&a->value[i], &b->value[i]))
            return i;
    return tags_same_pictures(a, b) ? -1 : TAG_FIELDS;
}

/* A field's name for messages; TAG_FIELDS is the pictures. */
static const char *field_key(int field)
{
    return field < TAG_FIELDS ? tags_key[field] : "the pictures";
}

enum tags_result tags_write(const char *path, const struct tags *now, const struct tags *want,
                            char *err, size_t errlen)
{
    struct stat sb;
    if (lstat(path, &sb) != 0) {
        set_err(err, errlen, "file not found: %s", strerror(errno));
        return TAGS_NOT_WRITTEN;
    }
    if (!S_ISREG(sb.st_mode)) {
        set_err(err, errlen, "not a regular file");
        return TAGS_NOT_WRITTEN;
    }
    TagLib_File *f = open_file(path);
    if (f == NULL) {
        set_err(err, errlen, "TagLib can not read the file");
        return TAGS_NOT_WRITTEN;
    }
    struct tags before;
    if (read_open(f, &before, NULL, NULL) != 0) {
        taglib_file_free(f);
        set_err(err, errlen, "out of memory");
        return TAGS_NOT_WRITTEN;
    }
    int diff = first_difference(&before, now);
    if (diff >= 0) {
        taglib_file_free(f);
        set_err(err, errlen, "the file changed since it was scanned (%s); scan it again",
                field_key(diff));
        return TAGS_NOT_WRITTEN;
    }

    if (!tags_same_pictures(&before, want)) {
        const struct tag_picture *p = want->npictures == 1 ? &want->pictures[0] : NULL;
        if (p == NULL || p->data == NULL || p->mime == NULL || p->size > UINT_MAX) {
            taglib_file_free(f);
            set_err(err, errlen, "only one picture, with its bytes, can be written");
            return TAGS_NOT_WRITTEN;
        }
        TAGLIB_COMPLEX_PROPERTY_PICTURE(pic, p->data, (unsigned int)p->size, p->description,
                                        p->mime, p->type);
        if (!taglib_complex_property_set(f, "PICTURE", pic)) {
            taglib_file_free(f);
            set_err(err, errlen, "this type of file can not hold a picture");
            return TAGS_NOT_WRITTEN;
        }
    }
    for (int i = 0; i < TAG_FIELDS; i++) {
        const struct tag_values *v = &want->value[i];
        if (tags_equal(v, &before.value[i]))
            continue;
        taglib_property_set(f, tags_key[i], v->n > 0 ? v->v[0] : NULL);
        for (size_t k = 1; k < v->n; k++)
            taglib_property_set_append(f, tags_key[i], v->v[k]);
    }
    int saved = taglib_file_save(f);
    taglib_file_free(f);
    if (!saved) {
        /* TagLib may have written part of the file before failing. */
        set_err(err, errlen, "TagLib could not save the file; check that it still plays");
        fprintf(stderr, "tags: TagLib could not save %s\n", path);
        return TAGS_WRITTEN_BAD;
    }

    struct tags after;
    char why[128];
    if (tags_read(path, &after, NULL, NULL, why, sizeof why) != 0) {
        set_err(err, errlen, "after saving, %s; check that it still plays", why);
        fprintf(stderr, "tags: can not read %s after saving it\n", path);
        return TAGS_WRITTEN_BAD;
    }
    diff = first_difference(&after, want);
    if (diff == TAG_FIELDS) {
        set_err(err, errlen, "after saving, the pictures differ");
        return TAGS_WRITTEN_BAD;
    }
    if (diff >= 0) {
        const struct tag_values *v = &after.value[diff];
        set_err(err, errlen, "after saving, %s reads %.200s", field_key(diff),
                v->n == 0 ? "nothing" : v->v[0]);
        return TAGS_WRITTEN_BAD;
    }
    return TAGS_WRITTEN;
}
