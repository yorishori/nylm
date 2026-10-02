/*
 * The database schema, as a list of migrations. db_open() applies every
 * migration past PRAGMA user_version, in order, each in its own transaction.
 *
 * To change the schema, append a new entry. Never edit or remove an entry
 * that has been installed anywhere: databases already ran it.
 */

#include "db.h"

const char *const migrations[] = {
    /* 1: the single user's password (Argon2id) and active login sessions.
     * Only the SHA-256 of each session token is stored, never the token. */
    "CREATE TABLE user ("
    "    id         INTEGER PRIMARY KEY CHECK (id = 1),"
    "    salt       BLOB    NOT NULL,"
    "    hash       BLOB    NOT NULL,"
    "    memcost    INTEGER NOT NULL," /* KiB */
    "    iterations INTEGER NOT NULL,"
    "    updated_at INTEGER NOT NULL DEFAULT (unixepoch())"
    ") STRICT;"
    "CREATE TABLE sessions ("
    "    token_hash BLOB    PRIMARY KEY,"
    "    expires_at INTEGER NOT NULL"
    ") STRICT, WITHOUT ROWID;",
};

const int migration_count = sizeof migrations / sizeof migrations[0];
