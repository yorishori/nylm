/* Small pure functions: cookie parsing, subnets, arena. */

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

static uint32_t ip(int a, int b, int c, int d)
{
    return (uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)c << 8 | (uint32_t)d;
}

static void test_subnets(void)
{
    struct subnet s;

    CHECK(subnet_parse("10.0.0.0/24", &s) == 0);
    CHECK(s.addr == ip(10, 0, 0, 0) && s.mask == 0xffffff00u);
    CHECK(subnet_parse("192.168.1.37/24", &s) == 0); /* host bits dropped */
    CHECK(s.addr == ip(192, 168, 1, 0));
    CHECK(subnet_parse("10.0.0.1", &s) == 0);
    CHECK(s.addr == ip(10, 0, 0, 1) && s.mask == 0xffffffffu);
    CHECK(subnet_parse("0.0.0.0/0", &s) == 0);
    CHECK(s.mask == 0);

    CHECK(subnet_parse("", &s) == -1);
    CHECK(subnet_parse("10.0.0.0/", &s) == -1);
    CHECK(subnet_parse("10.0.0.0/33", &s) == -1);
    CHECK(subnet_parse("10.0.0.0/-1", &s) == -1);
    CHECK(subnet_parse("10.0.0.0/2x", &s) == -1);
    CHECK(subnet_parse("10.0.0/24", &s) == -1);
    CHECK(subnet_parse("10.0.0.256", &s) == -1);
    CHECK(subnet_parse("example.com", &s) == -1);
    CHECK(subnet_parse("::1", &s) == -1);

    struct subnet list[2];
    subnet_parse("10.0.0.0/24", &list[0]);
    subnet_parse("192.168.1.0/24", &list[1]);
    CHECK(subnet_allowed(list, 2, ip(10, 0, 0, 2)));
    CHECK(subnet_allowed(list, 2, ip(192, 168, 1, 200)));
    CHECK(!subnet_allowed(list, 2, ip(10, 0, 1, 2)));
    CHECK(!subnet_allowed(list, 2, ip(192, 168, 2, 1)));
    CHECK(!subnet_allowed(list, 2, ip(8, 8, 8, 8)));
    CHECK(!subnet_allowed(list, 0, ip(10, 0, 0, 2)));
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
    test_subnets();
    test_arena();
    TEST_DONE();
}
