#ifndef NAVIDROME_H
#define NAVIDROME_H

#include <stddef.h>

#include <cjson/cJSON.h>

/*
 * Navidrome, optional: the music server on this machine that reads the
 * same music folder (read-only). nylm talks to it through its Subsonic API
 * (/rest/...), over plain HTTP to 127.x.x.x (src/https.h), as player
 * "nylm", with the login kept in music_db (navidrome_account; the password
 * never leaves the server). Set up by NYLM_NAVIDROME (its address) and
 * NYLM_NAVIDROME_MUSIC (the music folder as Navidrome sees it, if not the
 * same path as NYLM_MUSIC).
 */

#define NAVIDROME_MAX_URL      128  /* bytes in NYLM_NAVIDROME */
#define NAVIDROME_MAX_USER     64   /* bytes in a user name */
#define NAVIDROME_MAX_PASSWORD 256  /* bytes in a password */
#define NAVIDROME_MAX_ID       64   /* bytes in a Navidrome id */
#define NAVIDROME_MAX_ANSWER   (32 * 1024 * 1024) /* bytes in one answer: getStarred2
                                                   * sends every starred song at once */

/*
 * Checks and keeps the settings: url "http://127.x.x.x[:port][/prefix]"
 * (NULL or "": Navidrome is not used), music the absolute path of the music
 * folder inside Navidrome (NULL or "": the same as NYLM_MUSIC). Navidrome
 * needs the music folder (music_configure() first). 0, or -1 (logged).
 */
int navidrome_configure(const char *url, const char *music);

/* The address without a trailing slash, or NULL if Navidrome is not used. */
const char *navidrome_url(void);

/* The music folder as Navidrome sees it (NYLM_MUSIC if not set). */
const char *navidrome_music(void);

/*
 * 1 if url is an address nylm can use: "http://", an IPv4 address in
 * 127.0.0.0/8, an optional port, an optional path of A-Z a-z 0-9 / _ . -
 * (no "..", no "//"), at most NAVIDROME_MAX_URL bytes. A trailing slash is
 * allowed (dropped by navidrome_configure()).
 */
int navidrome_url_valid(const char *url);

/* 1 if s is a user name: 1..NAVIDROME_MAX_USER bytes of text (UTF-8, no
 * control characters). */
int navidrome_user_valid(const char *s);

/* 1 if s is a password: 1..NAVIDROME_MAX_PASSWORD bytes of text. */
int navidrome_password_valid(const char *s);

/* 1 if s is a Navidrome id: 1..NAVIDROME_MAX_ID bytes of A-Z a-z 0-9 - _. */
int navidrome_id_valid(const char *s);

/*
 * The address of a Subsonic call into out: base/rest/method?u=user&t=token
 * &s=salt&v=1.16.1&c=nylm&f=json, then extra ("" or "&name=value...", each
 * value already escaped). token is the MD5 of password + salt, in hex.
 * 0, or -1 if it does not fit or the hash fails.
 */
int navidrome_call_url(const char *base, const char *method, const char *user,
                       const char *password, const char *salt, const char *extra, char *out,
                       size_t outlen);

/*
 * Reads an answer's body: the "subsonic-response" object (in the arena) if
 * its status is "ok". Else NULL with why in err; *refused is set to 1 when
 * Navidrome answered with an error of its own (a wrong login, ...), to 0
 * when the answer is not one.
 */
cJSON *navidrome_answer(const char *body, size_t len, int *refused, char *err, size_t errlen);

/*
 * Calls method with extra (as for navidrome_call_url()), as user with
 * password (NULL: the login in music_db). The "subsonic-response" object
 * (in the arena), or NULL with why in err: *refused (may be NULL) is 1 if
 * Navidrome answered with an error, 0 if it did not answer, there is no
 * login, or it is not set up.
 */
cJSON *navidrome_call(const char *method, const char *extra, const char *user,
                      const char *password, int *refused, char *err, size_t errlen);

/*
 * Checks a login before it is kept: Navidrome answers a ping as user, and
 * user is an admin there (starting a scan needs one). 0, or -1 with why in
 * err and *refused as for navidrome_call() (1 also when not an admin).
 */
int navidrome_check_login(const char *user, const char *password, int *refused, char *err,
                          size_t errlen);

/* The user name of the login in music_db, in the arena: 1, 0 if none, -1 on
 * error (logged). */
int navidrome_login(const char **user);

/*
 * Scans around the services that change files, so that Navidrome keeps
 * a file's plays, stars and playlists when it moves: it matches a moved
 * file to the one it knew by its tags, which it must have read before.
 * Each call records itself in music_db (navidrome_scans: reason, state,
 * note) and must run under the library lock, so that one runs at a time.
 * reason: "before-write", "after-write", "before-move", "after-move",
 * "after-qobuz". Without NYLM_NAVIDROME each does nothing and returns 0.
 */

/* Checks that Navidrome answers with the kept login (a ping). 0, or -1
 * (recorded as failed, with why; logged): then the caller must not go on. */
int navidrome_ready(const char *reason);

/*
 * Has Navidrome scan the library and waits until it is done: first for a
 * scan it runs already, then for its own (startScan), which counts as
 * done when getScanStatus says it is no longer scanning after it was, or
 * its lastScan time changed. At most NAVIDROME_SCAN_LIMIT seconds; one
 * that does not start within NAVIDROME_SCAN_START seconds fails. 0, or -1
 * (recorded with why, logged).
 */
int navidrome_scan(const char *reason);

#define NAVIDROME_SCAN_LIMIT 3600
#define NAVIDROME_SCAN_START 60

/*
 * The path in the music folder of a song Navidrome lists at path (as it
 * reports it with Report Real Path: below navidrome_music(), or relative
 * to it) into out. 0, or -1 if it is not in the music folder (a path
 * without Report Real Path, "..", too long).
 */
int navidrome_local_path(const char *nav_root, const char *music_root, const char *path,
                         char *out, size_t outlen);

#endif
