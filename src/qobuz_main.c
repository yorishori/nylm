/*
 * nylm-qobuz: the Qobuz service (nylm-qobuz.service), a binary of its own
 * so that only it links libssl. Configured like nylm (NYLM_DATA,
 * NYLM_MUSIC); takes no arguments.
 */
#include <stdio.h>
#include <stdlib.h>

#include "arena.h"
#include "db.h"
#include "json.h"
#include "music.h"
#include "qobuz.h"

#define ARENA_SIZE (16 * 1024 * 1024)

int main(int argc, char **argv)
{
    if (argc != 1) {
        fprintf(stderr, "usage: %s   (finish a queued Qobuz login, download the queued "
                        "albums; environment as for nylm)\n", argv[0]);
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
            rc = qobuz_run();
        }
    }
    db_close_all();
    return rc;
}
