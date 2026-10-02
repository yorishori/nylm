#ifndef DB_H
#define DB_H

#include <sqlite3.h>

/*
 * Every app has its own folder and SQLite database under the data folder
 * (NYLM_DATA): <data>/<app>/<app>.db. Code uses only its own app's
 * connection. To add an app: a migrations list in src/migrations.c, a
 * connection here, and a line in apps[] in src/db.c.
 */

#define DB_MAX_PATH 4096

/* core: login password and sessions. */
extern const char *const core_migrations[];
extern const int core_migration_count;
extern sqlite3 *core_db;

/* plants: plant care. */
extern const char *const plants_migrations[];
extern const int plants_migration_count;
extern sqlite3 *plants_db;

/*
 * Opens every app's database under data_dir, which must already exist (it
 * lives on the data drive: missing means not mounted). Creates missing app
 * folders (mode 700) and databases, sets pragmas, applies pending
 * migrations. On failure logs why, closes what was opened, returns -1.
 */
int db_open_all(const char *data_dir);
void db_close_all(void);

/* Opens one database file with nylm's settings and applies migrations. */
sqlite3 *db_open(const char *path, const char *const *migrations, int count);

/* Runs SQL without parameters (no data!); logs and returns -1 on failure. */
int db_exec(sqlite3 *conn, const char *sql);

/* sqlite3_prepare_v2 on conn; logs and returns NULL on failure. */
sqlite3_stmt *db_prepare(sqlite3 *conn, const char *sql);

/* Logs the last SQLite error of conn with some context. */
void db_log_error(sqlite3 *conn, const char *context);

#endif
