#define _POSIX_C_SOURCE 200809L

#include <unistd.h>

#include "api.h"
#include "auth.h"
#include "json.h"

/* Every failed login costs the caller this long (and blocks the server). */
#define LOGIN_FAILURE_DELAY_SECONDS 1

void session_login(struct request *req, struct response *res)
{
    cJSON *obj = json_body(req, res);
    if (obj == NULL)
        return;
    const char *password;
    if (json_get_string(obj, "password", 1, AUTH_MAX_PASSWORD, &password) != NULL) {
        json_error(res, 400, "'password' is required");
        return;
    }

    int ok = auth_check_password(password);
    if (ok < 0) {
        json_error(res, 500, "internal error");
        return;
    }
    if (ok == 0) {
        sleep(LOGIN_FAILURE_DELAY_SECONDS);
        json_error(res, 401, "wrong password");
        return;
    }
    if (auth_start_session(res) != 0) {
        json_error(res, 500, "internal error");
        return;
    }
    res->status = 204;
}

void session_logout(struct request *req, struct response *res)
{
    if (auth_end_session(req, res) != 0) {
        json_error(res, 500, "logout failed");
        return;
    }
    res->status = 204;
}

void session_check(struct request *req, struct response *res)
{
    /* The router only lets authenticated requests get here. */
    (void)req;
    res->status = 204;
}
