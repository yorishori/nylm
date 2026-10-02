#include "../src/router.h"
#include "test.h"

static void h(struct request *req, struct response *res)
{
    (void)req;
    (void)res;
}

static const struct route table[] = {
    { "GET",    "/api/notes",   h, 0 },
    { "POST",   "/api/notes",   h, 0 },
    { "GET",    "/api/notes/:", h, 0 },
    { "DELETE", "/api/notes/:", h, 0 },
    { "GET",    "/api/health",  h, 1 },
};
#define N (sizeof table / sizeof table[0])

int main(void)
{
    const char *param = NULL;

    CHECK(router_find(table, N, "GET", "/api/notes", &param) == 0);
    CHECK(router_find(table, N, "POST", "/api/notes", &param) == 1);

    param = NULL;
    CHECK(router_find(table, N, "GET", "/api/notes/42", &param) == 2);
    CHECK_STR(param, "42");
    CHECK(router_find(table, N, "DELETE", "/api/notes/abc", &param) == 3);
    CHECK_STR(param, "abc");

    CHECK(router_find(table, N, "GET", "/api/health", &param) == 4);

    /* wrong method on a known path */
    CHECK(router_find(table, N, "DELETE", "/api/notes", &param) == ROUTE_METHOD_NOT_ALLOWED);
    CHECK(router_find(table, N, "PUT", "/api/notes/1", &param) == ROUTE_METHOD_NOT_ALLOWED);

    /* unknown paths; the ':' segment must be exactly one non-empty segment */
    CHECK(router_find(table, N, "GET", "/api/nope", &param) == ROUTE_NOT_FOUND);
    CHECK(router_find(table, N, "GET", "/api/notes/", &param) == ROUTE_NOT_FOUND);
    CHECK(router_find(table, N, "GET", "/api/notes/1/x", &param) == ROUTE_NOT_FOUND);
    CHECK(router_find(table, N, "GET", "/api/notesX", &param) == ROUTE_NOT_FOUND);
    CHECK(router_find(table, N, "GET", "/api/health/", &param) == ROUTE_NOT_FOUND);

    TEST_DONE();
}
