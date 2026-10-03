#ifndef TAGS_H
#define TAGS_H

#include <stddef.h>

/*
 * Music file tags through TagLib's C API. TagLib is the only thing that
 * opens music files (nylm itself only lstat()s them). Only MP3 and FLAC.
 */

/* The tags nylm reads and edits, by index. */
enum tag_field {
    TAG_TITLE,
    TAG_ARTIST,
    TAG_ALBUM,
    TAG_ALBUMARTIST,
    TAG_GENRE,
    TAG_DATE,
    TAG_TRACKNUMBER,
    TAG_DISCNUMBER,
    TAG_COMPILATION,
    TAG_FIELDS
};

extern const char *const tags_key[TAG_FIELDS];  /* TagLib property: "TITLE" */
extern const char *const tags_name[TAG_FIELDS]; /* JSON and column name: "title" */

#define TAGS_MAX_VALUE 500  /* bytes in one tag value */
#define TAGS_MAX_PATH  4096
#define TAGS_MAX_NOTE  256  /* bytes in a change's note */

enum tags_format { TAGS_NONE, TAGS_MP3, TAGS_FLAC };

/* The format a file name stands for (".mp3", ".flac", any case). */
enum tags_format tags_format_of(const char *name);

/* The field called name ("title", ...), or -1. */
int tags_field_of(const char *name);

struct tags {
    const char *value[TAG_FIELDS]; /* NULL: absent; several values joined by "; " */
    unsigned multi;                /* bit (1u << field) set: several values */
    int pictures;                  /* embedded pictures */
    int seconds;                   /* audio length */
};

/*
 * Reads a file's tags; strings go into the arena. 0, or -1 with a reason
 * in err.
 */
int tags_read(const char *path, enum tags_format format, struct tags *out, char *err,
              size_t errlen);

/*
 * Checks a new value for a field. "" means remove the tag; title, artist,
 * album, album artist, track and disc number can not be removed. A genre is
 * words of lowercase a-z and '-' with single spaces, several separated by
 * "; " (written as one string).
 * NULL if valid, else what is wrong.
 */
const char *tags_check_value(enum tag_field field, const char *value);

/* One tag to change, and what became of it. */
struct tags_change {
    enum tag_field field;
    const char *old;   /* the value it was queued against (NULL: absent) */
    const char *value; /* the new value; "" removes the tag */
    int failed;        /* set by tags_write: 1 if not done */
    char note[TAGS_MAX_NOTE]; /* why it failed, or what else changed */
};

/*
 * Writes the changes into the file in place, through TagLib. Each change
 * is checked first: a valid value (tags_check_value), and the file still
 * has the old value, with a single value (a genre may have several: they
 * are all replaced by the new string). Failing changes are skipped;
 * the others are set and the file is saved once. Then it is read back:
 * every change must read as asked; any other tag, picture or audio
 * property that differs from before is listed in each done change's note
 * (a warning: the file is already written).
 *
 * Returns 1 if the file was saved, 0 if nothing was written (all changes
 * failed, each with its note).
 */
int tags_write(const char *path, enum tags_format format, struct tags_change *changes, int n);

/* A file's modification time in ns, as stored for a scan. */
struct stat;
long long tags_mtime(const struct stat *sb);

#endif
