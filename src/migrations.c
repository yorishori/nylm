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
};

const int plants_migration_count = sizeof plants_migrations / sizeof plants_migrations[0];
