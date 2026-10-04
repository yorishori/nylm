#ifndef DUPES_H
#define DUPES_H

#include <sqlite3.h>
#include <stddef.h>

/*
 * Possible duplicates in the music library: two names are the same when
 * their keys are. A key is the name in lower case without accents on
 * Latin letters (also decomposed ones), punctuation, symbols and spaces,
 * with '&' read as "and" and a leading "The " dropped: "The Beatles",
 * "beatles" and "BEATLES!" share "beatles"; "Beyoncé" and "Beyonce" share
 * "beyonce". Other scripts are kept as they are (not case folded); bytes
 * that are not UTF-8 are skipped.
 */

#define DUPES_KEY_MAX 1024 /* bytes in a key; a longer one is cut */

/*
 * The key of the n bytes at s into out (cap bytes, at least 1), NUL
 * terminated; cut before a letter that would not fit. Its length; 0 if the
 * name has no letters or digits.
 */
size_t dupes_key(const char *s, size_t n, char *out, size_t cap);

/* Adds the SQL function dupe_key(text): the key, or NULL for NULL and for
 * an empty key. SQLITE_OK or an SQLite error code. */
int dupes_register(sqlite3 *db);

#endif
