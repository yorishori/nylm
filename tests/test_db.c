/* The data folder, database settings, passwords and sessions, against real
 * temporary databases. */
#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/arena.h"
#include "../src/auth.h"
#include "../src/db.h"
#include "test.h"

/* A request carrying the session cookie that res set (or none). */
static void request_with_cookie_from(const struct response *res, struct request *req,
                                     char *cookie, size_t size)
{
    memset(req, 0, sizeof *req);
    cookie[0] = '\0';
    for (size_t i = 0; i < res->nextra; i++)
        if (strcmp(res->extra[i].name, "Set-Cookie") == 0)
            snprintf(cookie, size, "%.*s", (int)strcspn(res->extra[i].value, ";"),
                     res->extra[i].value);
    req->headers[0].name = "Cookie";
    req->headers[0].value = cookie;
    req->nheaders = 1;
}

/* 1 if conn has a table with this name. */
static int has_table(sqlite3 *conn, const char *name)
{
    sqlite3_stmt *st = db_prepare(conn, "SELECT 1 FROM sqlite_schema WHERE name = ?");
    int found = st != NULL && sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC) == SQLITE_OK &&
                sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return found;
}

/* PRAGMA user_version of conn, -1 on error. */
static int version(sqlite3 *conn)
{
    sqlite3_stmt *st = db_prepare(conn, "PRAGMA user_version");
    int v = st != NULL && sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
    sqlite3_finalize(st);
    return v;
}

/* Mode bits of path, -1 if it does not exist. */
static int mode_of(const char *path)
{
    struct stat sb;
    return stat(path, &sb) == 0 ? (int)(sb.st_mode & 0777) : -1;
}

/* Rejected data folders: nothing is created or left open. */
static void test_bad_data_dirs(const char *dir)
{
    char path[256];

    CHECK(db_open_all(NULL) == -1);
    CHECK(db_open_all("") == -1);

    snprintf(path, sizeof path, "%s/missing", dir);
    CHECK(db_open_all(path) == -1);
    CHECK(mode_of(path) == -1); /* a missing folder is never created */

    snprintf(path, sizeof path, "%s/file", dir);
    FILE *f = fopen(path, "w");
    CHECK(f != NULL && fclose(f) == 0);
    CHECK(db_open_all(path) == -1); /* not a folder */
    CHECK(unlink(path) == 0);

    /* an app's folder name taken by a file: fails, core is closed again */
    snprintf(path, sizeof path, "%s/blocked", dir);
    CHECK(mkdir(path, 0700) == 0);
    snprintf(path, sizeof path, "%s/blocked/plants", dir);
    f = fopen(path, "w");
    CHECK(f != NULL && fclose(f) == 0);
    snprintf(path, sizeof path, "%s/blocked", dir);
    CHECK(db_open_all(path) == -1);
    CHECK(core_db == NULL && plants_db == NULL);
    static const char *const blocked[] = {
        "blocked/plants", "blocked/core/core.db", "blocked/core/core.db-wal",
        "blocked/core/core.db-shm",
    };
    for (size_t i = 0; i < sizeof blocked / sizeof blocked[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, blocked[i]);
        unlink(path);
    }
    snprintf(path, sizeof path, "%s/blocked/core", dir);
    CHECK(rmdir(path) == 0);
    snprintf(path, sizeof path, "%s/blocked", dir);
    CHECK(rmdir(path) == 0);

    /* a path that cannot fit with the app folders after it */
    static char long_dir[DB_MAX_PATH + 16];
    memset(long_dir, 'a', DB_MAX_PATH);
    long_dir[0] = '/';
    long_dir[DB_MAX_PATH] = '\0';
    CHECK(db_open_all(long_dir) == -1);
}

