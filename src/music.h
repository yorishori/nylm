#ifndef MUSIC_H
#define MUSIC_H

/*
 * The music library: the folder NYLM_MUSIC and its cache in music_db.
 * An album is a folder; its tracks are the .mp3 and .flac files directly
 * in it. Dot files and folders are skipped, symlinks are never followed.
 *
 * Only two services touch the files, each its own process, one at a time:
 *   music_scan   reads tags into the cache;
 *   music_write  writes the pending rows of the changes table.
 * The server only reads the database, queues changes, and starts them.
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

/*
 * `nylm music-scan`: brings the cache in line with the files, reading the
 * tags of new and changed files only. Prints progress; 0 or 1 (exit code).
 */
int music_scan(void);

/*
 * `nylm music-write`: writes every pending change (src/tags.c checks and
 * verifies each) and records its result in the changes table; refreshes
 * the cache of the files written. 0, or 1 if it could not run.
 */
int music_write(void);

/* For the server's writes to music_db: an fd holding the lock shared, or
 * MUSIC_BUSY, or -1 on error (logged). Release with music_unlock(). */
int music_lock_shared(void);
void music_unlock(int fd);

/* The service running now: "scan", "write", "busy" (just starting), or
 * NULL if none (or on error: *error set, logged). */
const char *music_busy(int *error);

#endif
