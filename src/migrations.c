/*
 * The database schemas: one list of migrations per app, each app with its
 * own database (see src/db.h). db_open() applies every migration past the
 * database's PRAGMA user_version, in order, each in its own transaction.
 *
 * To change an app's schema, append a new entry to its list. Never edit or
 * remove an entry that has been installed anywhere: databases already ran it.
 */

#include "db.h"

const char *const core_migrations[] = {
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

const int core_migration_count = sizeof core_migrations / sizeof core_migrations[0];

const char *const plants_migrations[] = {
    /* 1: plant care. Plants and care types are archived, never deleted.
     * A rule says how often a care type is due for a plant; the log records
     * what was done (care_type_id NULL: a plain note). Due dates are not
     * stored: they are computed from the rule and the latest log entry.
     * Dates are "YYYY-MM-DD". Intervals: NULL means paused. */
    "CREATE TABLE plants ("
    "    id       INTEGER PRIMARY KEY,"
    "    name     TEXT    NOT NULL,"
    "    species  TEXT    NOT NULL,"
    "    location TEXT    NOT NULL,"
    "    acquired TEXT,"
    "    notes    TEXT    NOT NULL,"
    "    archived INTEGER NOT NULL DEFAULT 0 CHECK (archived IN (0, 1))"
    ") STRICT;"
    "CREATE TABLE care_types ("
    "    id       INTEGER PRIMARY KEY,"
    "    name     TEXT    NOT NULL UNIQUE COLLATE NOCASE,"
    "    archived INTEGER NOT NULL DEFAULT 0 CHECK (archived IN (0, 1))"
    ") STRICT;"
    "CREATE TABLE care_rules ("
    "    plant_id      INTEGER NOT NULL REFERENCES plants(id),"
    "    care_type_id  INTEGER NOT NULL REFERENCES care_types(id),"
    "    interval_days INTEGER CHECK (interval_days BETWEEN 1 AND 3650),"
    "    yearly_month  INTEGER CHECK (yearly_month BETWEEN 1 AND 12),"
    "    yearly_day    INTEGER CHECK (yearly_day BETWEEN 1 AND 31),"
    "    created       TEXT    NOT NULL," /* first due date counts from here */
    "    PRIMARY KEY (plant_id, care_type_id),"
    "    CHECK ((yearly_month IS NULL) = (yearly_day IS NULL)),"
    "    CHECK (yearly_month IS NULL OR interval_days IS NULL)"
    ") STRICT, WITHOUT ROWID;"
    /* Seasonal exceptions to a rule's interval; they never overlap. */
    "CREATE TABLE care_periods ("
    "    plant_id      INTEGER NOT NULL,"
    "    care_type_id  INTEGER NOT NULL,"
    "    start_month   INTEGER NOT NULL CHECK (start_month BETWEEN 1 AND 12),"
    "    start_day     INTEGER NOT NULL CHECK (start_day BETWEEN 1 AND 31),"
    "    end_month     INTEGER NOT NULL CHECK (end_month BETWEEN 1 AND 12),"
    "    end_day       INTEGER NOT NULL CHECK (end_day BETWEEN 1 AND 31),"
    "    interval_days INTEGER CHECK (interval_days BETWEEN 1 AND 3650),"
    "    FOREIGN KEY (plant_id, care_type_id) REFERENCES care_rules ON DELETE CASCADE"
    ") STRICT;"
    "CREATE INDEX care_periods_by_rule ON care_periods (plant_id, care_type_id);"
    "CREATE TABLE care_log ("
    "    id           INTEGER PRIMARY KEY,"
    "    plant_id     INTEGER NOT NULL REFERENCES plants(id),"
    "    care_type_id INTEGER REFERENCES care_types(id),"
    "    date         TEXT    NOT NULL,"
    "    note         TEXT    NOT NULL"
    ") STRICT;"
    "CREATE INDEX care_log_by_plant ON care_log (plant_id, date, id);"
    "CREATE INDEX care_log_by_rule ON care_log (plant_id, care_type_id, date);",

    /* 2: a colour per plant and care type, from a fixed palette, so the
     * frontend can show at a glance which plant and which care. */
    "ALTER TABLE plants ADD COLUMN color TEXT NOT NULL DEFAULT 'mint'"
    "    CHECK (color IN ('butter', 'lime', 'mint', 'teal', 'sky', 'periwinkle',"
    "                     'lavender', 'orchid'));"
    "ALTER TABLE care_types ADD COLUMN color TEXT NOT NULL DEFAULT 'sky'"
    "    CHECK (color IN ('butter', 'lime', 'mint', 'teal', 'sky', 'periwinkle',"
    "                     'lavender', 'orchid'));",
};

const int plants_migration_count = sizeof plants_migrations / sizeof plants_migrations[0];

const char *const music_migrations[] = {
    /* 1: the music library. The files are the truth: albums and tracks are a
     * cache of them, filled by `nylm music-scan` and refreshed after every
     * edit; deleting them loses nothing. An album is a folder. Paths are
     * relative to NYLM_MUSIC. Tag columns hold the file's value (NULL when
     * absent); a tag with several values holds them joined by "; " and has
     * its bit (1 << field, see src/tags.h) set in multi.
     * scans and audit are not a cache: they record what happened. */
    "CREATE TABLE albums ("
    "    id  INTEGER PRIMARY KEY,"
    "    dir TEXT    NOT NULL UNIQUE"
    ") STRICT;"
    "CREATE TABLE tracks ("
    "    id          INTEGER PRIMARY KEY,"
    "    album_id    INTEGER NOT NULL REFERENCES albums(id),"
    "    path        TEXT    NOT NULL UNIQUE,"
    "    format      TEXT    NOT NULL CHECK (format IN ('mp3', 'flac')),"
    "    size        INTEGER NOT NULL,"
    "    mtime       INTEGER NOT NULL," /* ns */
    "    seconds     INTEGER NOT NULL,"
    "    pictures    INTEGER NOT NULL,"
    "    multi       INTEGER NOT NULL,"
    "    title       TEXT,"
    "    artist      TEXT,"
    "    album       TEXT,"
    "    albumartist TEXT,"
    "    genre       TEXT,"
    "    date        TEXT,"
    "    tracknumber TEXT,"
    "    discnumber  TEXT,"
    "    compilation TEXT,"
    "    scan        INTEGER NOT NULL" /* the last scan that saw the file */
    ") STRICT;"
    "CREATE INDEX tracks_by_album ON tracks (album_id);"
    "CREATE TABLE scans ("
    "    id       INTEGER PRIMARY KEY,"
    "    started  INTEGER NOT NULL DEFAULT (unixepoch()),"
    "    finished INTEGER,"
    "    files    INTEGER NOT NULL DEFAULT 0," /* music files found */
    "    parsed   INTEGER NOT NULL DEFAULT 0," /* new or changed: tags read */
    "    failed   INTEGER NOT NULL DEFAULT 0," /* unreadable files and folders */
    "    ok       INTEGER CHECK (ok IN (0, 1))"
    ") STRICT;"
    /* Every change nylm makes to the library, or tries to. */
    "CREATE TABLE audit ("
    "    id     INTEGER PRIMARY KEY,"
    "    at     INTEGER NOT NULL DEFAULT (unixepoch()),"
    "    client TEXT    NOT NULL," /* IP address */
    "    action TEXT    NOT NULL,"
    "    detail TEXT    NOT NULL," /* JSON */
    "    result TEXT    NOT NULL"
    ") STRICT;",

    /* 2: tag changes, queued by the web app and written by `nylm
     * music-write`. One row per tag of one file. old is the cached value
     * when it was queued (NULL: absent); the write is refused if the file
     * no longer has it. new "" removes the tag. A track has at most one
     * pending change per tag. Written rows stay as history: done, warning
     * (written, but TagLib also changed what note lists) or failed (note
     * says why). path keeps the history readable after a scan removes the
     * track. */
    "CREATE TABLE changes ("
    "    id       INTEGER PRIMARY KEY,"
    "    track_id INTEGER REFERENCES tracks(id) ON DELETE SET NULL,"
    "    path     TEXT    NOT NULL,"
    "    field    TEXT    NOT NULL CHECK (field IN ('title', 'artist', 'album',"
    "                     'albumartist', 'genre', 'date', 'tracknumber', 'discnumber',"
    "                     'compilation')),"
    "    old      TEXT,"
    "    new      TEXT    NOT NULL,"
    "    client   TEXT    NOT NULL," /* IP address that queued it */
    "    queued   INTEGER NOT NULL DEFAULT (unixepoch()),"
    "    state    TEXT    NOT NULL DEFAULT 'pending'"
    "                     CHECK (state IN ('pending', 'done', 'warning', 'failed')),"
    "    finished INTEGER,"
    "    note     TEXT    NOT NULL DEFAULT ''"
    ") STRICT;"
    "CREATE UNIQUE INDEX changes_one_pending ON changes (track_id, field)"
    "    WHERE state = 'pending';"
    "CREATE INDEX changes_by_state ON changes (state, track_id);",
};

const int music_migration_count = sizeof music_migrations / sizeof music_migrations[0];
