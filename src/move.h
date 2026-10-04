#ifndef MOVE_H
#define MOVE_H

#include <stddef.h>

/*
 * Moving music files to where the naming rule puts them, from the cache's
 * tags: ALBUMARTIST/ALBUM/NN - TITLE.ext below the library folder, or
 * ALBUMARTIST/ALBUM/D-NN - TITLE.ext when the album has several discs.
 * The server only shows the plan; `nylm music-move` moves the files.
 */

#define MOVE_MAX_NAME 255 /* bytes in one file or folder name */

/*
 * The path, relative to the library folder, of a track with these tags
 * and file extension (lower case, without the dot), into out. NN is the
 * track number, zero-padded to two digits or to the digits of the track
 * total; D (and the dash) only when the disc total is more than 1. Each
 * part: '/', '\', ':', '*', '?', '"', '<', '>', '|' and control characters
 * become '_', spaces around and dots at the end are dropped, a leading dot
 * becomes '_', an empty part is "_", and it is cut at a character boundary
 * to fit MOVE_MAX_NAME bytes (the file name keeps its number and
 * extension). NULL if done, else why the track can not be named.
 */
const char *move_target(const char *albumartist, const char *album, const char *title,
                        const char *tracknumber, const char *discnumber, const char *ext,
                        char *out, size_t outlen);

/* One track of the plan. to is NULL when it stays: it is where it should
 * be, or it can not move (problem says why). */
struct move_item {
    long long id;
    const char *from;
    const char *to;
    const char *problem;
};

struct move_plan {
    struct move_item *items; /* every track, by path */
    size_t n, moves, problems;
};

/*
 * Plans the move of every track in the cache (in the arena): its target,
 * or why it can not move: it can not be named, two tracks would get the
 * same name, or a track that stays is already there. 0, or -1 (logged).
 */
int move_plan(struct move_plan *plan);

/*
 * `nylm music-move`: moves each track of the plan (never over an existing
 * file) and records its new path in the cache. When all the tracks of a
 * folder went to one folder, the folder's other files go there too; then
 * emptied folders are removed. Each file moved, failed or kept is
 * recorded in the moves table. Prints what it did; 0, or 1 if it could not
 * run.
 */
int music_move(void);

/* Renames from to to, never over an existing file or folder. On a file
 * system without RENAME_NOREPLACE (NFS) it checks first: nothing else
 * touches the library while the caller holds the library lock. 0, or -1
 * (errno). */
int move_rename_new(const char *from, const char *to);

/* Makes the folders of path below the library folder (each a real folder,
 * never a symlink). 0, or -1 (errno). */
int move_make_folders(const char *path);

#endif
