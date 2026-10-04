#ifndef QOBUZ_H
#define QOBUZ_H

#include <stddef.h>

#include <cjson/cJSON.h>

#include "tags.h"

/*
 * Qobuz: the parts that need no network, so the server can use them too
 * (it validates album links). The service that talks to Qobuz is
 * nylm-qobuz (src/qobuz_service.c).
 *
 * Logging in is Qobuz's OAuth: the user logs in at the link with the web
 * player's app id, Qobuz sends the browser to http://localhost/?code=...
 * (nothing listens there), and the user pastes that address into nylm.
 * The app id, the OAuth private key and the secrets that sign download
 * requests are read from the web player's bundle.js.
 */

#define QOBUZ_MAX_ID       32   /* characters of an album or track id */
#define QOBUZ_MAX_REDIRECT 2048 /* bytes of a pasted redirect address */
#define QOBUZ_MAX_TOKEN    512  /* bytes of a code, token, user id or key */
#define QOBUZ_MAX_SECRETS  8
#define QOBUZ_MAX_SECRET   128
#define QOBUZ_APP_ID_LEN   9

/*
 * The id of a Qobuz album link: http(s)://www.qobuz.com/[xx-xx/]album/
 * [name/]ID, or open.qobuz.com/album/ID, play.qobuz.com/album/ID (a query
 * or a slash at the end is allowed). 0, or -1 if it is not one.
 */
int qobuz_album_id(const char *url, char id[QOBUZ_MAX_ID + 1]);

/* 1 if s may be a pasted redirect address or code: 1 to 2048 printable
 * ASCII characters, no spaces. */
int qobuz_redirect_valid(const char *s);

/*
 * What a pasted redirect address gives: code (code_autorisation or code),
 * token (user_auth_token or token) and user_id from its query, each
 * percent-decoded ("" if absent); or the whole text as the code if it is
 * not an address. 0, or -1 if it has neither a code nor a token.
 */
int qobuz_redirect_parse(const char *s, char code[QOBUZ_MAX_TOKEN + 1],
                         char token[QOBUZ_MAX_TOKEN + 1], char user_id[QOBUZ_MAX_TOKEN + 1]);

/* The path of bundle.js in the login page html ("/resources/1.2.3-b456/
 * bundle.js"). 0, or -1 if it is not there. */
int qobuz_bundle_path(const char *html, char *out, size_t size);

/* The production app id (9 digits) in the bundle. 0, or -1. */
int qobuz_app_id(const char *bundle, char out[QOBUZ_APP_ID_LEN + 1]);

/* The OAuth private key in the bundle, or "" if none is found. */
void qobuz_private_key(const char *bundle, char out[QOBUZ_MAX_TOKEN + 1]);

/*
 * The secrets that may sign download requests: per time zone of the
 * bundle's initialSeed() calls (in the order they first appear), its seeds
 * and then the info and extras of its entries, joined, less the last 44
 * characters, base64-decoded. The number found (at most max).
 */
int qobuz_secrets(const char *bundle, char out[][QOBUZ_MAX_SECRET + 1], int max);

/* The request_sig of track/getFileUrl: the MD5 (hex) of
 * "trackgetFileUrlformat_id<format>intentstreamtrack_id<id><ts><secret>".
 * 0, or -1 (logged). */
int qobuz_signature(const char *track_id, int format, long long ts, const char *secret,
                    char out[33]);

/* s percent-encoded for a query (all but A-Z a-z 0-9 - _ . ~) into out.
 * 0, or -1 if it does not fit. */
int qobuz_urlencode(const char *s, char *out, size_t size);

/*
 * Sets t's tags for one track of an album (album/get's JSON): title (with
 * its version), album (with its version), artist (the performer, else the
 * album artist), album artist, track number "N/tracks on its disc", disc
 * number "D/discs", date (the year), composer, copyright ((P) and (C) as
 * the symbols), label, ISRC, barcode, compilation 0. No genre: the user's
 * own. Strings go into the arena. NULL, or what is missing.
 */
const char *qobuz_track_tags(const cJSON *album, const cJSON *track, struct tags *t);

/*
 * nylm-qobuz only (src/qobuz_service.c): reads the web player's bundle,
 * finishes the queued login, downloads the queued albums into the library
 * and scans them. Prints what it did; 0, or 1 if it could not run.
 */
int qobuz_run(void);

/*
 * nylm-qobuz only: tags the downloaded file path (extension ext) from
 * album/get's album and track through TagLib (qobuz_track_tags(), the
 * sort tags mirrored, pic as its only picture if not NULL) and sets rel
 * to where it belongs in the library (move_target()). 0, or -1 with err
 * set.
 */
int qobuz_tag_file(const char *path, const char *ext, const cJSON *album, const cJSON *track,
                   const struct tag_picture *pic, char *rel, size_t rellen, char *err,
                   size_t errlen);

#endif
