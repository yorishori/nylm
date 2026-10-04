#define _POSIX_C_SOURCE 200809L

#include "audit.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "auth.h"
#include "db.h"
#include "json.h"

int audit_password_ok(const cJSON *body, struct response *res)
{
    const char *password;
    if (json_get_string(body, "password", 1, AUTH_MAX_PASSWORD, &password) != NULL) {
        json_error(res, 400, "'password' is required");
        return 0;
    }
    int ok = auth_check_password(password);
    if (ok < 0) {
        json_error(res, 500, "internal error");
        return 0;
    }
    if (ok == 0) {
        sleep(PASSWORD_FAILURE_DELAY_SECONDS);
        json_error(res, 403, "wrong password");
        return 0;
    }
    return 1;
}

long long audit_begin(sqlite3 *db, const struct request *req, const char *action,
                      const char *detail)
{
    sqlite3_stmt *st = db_prepare(db, "INSERT INTO audit (client, action, detail, result)"
                                      " VALUES (?, ?, ?, 'started')");
    int ok = st != NULL &&
             sqlite3_bind_text(st, 1, req->client ? req->client : "-", -1, SQLITE_STATIC) ==
                 SQLITE_OK &&
             sqlite3_bind_text(st, 2, action, -1, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_bind_text(st, 3, detail, -1, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_step(st) == SQLITE_DONE;
    if (!ok)
        fprintf(stderr, "audit: can not record %s: %s\n", action, sqlite3_errmsg(db));
    sqlite3_finalize(st);
    return ok ? (long long)sqlite3_last_insert_rowid(db) : -1;
}

void audit_end(sqlite3 *db, long long id, const char *result)
{
    sqlite3_stmt *st = db_prepare(db, "UPDATE audit SET result = ? WHERE id = ?");
    int ok = st != NULL && sqlite3_bind_text(st, 1, result, -1, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_bind_int64(st, 2, id) == SQLITE_OK && sqlite3_step(st) == SQLITE_DONE;
    if (!ok)
        fprintf(stderr, "audit: can not record result %lld '%s': %s\n", id, result,
                sqlite3_errmsg(db));
    sqlite3_finalize(st);
    if (strncmp(result, "ok", 2) != 0)
        fprintf(stderr, "audit %lld: %s\n", id, result);
}
