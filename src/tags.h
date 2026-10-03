#ifndef TAGS_H
#define TAGS_H

#include <stddef.h>

/*
 * Music file tags through TagLib's C API. The only code in nylm that opens
 * music files. Only MP3 and FLAC are handled.
 *
 * Writing never touches the original file until the very end:
 *   tags_prepare  copies the file to a temporary file in the same folder,
 *                 edits the copy, and reads it back to check that exactly the
 *                 requested tags changed and the audio is the same;
 *   tags_commit   renames the copy over the original (atomic);
 *   tags_discard  deletes the copy instead.
 * Prepare every file of an album first, then commit them all, so a failure
 * on any file leaves every file as it was.
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
#define TAGS_TMP_PREFIX ".nylm-tmp-" /* temporary copies; scans skip dot files */

enum tags_format { TAGS_NONE, TAGS_MP3, TAGS_FLAC };

/* The format a file name stands for (".mp3", ".flac", any case). */
enum tags_format tags_format_of(const char *name);

struct tags {
    const char *value[TAG_FIELDS]; /* NULL: absent; several values joined by "; " */
    unsigned multi;                /* bit (1u << field) set: several values */
    int pictures;                  /* embedded pictures */
    int seconds;                   /* audio length */
};

/* Results of tags_prepare / tags_commit besides 0 (done) and -1 (failed). */
#define TAGS_STALE  1 /* the file changed since it was scanned */
#define TAGS_LOCKED 2 /* a tag to change has several values */

/*
 * Reads a file's tags; strings go into the request arena. 0, or -1 with a
 * reason in err.
 */
int tags_read(const char *path, enum tags_format format, struct tags *out, char *err,
              size_t errlen);

/*
 * Checks a new value for a field. "" means remove the tag; title, artist,
 * album, album artist and track number can not be removed. NULL if valid,
 * else what is wrong.
 */
const char *tags_check_value(enum tag_field field, const char *value);

struct tags_edit {
    const char *path;             /* absolute path of the file */
    enum tags_format format;
    long long size, mtime;        /* as scanned (mtime in ns); must still match */
    const char *set[TAG_FIELDS];  /* NULL: keep; "": remove; else the new value */
    char tmp[TAGS_MAX_PATH];      /* the prepared copy; "" if none */
};

/* Writes and verifies the edited copy. 0, TAGS_STALE, TAGS_LOCKED or -1;
 * on anything but 0 no copy is left and err says why. */
int tags_prepare(struct tags_edit *e, char *err, size_t errlen);

/* Replaces the original with the prepared copy. 0, TAGS_STALE or -1. */
int tags_commit(struct tags_edit *e, char *err, size_t errlen);

/* Deletes the prepared copy, if any. */
void tags_discard(struct tags_edit *e);

/* A file's modification time in ns, as stored for a scan. */
struct stat;
long long tags_mtime(const struct stat *sb);

#endif
