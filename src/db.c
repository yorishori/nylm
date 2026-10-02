#include "db.h"

#include <stdio.h>

sqlite3 *db;

void db_log_error(const char *context)
{
    fprintf(stderr, "db: %s: %s\n", context, sqlite3_errmsg(db));
}

int db_exec(const char *sql)
{
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "db: %s\n", err ? err : "unknown error");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

static int user_version(void)
{
    sqlite3_stmt *st = db_prepare("PRAGMA user_version");
    if (st == NULL)
        return -1;
    int v = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
    sqlite3_finalize(st);
    return v;
}

/* Applies each migration past PRAGMA user_version in its own transaction. */
static int migrate(void)
{
    int version = user_version();
    if (version < 0)
        return -1;
    if (version > migration_count) {
        fprintf(stderr, "db: schema version %d is newer than this binary (%d)\n", version,
                migration_count);
        return -1;
    }
    for (int i = version; i < migration_count; i++) {
        char set_version[64];
        snprintf(set_version, sizeof set_version, "PRAGMA user_version = %d", i + 1);
        if (db_exec("BEGIN IMMEDIATE") != 0)
            return -1;
        if (db_exec(migrations[i]) != 0 || db_exec(set_version) != 0) {
            fprintf(stderr, "db: migration %d failed\n", i + 1);
            db_exec("ROLLBACK");
            return -1;
        }
        if (db_exec("COMMIT") != 0)
            return -1;
        printf("db: applied migration %d\n", i + 1);
    }
    return 0;
}

/* Oldest SQLite with everything the schema uses: STRICT tables (3.37) and
 * unixepoch() (3.38). */
#define MIN_SQLITE_VERSION 3038000

int db_open(const char *path)
{
    if (sqlite3_libversion_number() < MIN_SQLITE_VERSION) {
        fprintf(stderr, "db: SQLite %s is too old, need 3.38 or newer\n", sqlite3_libversion());
        return -1;
    }
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) !=
        SQLITE_OK) {
        db_log_error(path);
        return -1;
    }

    /* The system library is built with general-purpose defaults. Turn off what
     * we never want: double-quoted strings silently accepted as literals (hides
     * typos in column names) and loading extensions from SQL. */
    if (sqlite3_db_config(db, SQLITE_DBCONFIG_DQS_DML, 0, (int *)NULL) != SQLITE_OK ||
        sqlite3_db_config(db, SQLITE_DBCONFIG_DQS_DDL, 0, (int *)NULL) != SQLITE_OK ||
        sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, (int *)NULL) !=
            SQLITE_OK) {
        db_log_error("sqlite3_db_config");
        return -1;
    }
    if (sqlite3_busy_timeout(db, 5000) != SQLITE_OK) {
        db_log_error("sqlite3_busy_timeout");
        return -1;
    }
    if (db_exec("PRAGMA journal_mode = WAL;"
             "PRAGMA synchronous = NORMAL;"
             "PRAGMA foreign_keys = ON;") != 0)
        return -1;
    return migrate();
}

void db_close(void)
{
    sqlite3_close(db);
    db = NULL;
}

sqlite3_stmt *db_prepare(const char *sql)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        db_log_error(sql);
        return NULL;
    }
    return st;
}
