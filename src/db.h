#ifndef DB_H
#define DB_H

#include <sqlite3.h>

/* The schema, in order (src/migrations.c). */
extern const char *const migrations[];
extern const int migration_count;

/* The one database connection, open for the life of the process. */
extern sqlite3 *db;

/* Opens the database, sets pragmas and applies pending migrations. */
int db_open(const char *path);
void db_close(void);

/* Runs SQL without parameters (no data!); logs and returns -1 on failure. */
int db_exec(const char *sql);

/* sqlite3_prepare_v2 on db; logs and returns NULL on failure. */
sqlite3_stmt *db_prepare(const char *sql);

/* Logs the last SQLite error with some context. */
void db_log_error(const char *context);

#endif
