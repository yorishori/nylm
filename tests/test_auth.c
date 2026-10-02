/* Passwords and sessions against a real, temporary SQLite database. */
#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
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

int main(void)
{
    char dir[] = "/tmp/nylm-test-XXXXXX";
    char path[64];
    if (mkdtemp(dir) == NULL || arena_init(64 * 1024) != 0)
        return 1;
    snprintf(path, sizeof path, "%s/t.db", dir);
    CHECK(db_open(path) == 0);

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

    db_close();
    static const char *const files[] = { "t.db", "t.db-wal", "t.db-shm" };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, files[i]);
        unlink(path); /* the -wal/-shm files may already be gone */
    }
    CHECK(rmdir(dir) == 0);
    TEST_DONE();
}