int main(void)
{
    char dir[] = "/tmp/nylm-test-XXXXXX";
    char path[256];
    if (mkdtemp(dir) == NULL || arena_init(64 * 1024) != 0)
        return 1;

    test_bad_data_dirs(dir);

    /* an empty data folder: every app gets a new folder and database */
    CHECK(db_open_all(dir) == 0);
    snprintf(path, sizeof path, "%s/core", dir);
    CHECK(mode_of(path) == 0700);
    snprintf(path, sizeof path, "%s/plants", dir);
    CHECK(mode_of(path) == 0700);
    snprintf(path, sizeof path, "%s/core/core.db", dir);
    CHECK(mode_of(path) >= 0);
    snprintf(path, sizeof path, "%s/plants/plants.db", dir);
    CHECK(mode_of(path) >= 0);

    /* each app's tables live only in its own database */
    CHECK(has_table(core_db, "user") && has_table(core_db, "sessions"));
    CHECK(!has_table(core_db, "plants"));
    CHECK(has_table(plants_db, "plants") && has_table(plants_db, "care_log"));
    CHECK(!has_table(plants_db, "user") && !has_table(plants_db, "sessions"));
    CHECK(version(core_db) == core_migration_count);
    CHECK(version(plants_db) == plants_migration_count);

    /* opening again finds the databases and applies nothing twice */
    db_close_all();
    CHECK(core_db == NULL && plants_db == NULL);
    CHECK(db_open_all(dir) == 0);
    CHECK(version(core_db) == core_migration_count);

    /* settings the system SQLite library must have after db_open() */
    CHECK(db_exec(core_db, "SELECT \"no_such_column\"") != 0); /* DQS off: not a string */
    sqlite3_stmt *st = db_prepare(plants_db, "PRAGMA foreign_keys");
    CHECK(st != NULL && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1);
    sqlite3_finalize(st);

    /* passwords */
    CHECK(auth_check_password("anything") == 0); /* none set yet */
    CHECK(auth_set_password("first password") == 0);
    CHECK(auth_check_password("first password") == 1);
    CHECK(auth_check_password("First password") == 0);
    CHECK(auth_check_password("") == 0);

    /* a session works, and only with the exact token */
    struct response res;
    struct request req;
    char cookie[128];
    http_response_init(&res);
    CHECK(auth_start_session(&res) == 0);
    request_with_cookie_from(&res, &req, cookie, sizeof cookie);
    CHECK(strncmp(cookie, "nylm_session=", 13) == 0 && strlen(cookie) == 13 + 64);
    CHECK(auth_session_valid(&req) == 1);

    cookie[13] = cookie[13] == 'a' ? 'b' : 'a'; /* one character off */
    CHECK(auth_session_valid(&req) == 0);
    cookie[13] = '\0';                            /* empty value */
    CHECK(auth_session_valid(&req) == 0);
    struct request none;
    memset(&none, 0, sizeof none);
    CHECK(auth_session_valid(&none) == 0);        /* no cookie at all */

    /* logout deletes the session server-side and clears the cookie */
    http_response_init(&res);
    CHECK(auth_start_session(&res) == 0);
    request_with_cookie_from(&res, &req, cookie, sizeof cookie);
    struct response out;
    http_response_init(&out);
    CHECK(auth_end_session(&req, &out) == 0);
    CHECK(auth_session_valid(&req) == 0);
    CHECK(out.nextra == 1 && strstr(out.extra[0].value, "Max-Age=0") != NULL);
    http_response_init(&out);
    CHECK(auth_end_session(&none, &out) == 0); /* logging out without a session is fine */

    /* a new password logs out every session, in the same transaction */
    http_response_init(&res);
    CHECK(auth_start_session(&res) == 0);
    request_with_cookie_from(&res, &req, cookie, sizeof cookie);
    CHECK(auth_session_valid(&req) == 1);
    CHECK(auth_set_password("second password") == 0);
    CHECK(auth_session_valid(&req) == 0);
    CHECK(auth_check_password("first password") == 0);
    CHECK(auth_check_password("second password") == 1);

    db_close_all();
    static const char *const files[] = {
        "core/core.db", "core/core.db-wal", "core/core.db-shm",
        "plants/plants.db", "plants/plants.db-wal", "plants/plants.db-shm",
        "music/music.db", "music/music.db-wal", "music/music.db-shm",
    };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, files[i]);
        unlink(path); /* the -wal/-shm files may already be gone */
    }
    static const char *const dirs[] = { "core", "plants", "music", "" };
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, dirs[i]);
        CHECK(rmdir(path) == 0);
    }
    TEST_DONE();
}
