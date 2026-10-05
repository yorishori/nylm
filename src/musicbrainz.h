#ifndef MUSICBRAINZ_H
#define MUSICBRAINZ_H

#include <stddef.h>

#include <cjson/cJSON.h>

/*
 * nylm-musicbrainz: genres from MusicBrainz for albums without one, by
 * their MusicBrainz album id (only in that binary, with src/https.c; the
 * server never links it). The genres are queued like any edit.
 */

#define MB_ID_LEN    36  /* a MusicBrainz id: lowercase hex 8-4-4-4-12 */
#define MB_ALBUMS    10  /* albums looked up in one run */
#define MB_GENRES    3   /* genres queued for an album, at most */
#define MB_MAX_GENRE 100 /* bytes in a genre taken from MusicBrainz */

/* 1 if s is a MusicBrainz id: lowercase hex 8-4-4-4-12. */
int mb_id_valid(const char *s);

/* Genres chosen from MusicBrainz: name[i] for i < n; v points at them. */
struct mb_genres {
    size_t n;
    char name[MB_GENRES][MB_MAX_GENRE + 1];
    const char *v[MB_GENRES];
};

/*
 * The genres of a MusicBrainz release or release group (its JSON, with
 * "genres": [{"name", "count"}, ...]): the MB_GENRES with the most votes (at
 * least 1; between equal votes MusicBrainz's order), leaving out names that
 * break the genre rule (tags_check()) or are longer than MB_MAX_GENRE.
 * The count.
 */
size_t mb_genres(const cJSON *entity, struct mb_genres *out);

/* The id of a release's release group (its JSON); NULL if none or not an id. */
const char *mb_release_group(const cJSON *release);

/* An album to look up: its MusicBrainz album id, names, and its tracks
 * without a genre. */
struct mb_album {
    char mbid[MB_ID_LEN + 1];
    const char *album, *albumartist; /* NULL if absent */
    long long *tracks;
    size_t ntracks;
};

/*
 * The next albums to look up, at most max, in the order of the albums list,
 * into out (in the arena): those whose tracks without a planned genre all
 * have one planned MusicBrainz album id, valid, that no lookup found
 * before (queued, none, not_found; a failed or skipped one is tried
 * again). The count, or -1 (logged).
 */
int mb_next_albums(struct mb_album *out, int max);

/*
 * Queues g as the genre of a's tracks that still have no planned genre and
 * a's id, in batch. The caller holds the library lock and a transaction.
 * The number of tracks queued, or -1 (logged).
 */
int mb_queue(const struct mb_album *a, const struct mb_genres *g, long long batch);

/* Records the lookup of a: state (as the table allows), the genres (NULL:
 * none) and a note. 0 or -1 (logged). */
int mb_record(const struct mb_album *a, const char *state, const struct mb_genres *g,
              const char *note);

/* One run of the service: looks up the next MB_ALBUMS albums and queues
 * what it found. The exit status. */
int musicbrainz_run(void);

#endif
