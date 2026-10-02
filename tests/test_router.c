#include "../src/router.h"
#include "test.h"

static void h(struct request *req, struct response *res)
{
    (void)req;
    (void)res;
}

static const struct route table[] = {
    { "GET",  "/api/items",  h, 0 },
    { "POST", "/api/items",  h, 0 },
    { "GET",  "/api/health", h, 1 },
};
#define N (sizeof table / sizeof table[0])

int main(void)
{
    CHECK(router_find(table, N, "GET", "/api/items") == 0);
    CHECK(router_find(table, N, "POST", "/api/items") == 1);
    CHECK(router_find(table, N, "GET", "/api/health") == 2);

    /* wrong method on a known path */
    CHECK(router_find(table, N, "DELETE", "/api/items") == ROUTE_METHOD_NOT_ALLOWED);
    CHECK(router_find(table, N, "POST", "/api/health") == ROUTE_METHOD_NOT_ALLOWED);

    /* matching is exact */
    CHECK(router_find(table, N, "GET", "/api/nope") == ROUTE_NOT_FOUND);
    CHECK(router_find(table, N, "GET", "/api/items/") == ROUTE_NOT_FOUND);
    CHECK(router_find(table, N, "GET", "/api/items/1") == ROUTE_NOT_FOUND);
    CHECK(router_find(table, N, "GET", "/api/itemsX") == ROUTE_NOT_FOUND);
    CHECK(router_find(table, N, "GET", "/API/items") == ROUTE_NOT_FOUND);
    CHECK(router_find(table, N, "GET", "") == ROUTE_NOT_FOUND);

    TEST_DONE();
}
