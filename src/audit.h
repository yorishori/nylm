#ifndef AUDIT_H
#define AUDIT_H

#include <cjson/cJSON.h>
#include <sqlite3.h>

#include "http.h"

/*
 * Dangerous actions from the web app: the password again in the same
 * request, and a row in the app's own `audit` table (client, action,
 * detail, result; see src/migrations.c).
 */

/* Every failed password check costs the caller this long, as for login. */
#define PASSWORD_FAILURE_DELAY_SECONDS 1

/* Reads body's "password" and checks it. 1 if it is right; else replies
 * (400 missing, 403 wrong after the delay, 500) and returns 0. */
int audit_password_ok(const cJSON *body, struct response *res);

/* Records that an action starts, result 'started'. Its row id, or -1
 * (logged): then the action must not happen. */
long long audit_begin(sqlite3 *db, const struct request *req, const char *action,
                      const char *detail);

/* Records an action's result ("ok..." or "failed: why"; logged too unless ok). */
void audit_end(sqlite3 *db, long long id, const char *result);

#endif
