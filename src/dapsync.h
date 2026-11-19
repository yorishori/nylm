#ifndef DAPSYNC_H
#define DAPSYNC_H

#include <stddef.h>

/*
 * Copying music to the DAP mounted at DAP_MOUNT (src/dap.h). Each DAP (by
 * its file system's UUID) has a selection in music_db (dap_items): albums,
 * tracks and Navidrome playlists. `nylm dap-sync` (nylm-dap-sync.service,
 * started by the web app with the password) brings the folder DAP_FOLDER on
 * the DAP to that selection: the selected tracks (and the pictures of their
 * folders) where the library has them, each name made safe for FAT, a
 * playlist as DAP_FOLDER/NAME.m3u8; what is not in it is deleted. Nothing
 * outside DAP_FOLDER is touched.
 *
 * Where it may write is limited three times over: its systemd unit lets it
 * write only to DAP_MOUNT and nylm's music data; it checks that DAP_MOUNT
 * is a FAT or exFAT mount of the DAP the sync is for, not the music
 * folder's file system; and every copy and delete goes through directory
 * handles opened below DAP_FOLDER without following links, never through
 * a path.
 */

#define DAP_FOLDER    "Music"          /* the folder nylm keeps on the DAP */
#define DAP_MAX_FILES 30000            /* files in one selection */
#define DAP_MAX_DEPTH 16               /* folder levels below DAP_FOLDER */
#define DAP_FAT_LIMIT 4294967295LL     /* bytes in a file on FAT */
#define DAP_PART      ".nylm-part"     /* a copy before it is complete */

/*
 * The path below DAP_FOLDER of the file at rel (relative to the music
 * folder): each part as move_safe_name() makes it (no characters FAT
 * refuses, no dot first or last), the file name keeping its extension, at
 * most 255 bytes each. 0, or -1 if rel is not a relative path of names or
 * the result does not fit.
 */
int dap_dest_path(const char *rel, char *out, size_t outlen);

/* s with A-Z as a-z, into out: two names FAT can not tell apart have the
 * same. It is cut to fit. */
void dap_fold(const char *s, char *out, size_t outlen);

/* One entry of a playlist: its path below DAP_FOLDER, and what
 * #EXTINF says (artist and title may be NULL, duration < 0: unknown). */
struct dap_song {
    const char *dest;
    const char *artist;
    const char *title;
    long duration;
};

/* The text of an M3U8 playlist of the songs (in the arena): #EXTM3U, then
 * an #EXTINF line and the path, relative to DAP_FOLDER, of each. NULL when
 * out of memory. */
char *dap_m3u8(const struct dap_song *songs, size_t n);

/* A file to have on the DAP: where it comes from, its path below
 * DAP_FOLDER, its size and modification time. */
struct dap_file {
    const char *src;
    const char *dest;
    long long size;
    long long mtime;
};

/* A file whose text nylm writes (a playlist). */
struct dap_text {
    const char *dest;
    const char *text;
};

struct dap_plan {
    struct dap_file *files;
    size_t nfiles;
    struct dap_text *texts;
    size_t ntexts;
};

struct dap_progress {
    long long copied, kept, deleted, failed; /* files */
    long long bytes;                          /* bytes copied */
    long long total;                          /* bytes to copy */
};

/* Told about each file: what happened ("copied", "kept", "deleted",
 * "failed"), its path below DAP_FOLDER and why it failed (else NULL).
 * 0, or -1 to stop. */
typedef int (*dap_report_fn)(const struct dap_progress *p, const char *what, const char *path,
                             const char *why, void *ctx);

/*
 * Brings the folder dir (an open handle of DAP_FOLDER) to plan: deletes
 * every file below it that is not in the plan and the folders left empty;
 * checks there is room; copies each file that is not there with the same
 * size and time (within 2 s: FAT keeps 2 s steps) to DAP_PART in its folder,
 * gives it the source's time, then renames it; writes each text the same
 * way unless the file holds it already. Two files whose names FAT can not tell apart: the second fails.
 * fat: files of 4 GiB and more fail. A file that can not be copied fails
 * and the next one goes on; the DAP running full stops it. 0 when done
 * (some files may have failed), or -1 with why in err.
 */
int dap_apply(int dir, const struct dap_plan *plan, int fat, struct dap_progress *p,
              dap_report_fn fn, void *ctx, char *err, size_t errlen);

/*
 * `nylm dap-sync`: runs the oldest queued sync (dap_syncs) of the DAP
 * mounted now and records how it went, the progress about once a second.
 * Holds the DAP lock, and the library lock shared. 0, or 1 if it failed.
 */
int dap_sync(void);

#endif
