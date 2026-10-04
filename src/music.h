#ifndef MUSIC_H
#define MUSIC_H

#include <sqlite3.h>

#include "tags.h"

/*
 * The music library: the folder NYLM_MUSIC and its cache in music_db.
 * A track is a file with a music extension (tags_is_music()) anywhere
 * below the folder; an album is the tracks that share ALBUM and
 * ALBUMARTIST. Dot files and folders are skipped, symlinks never followed.
 *
 * Only these services touch the files, each its own process, one at a time:
 *   music_scan   reads tags into the cache;
 *   music_write  writes the pending rows of the changes table, then reads
 *                each file it wrote back into the cache;
 *   music_move   moves files to where the naming rule puts them (move.h).
 * The server only reads the database, queues changes and scans, and
 * starts the services.
 *
 * The library lock (<NYLM_DATA>/music/library.lock) keeps them apart: a
 * service holds it exclusively while it runs; the server holds it shared
 * for each of its own writes to music_db, and refuses to write while a
 * service runs.
 */

#define MUSIC_MAX_ROOT  1024 /* bytes in NYLM_MUSIC */
#define MUSIC_MAX_DEPTH 32   /* folder levels below NYLM_MUSIC */
#define MUSIC_BUSY      (-2) /* music_lock_shared(): a service runs */

/*
 * Sets the library folder (root, may be NULL or "": no music) and the data
 * folder (for the lock). Logs and returns -1 if root is not an absolute
 * path other than "/". Whether it exists is checked when used.
 */
int music_configure(const char *root, const char *data_dir);

/* The library folder without a trailing slash; NULL if not configured. */
const char *music_root(void);

/* 1 if the library folder is an existing folder, 0 if not (logged). */
int music_available(void);

/* 1 if path is the library folder or a path below it: absolute, UTF-8,
 * without empty, "." or ".." parts, shorter than TAGS_MAX_PATH. */
int music_inside(const char *path);

/*
 * `nylm music-scan [path]`: with a path (the library folder or a folder or
 * file in it), scans that. Without, runs the scans the web app queued, or
 * the whole library if none is queued. A folder's new and changed files
 * are read (a single file always is), their pictures stored (src/art.h);
 * tracks no longer there are dropped from the cache. A complete scan of
 * the whole library also removes the stored pictures no track has. Prints
 * what it did; 0 or 1 (exit code).
 */
int music_scan(const char *path);

/*
 * `nylm music-write`: writes the pending changes, one track at a time
 * (src/tags.c checks the file and verifies the write), records each
 * result, and reads each track back into the cache. A new cover is first
 * checked: its bytes must still have its hash and decode completely (which
 * makes its thumbnail). 0, or 1 if it could not run.
 */
int music_write(void);

/* For the server's writes to music_db: an fd holding the lock shared, or
 * MUSIC_BUSY, or -1 on error (logged). Release with music_unlock(). */
int music_lock_shared(void);
void music_unlock(int fd);

/* The service running now: "scan", "write", "move", "busy" (just
 * starting), or NULL if none (or on error: *error set, logged). */
const char *music_busy(int *error);

/* For a service (name: "scan", "write", "move"): takes the library lock
 * once the library is set up and there. The fd (closing it releases the
 * lock), or -1 (logged). */
int music_start_service(const char *name);

/*
 * The columns of a track's tags in a query on "tracks t": its pictures (a
 * JSON array of {hash, type, description}), the single-valued tags in
 * tags.h order, then genre and composer as JSON arrays.
 * music_track_tags() reads them.
 */
#define MUSIC_TAG_COLUMNS                                                               \
    "(SELECT json_group_array(json_object('hash', p.hash, 'type', p.type,"              \
    "  'description', p.description) ORDER BY p.position)"                              \
    "  FROM track_pictures p WHERE p.track_id = t.id),"                                 \
    " t.title, t.album, t.artist, t.albumartist, t.tracknumber, t.discnumber,"          \
    " t.date, t.compilation, t.isrc, t.asin, t.bpm, t.copyright,"                      \
    " t.encodedby, t.mood, t.media, t.label, t.catalognumber, t.barcode, t.titlesort,"  \
    " t.albumsort, t.artistsort, t.albumartistsort, t.composersort,"                    \
    " t.musicbrainz_trackid, t.musicbrainz_albumid, t.navidrome_id,"                    \
    " (SELECT json_group_array(value ORDER BY position) FROM track_values v"            \
    "  WHERE v.track_id = t.id AND v.field = 'genre'),"                                 \
    " (SELECT json_group_array(value ORDER BY position) FROM track_values v"            \
    "  WHERE v.track_id = t.id AND v.field = 'composer')"
#define MUSIC_TAG_NCOLS (1 + TAG_FIELDS)

/*
 * The field of a change that sets the album cover (value: the hash of an
 * uploaded JPEG in the art table), and its bit next to the tags' bits
 * (1u << field) in a set of changed fields.
 */
#define MUSIC_COVER_FIELD "picture"
#define MUSIC_COVER_BIT   (1u << TAG_FIELDS)

/* Sets t's pictures to one front cover, the picture hash (in the arena).
 * 0, or -1 if hash is not one or out of memory. */
int music_set_cover(struct tags *t, const char *hash);

/* Reads MUSIC_TAG_COLUMNS from column col of st's row into out (strings in
 * the arena). 0, or -1 when out of memory or a JSON array is invalid. */
int music_track_tags(sqlite3_stmt *st, int col, struct tags *out);

/* A JSON array of strings (as stored for genre and composer) into out, in
 * the arena. 0, or -1 if it is not one or out of memory. */
int music_values_parse(const char *json, struct tag_values *out);

/* v as a JSON array of strings, in the arena; NULL when out of memory. */
const char *music_values_json(const struct tag_values *v);

#endif
