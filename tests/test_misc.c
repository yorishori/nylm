/* Small pure functions: cookie parsing, redirect targets, arena. */

#include "../src/arena.h"
#include "../src/auth.h"
#include "../src/server.h"
#include "test.h"

static const char *cookie(const char *header, const char *name)
{
    static char out[128];
    const char *v;
    int len = auth_cookie_value(header, name, &v);
    if (len < 0)
        return NULL;
    snprintf(out, sizeof out, "%.*s", len, v);
    return out;
}

static void test_cookies(void)
{
    CHECK_STR(cookie("nylm_session=abc", "nylm_session"), "abc");
    CHECK_STR(cookie("a=1; nylm_session=abc; b=2", "nylm_session"), "abc");
    CHECK_STR(cookie("a=1;nylm_session=abc", "nylm_session"), "abc");
    CHECK_STR(cookie("nylm_session=", "nylm_session"), "");
    CHECK(cookie("x_nylm_session=abc", "nylm_session") == NULL);
    CHECK(cookie("nylm_sessionX=abc", "nylm_session") == NULL);
    CHECK(cookie("", "nylm_session") == NULL);
    CHECK(cookie("; ;", "nylm_session") == NULL);
}

static void test_redirects(void)
{
    CHECK_STR(server_redirect_location("example.com", "/", "", 443), "https://example.com/");
    CHECK_STR(server_redirect_location("example.com:80", "/a", "x=1", 443),
              "https://example.com/a?x=1");
    CHECK_STR(server_redirect_location("localhost:8080", "/a b", "", 8443),
              "https://localhost:8443/a%20b");
    CHECK_STR(server_redirect_location("h", "/\"<x>", "q=\t", 443),
              "https://h/%22%3Cx%3E?q=%09");

    CHECK(server_redirect_location(NULL, "/", "", 443) == NULL);
    CHECK(server_redirect_location("", "/", "", 443) == NULL);
    CHECK(server_redirect_location(":443", "/", "", 443) == NULL);
    CHECK(server_redirect_location("evil.com/x", "/", "", 443) == NULL);
    CHECK(server_redirect_location("a b", "/", "", 443) == NULL);
    CHECK(server_redirect_location("[::1]", "/", "", 443) == NULL);
}

static void test_arena(void)
{
    arena_reset();
    char *a = arena_alloc(3);
    char *b = arena_alloc(8);
    CHECK(a != NULL && b != NULL);
    CHECK((size_t)b % _Alignof(max_align_t) == 0);
    CHECK(arena_alloc((size_t)-1) == NULL);
    CHECK(arena_alloc(1 << 20) == NULL); /* larger than the test arena */
    CHECK_STR(arena_strndup("hello world", 5), "hello");
    arena_reset();
    CHECK(arena_alloc(1) == a); /* reset reuses the same memory */
}

int main(void)
{
    if (arena_init(64 * 1024) != 0)
        return 1;
    test_cookies();
    test_redirects();
    test_arena();
    TEST_DONE();
}
