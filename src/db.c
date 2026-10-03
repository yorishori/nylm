#define _POSIX_C_SOURCE 200809L

#include "db.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

sqlite3 *core_db;
sqlite3 *plants_db;
sqlite3 *music_db;

/* Each app: its folder and file name under the data folder, its schema, and
 * the connection its code uses. */
static const struct {
    const char *name;
    const char *const *migrations;
    const int *count;
    sqlite3 **conn;
} apps[] = {
    { "core",   core_migrations,   &core_migration_count,   &core_db },
    { "plants", plants_migrations, &plants_migration_count, &plants_db },
    { "music",  music_migrations,  &music_migration_count,  &music_db },
};

#define NAPPS (sizeof apps / sizeof apps[0])

void db_log_error(sqlite3 *conn, const char *context)
{
    fprintf(stderr, "db: %s: %s\n", context, sqlite3_errmsg(conn));
}

int db_exec(sqlite3 *conn, const char *sql)
{
    char *err = NULL;
    if (sqlite3_exec(conn, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "db: %s\n", err ? err : "unknown error");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

sqlite3_stmt *db_prepare(sqlite3 *conn, const char *sql)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(conn, sql, -1, &st, NULL) != SQLITE_OK) {
        db_log_error(conn, sql);
        return NULL;
    }
    return st;
}

static int user_version(sqlite3 *conn)
{
    sqlite3_stmt *st = db_prepare(conn, "PRAGMA user_version");
    if (st == NULL)
        return -1;
    int v = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
    sqlite3_finalize(st);
    return v;
}

/* Applies each migration past PRAGMA user_version in its own transaction. */
static int migrate(sqlite3 *conn, const char *path, const char *const *migrations, int count)
{
    int version = user_version(conn);
    if (version < 0)
        return -1;
    if (version > count) {
        fprintf(stderr, "db: %s: schema version %d is newer than this binary (%d)\n", path,
                version, count);
        return -1;
    }
    for (int i = version; i < count; i++) {
        char set_version[64];
        snprintf(set_version, sizeof set_version, "PRAGMA user_version = %d", i + 1);
        if (db_exec(conn, "BEGIN IMMEDIATE") != 0)
            return -1;
        if (db_exec(conn, migrations[i]) != 0 || db_exec(conn, set_version) != 0) {
            fprintf(stderr, "db: %s: migration %d failed\n", path, i + 1);
            db_exec(conn, "ROLLBACK");
            return -1;
        }
        if (db_exec(conn, "COMMIT") != 0)
            return -1;
        printf("db: %s: applied migration %d\n", path, i + 1);
    }
    return 0;
}

/* Oldest SQLite with everything the schema uses: STRICT tables (3.37) and
 * unixepoch() (3.38). */
#define MIN_SQLITE_VERSION 3038000

sqlite3 *db_open(const char *path, const char *const *migrations, int count)
{
    if (sqlite3_libversion_number() < MIN_SQLITE_VERSION) {
        fprintf(stderr, "db: SQLite %s is too old, need 3.38 or newer\n", sqlite3_libversion());
        return NULL;
    }
    sqlite3 *conn = NULL;
    if (sqlite3_open_v2(path, &conn, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) !=
        SQLITE_OK) {
        db_log_error(conn, path);
        sqlite3_close(conn);
        return NULL;
    }

    /* The system library is built with general-purpose defaults. Turn off what
     * we never want: double-quoted strings silently accepted as literals (hides
     * typos in column names) and loading extensions from SQL. */
    if (sqlite3_db_config(conn, SQLITE_DBCONFIG_DQS_DML, 0, (int *)NULL) != SQLITE_OK ||
        sqlite3_db_config(conn, SQLITE_DBCONFIG_DQS_DDL, 0, (int *)NULL) != SQLITE_OK ||
        sqlite3_db_config(conn, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, (int *)NULL) !=
            SQLITE_OK) {
        db_log_error(conn, "sqlite3_db_config");
        sqlite3_close(conn);
        return NULL;
    }
    if (sqlite3_busy_timeout(conn, 5000) != SQLITE_OK) {
        db_log_error(conn, "sqlite3_busy_timeout");
        sqlite3_close(conn);
        return NULL;
    }
    if (db_exec(conn, "PRAGMA journal_mode = WAL;"
                      "PRAGMA synchronous = NORMAL;"
                      "PRAGMA foreign_keys = ON;") != 0 ||
        migrate(conn, path, migrations, count) != 0) {
        sqlite3_close(conn);
        return NULL;
    }
    return conn;
}

/* Creates dir (mode 700) unless it is already a directory. 0 or -1 (logged). */
static int ensure_dir(const char *dir)
{
    struct stat sb;
    if (mkdir(dir, 0700) == 0) {
        printf("db: created %s\n", dir);
        return 0;
    }
    if (errno == EEXIST && stat(dir, &sb) == 0 && S_ISDIR(sb.st_mode))
        return 0;
    fprintf(stderr, "db: cannot create folder %s: %s\n", dir,
            errno == EEXIST ? "a file with that name exists" : strerror(errno));
    return -1;
}

int db_open_all(const char *data_dir)
{
    struct stat sb;
    if (data_dir == NULL || *data_dir == '\0') {
        fprintf(stderr, "db: NYLM_DATA is not set\n");
        return -1;
    }
    if (stat(data_dir, &sb) != 0) {
        fprintf(stderr, "db: data folder %s: %s%s\n", data_dir, strerror(errno),
                errno == ENOENT ? " (is the drive mounted?)" : "");
        return -1;
    }
    if (!S_ISDIR(sb.st_mode)) {
        fprintf(stderr, "db: data folder %s is not a folder\n", data_dir);
        return -1;
    }

    for (size_t i = 0; i < NAPPS; i++) {
        char dir[DB_MAX_PATH], path[DB_MAX_PATH];
        int n = snprintf(dir, sizeof dir, "%s/%s", data_dir, apps[i].name);
        int m = snprintf(path, sizeof path, "%s/%s.db", dir, apps[i].name);
        if (n < 0 || (size_t)n >= sizeof dir || m < 0 || (size_t)m >= sizeof path) {
            fprintf(stderr, "db: data folder path is too long\n");
            db_close_all();
            return -1;
        }
        if (ensure_dir(dir) != 0 ||
            (*apps[i].conn = db_open(path, apps[i].migrations, *apps[i].count)) == NULL) {
            db_close_all();
            return -1;
        }
    }
    return 0;
}

void db_close_all(void)
{
    for (size_t i = 0; i < NAPPS; i++) {
        sqlite3_close(*apps[i].conn); /* no-op on NULL */
        *apps[i].conn = NULL;
    }
}
