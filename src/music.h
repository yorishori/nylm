#ifndef MUSIC_H
#define MUSIC_H

/*
 * The music library: the folder NYLM_MUSIC and its cache in music_db.
 * An album is a folder; its tracks are the .mp3 and .flac files directly
 * in it. Dot files and folders are skipped, symlinks are never followed.
 */

#define MUSIC_MAX_ROOT  1024 /* bytes in NYLM_MUSIC */
#define MUSIC_MAX_DEPTH 32   /* folder levels below NYLM_MUSIC */

/*
 * Sets the library folder (root, may be NULL or "": no music) and the data
 * folder (for the scan lock). Logs and returns -1 if root is not an
 * absolute path other than "/". Whether it exists is checked when used.
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

/* 1 if a scan is running now, 0 if not, -1 on error (logged). */
int music_scan_running(void);

/* Reads a track's file again into its row, after nylm changed it. 0 or -1. */
int music_reread(long track_id);

#endif
