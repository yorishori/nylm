#ifndef MUSICBRAINZ_H
#define MUSICBRAINZ_H

#include <stddef.h>

#include <cjson/cJSON.h>

/*
 * nylm-musicbrainz (only in that binary, with src/https.c; the server
 * never links it), two jobs, each for MB_ALBUMS albums a run:
 *   ids     MusicBrainz album ids for albums without one, found by their
 *           names;
 *   genres  genres for albums without one, by their MusicBrainz album id.
 * What it finds is queued like any edit.
 */

#define MB_ID_LEN    36  /* a MusicBrainz id: lowercase hex 8-4-4-4-12 */
#define MB_ALBUMS    10  /* albums looked up in one run */
#define MB_GENRES    5   /* genres taken from MusicBrainz's votes, at most */
#define MB_MAX_GENRE 100 /* bytes in a genre taken from MusicBrainz */
#define MB_MAX_NAME  2048 /* bytes in a name compared (album, artist credit) */
#define MB_MAX_QUERY 6400 /* bytes in a search's path, percent-encoded */

/* 1 if s is a MusicBrainz id: lowercase hex 8-4-4-4-12. */
int mb_id_valid(const char *s);

/* Genres chosen from MusicBrainz: name[i] for i < n; v points at them.
 * The voted ones, then the language. */
struct mb_genres {
    size_t n;
    char name[MB_GENRES + 1][MB_MAX_GENRE + 1];
    const char *v[MB_GENRES + 1];
};

/* Adds name to g unless it is there already or g is full. */
void mb_add_genre(struct mb_genres *g, const char *name);

/*
 * The genres of a MusicBrainz release or release group (its JSON, with
 * "genres": [{"name", "count"}, ...]): the MB_GENRES with the most votes (at
 * least 1; between equal votes MusicBrainz's order), leaving out names that
 * break the genre rule (tags_check()) or are longer than MB_MAX_GENRE.
 * The count.
 */
size_t mb_genres(const cJSON *entity, struct mb_genres *out);

/*
 * The language of a release (its JSON, "text-representation": {"language"},
 * an ISO 639-3 code) as a genre: its English name ("spanish"),
 * "instrumental" for no lyrics (zxx); NULL if it has none, several (mul)
 * or one not in nylm's list. *code is the code as given (NULL if none or
 * not 3 lowercase letters).
 */
const char *mb_language(const cJSON *release, const char **code);

/* The id of a release's release group (its JSON); NULL if none or not an id. */
const char *mb_release_group(const cJSON *release);

/*
 * name made comparable into out: ASCII letters and À..Þ lowercase, punctuation
 * (ASCII, and the typographic quotes, dashes and ellipsis) left out, white
 * space made one space, trimmed, a leading "the " left out; other bytes
 * kept. 0, or -1 if it does not fit.
 */
int mb_normalize(const char *name, char *out, size_t size);

/*
 * The path of a release search for album by albumartist (a Lucene query:
 * release:"album" AND artist:"albumartist", quotes and backslashes
 * escaped, percent-encoded) into out. 0, or -1 if it does not fit.
 */
int mb_search_path(const char *album, const char *albumartist, char *out, size_t size);

/*
 * The release of a release search (its JSON, "releases": [...]) whose
 * title and artist credit (names and join phrases) are album and
 * albumartist once normalised: the first Official one, else the first.
 * Its id, or NULL if none.
 */
const char *mb_pick_release(const cJSON *search, const char *album, const char *albumartist);

/* An album to look up: its MusicBrainz album id ("" when it is searched
 * for), names, and its tracks without a genre (or without an id). */
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

/*
 * The next albums to find an id for, at most max, in the order of the
 * albums list, into out (in the arena): by their planned album and album
 * artist (both present), none of whose tracks has a valid planned
 * MusicBrainz album id, not searched by these names before (queued,
 * unsure, not_found; failed or skipped are tried again). The count, or -1
 * (logged).
 */
int mb_next_unidentified(struct mb_album *out, int max);

/*
 * Queues mbid as the MusicBrainz album id of a's tracks that still have
 * no valid one, in batch. The caller holds the library lock and a
 * transaction. The number of tracks queued, or -1 (logged).
 */
int mb_queue_id(const struct mb_album *a, const char *mbid, long long batch);

/* Records the search for a: state (as the table allows), the id found
 * (NULL: none) and a note. 0 or -1 (logged). */
int mb_record_search(const struct mb_album *a, const char *state, const char *mbid,
                     const char *note);

/* One run of the service: what is "ids" or "genres". Looks up the next
 * MB_ALBUMS albums and queues what it found. The exit status. */
int musicbrainz_run(const char *what);

#endif
