/*
 * nylm-musicbrainz: the MusicBrainz service (nylm-musicbrainz@.service), a
 * binary of its own so that the server never links libssl. Configured like
 * nylm (NYLM_DATA; NYLM_MUSIC is not needed: it reads only the cache);
 * one argument, the job: ids or genres.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "db.h"
#include "json.h"
#include "music.h"
#include "musicbrainz.h"

#define ARENA_SIZE (16 * 1024 * 1024)

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "ids") != 0 && strcmp(argv[1], "genres") != 0)) {
        fprintf(stderr, "usage: %s ids|genres   (find on MusicBrainz the album ids, or the "
                        "genres, of the next %d albums without one, and queue them; "
                        "environment as for nylm)\n",
                argv[0], MB_ALBUMS);
        return 2;
    }
    if (db_open_all(getenv("NYLM_DATA")) != 0)
        return 1;
    int rc = 1;
    if (music_configure(getenv("NYLM_MUSIC"), getenv("NYLM_DATA")) == 0) {
        if (arena_init(ARENA_SIZE) != 0) {
            fprintf(stderr, "out of memory\n");
        } else {
            json_init();
            rc = musicbrainz_run(argv[1]);
        }
    }
    db_close_all();
    return rc;
}
