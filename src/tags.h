#ifndef TAGS_H
#define TAGS_H

#include <stddef.h>

#include "art.h"

/*
 * Music file tags through TagLib's C API. TagLib is the only thing that
 * opens music files (nylm itself only lstat()s them), and only the tags
 * below are read or written.
 */

/* The tags, by index: the single-valued ones first (in the column order of
 * the tracks table), then the two that hold several values. */
enum tag_field {
    TAG_TITLE,
    TAG_ALBUM,
    TAG_ARTIST,
    TAG_ALBUMARTIST,
    TAG_TRACKNUMBER,
    TAG_DISCNUMBER,
    TAG_DATE,
    TAG_COMPILATION,
    TAG_ISRC,
    TAG_ASIN,
    TAG_BPM,
    TAG_COPYRIGHT,
    TAG_ENCODEDBY,
    TAG_MOOD,
    TAG_MEDIA,
    TAG_LABEL,
    TAG_CATALOGNUMBER,
    TAG_BARCODE,
    TAG_TITLESORT,
    TAG_ALBUMSORT,
    TAG_ARTISTSORT,
    TAG_ALBUMARTISTSORT,
    TAG_COMPOSERSORT,
    TAG_MUSICBRAINZ_TRACKID,
    TAG_MUSICBRAINZ_ALBUMID,
    TAG_NAVIDROME_ID,
    TAG_GENRE,    /* several values: table track_values */
    TAG_COMPOSER, /* several values: table track_values */
    TAG_FIELDS
};

#define TAG_SINGLE_FIELDS TAG_GENRE /* fields before this hold one value */

extern const char *const tags_key[TAG_FIELDS];  /* TagLib property: "TITLE" */
extern const char *const tags_name[TAG_FIELDS]; /* JSON and column name: "title" */

/* The field called name ("title", ...), or -1. */
int tags_field_of(const char *name);

/* 1 for genre and composer: several values, kept in order. */
int tags_is_multi(enum tag_field f);

/* 1 if the web app may change it: all but the sort tags (written by nylm
 * from their source tag) and the Navidrome id. */
int tags_is_editable(enum tag_field f);

/* 1 if every track must have it. */
int tags_is_required(enum tag_field f);

#define TAGS_MAX_VALUE  500  /* bytes in one value */
#define TAGS_MAX_VALUES 64   /* values of one genre or composer tag */
#define TAGS_MAX_PATH   4096
#define TAGS_MAX_EXT    16   /* bytes in a file name extension we keep */
#define TAGS_MAX_NOTE   512  /* bytes in a change's note */

/* 1 if ext (lower case, without the dot) is a music file extension. */
int tags_is_music(const char *ext);

/* One tag's values: n == 0 is absent. A single-valued tag has at most one
 * (a file's several values are read joined by "; "). */
struct tag_values {
    size_t n;
    const char **v;
};

#define TAGS_MAX_PICTURES 32 /* pictures of one track that are kept */

/* An embedded picture. data, size and mime are only set to write it. */
struct tag_picture {
    char hash[ART_HASH_LEN + 1]; /* SHA-256 of its bytes */
    const char *type;            /* "Front Cover", ...; "" if the format has none */
    const char *description;     /* "" if none */
    const unsigned char *data;
    size_t size;
    const char *mime;
};

struct tags {
    struct tag_values value[TAG_FIELDS];
    size_t npictures;
    const struct tag_picture *pictures; /* in the file's order */
};

/* Called by tags_read() for each picture, with its bytes (valid only
 * during the call); returns 0, or -1 to fail the read. */
typedef int (*tags_picture_fn)(void *ctx, const struct tag_picture *p,
                               const unsigned char *data, size_t size);

/* 1 if a and b hold the same values in the same order. */
int tags_equal(const struct tag_values *a, const struct tag_values *b);

/* Sets t's field to the one value s (NULL or "": absent). 0, or -1 when out
 * of memory. */
int tags_set_one(struct tags *t, enum tag_field f, const char *s);

/*
 * Reads a file's tags and its first TAGS_MAX_PICTURES pictures (each also
 * passed to on_picture, unless NULL); strings go into the arena. An absent
 * compilation reads as "0". 0, or -1 with a reason in err.
 */
int tags_read(const char *path, struct tags *out, tags_picture_fn on_picture, void *ctx,
              char *err, size_t errlen);

/* 1 if a and b hold the same pictures (by their bytes) in the same order. */
int tags_same_pictures(const struct tags *a, const struct tags *b);

/*
 * Checks one tag's values. NULL if valid, else what is wrong:
 *  - required tags must have a value;
 *  - text: UTF-8 without control characters, at most 500 bytes each;
 *  - genre and composer: at most 64 values, none empty; a genre is
 *    lowercase a-z, 0-9 and '-';
 *  - track and disc number "X/Y", positive whole numbers, X <= Y;
 *  - date (the year) and BPM: a positive whole number;
 *  - compilation "0" or "1".
 */
const char *tags_check(enum tag_field f, const struct tag_values *v);

/* Parses "X/Y" as tags_check() allows it. 0 and *x, *y; or -1. */
int tags_number(const char *s, int *x, int *y);

/*
 * Makes t ready to write, after its changes were applied:
 *  - the sort tags of the fields in changed (bit 1u << field) mirror them
 *    (TITLESORT = TITLE, ..., COMPOSERSORT = the composers joined by "; ");
 *  - a sort tag over 500 bytes is cut at a character boundary;
 *  - an invalid or missing disc number becomes "1/1".
 * What it did, if anything, is appended to note (a warning for the user).
 * 0, or -1 when out of memory.
 */
int tags_prepare(struct tags *t, unsigned changed, char *note, size_t notelen);

enum tags_result {
    TAGS_NOT_WRITTEN, /* refused or failed before saving: the file is as it was */
    TAGS_WRITTEN,     /* saved, and reads back as want */
    TAGS_WRITTEN_BAD, /* saved, but does not read back as want (or at all) */
};

/*
 * Writes want into the file in place, through TagLib. First the file must
 * still hold now (what the cache has, pictures included); then every tag
 * that differs is set (a genre or composer value by value, in order); if
 * want's pictures differ from the file's, they replace all of them (want
 * then has exactly one, with its data and mime); the file is saved once;
 * then it is read back: every tag and the pictures (by their bytes) must
 * read as want. err says why when not TAGS_WRITTEN.
 */
enum tags_result tags_write(const char *path, const struct tags *now, const struct tags *want,
                            char *err, size_t errlen);

#endif
