/* Small units: cookie parsing, subnets, arena, response headers, sockets, JSON. */

#include "../src/arena.h"
#include "../src/auth.h"
#include "../src/conn.h"
#include "../src/http.h"
#include "../src/json.h"
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

static void test_response_headers(void)
{
    struct response res;
    http_response_init(&res);
    for (int i = 0; i < HTTP_MAX_EXTRA; i++)
        CHECK(http_add_header(&res, "X-A", "1") == 0);
    CHECK(http_add_header(&res, "X-Overflow", "1") == -1); /* never silently dropped */
    CHECK(res.nextra == HTTP_MAX_EXTRA);
}

static void test_conn(void)
{
    struct conn c;
    CHECK(conn_init(&c, -1) == -1); /* timeouts cannot be set: must fail */
}

/* Runs json_body on a POST with the given content type and body. */
static cJSON *body(const char *type, const char *text, size_t len, struct response *res)
{
    static char buf[256];
    struct request req;
    memset(&req, 0, sizeof req);
    req.method = "POST";
    req.path = "/";
    if (type != NULL) {
        req.headers[0].name = "Content-Type";
        req.headers[0].value = type;
        req.nheaders = 1;
    }
    if (text != NULL) {
        memcpy(buf, text, len);
        buf[len] = '\0';
        req.body = buf;
        req.body_len = len;
    }
    http_response_init(res);
    return json_body(&req, res);
}

#define BODY(type, lit, res) body(type, lit, sizeof lit - 1, res)

static void test_json(void)
{
    struct response res;
    const char *J = "application/json";

    CHECK(BODY(J, "{\"a\":\"b\"}", &res) != NULL);
    CHECK(BODY("application/json; charset=utf-8", "{}", &res) != NULL);
    CHECK(BODY("Application/JSON", "{}", &res) != NULL);

    CHECK(BODY(NULL, "{}", &res) == NULL && res.status == 415);
    CHECK(BODY("text/plain", "{}", &res) == NULL && res.status == 415);
    CHECK(BODY("application/jsonx", "{}", &res) == NULL && res.status == 415);
    CHECK(body(J, NULL, 0, &res) == NULL && res.status == 400);
    CHECK(BODY(J, "{\"a\":", &res) == NULL && res.status == 400);
    CHECK(BODY(J, "[1]", &res) == NULL && res.status == 400);
    CHECK(BODY(J, "\"x\"", &res) == NULL && res.status == 400);

    /* NUL in any form is rejected, never silently truncated */
    CHECK(BODY(J, "{\"a\":\"x\\u0000y\"}", &res) == NULL && res.status == 400);
    CHECK(BODY(J, "{\"a\":\"x\0y\"}", &res) == NULL && res.status == 400);

    cJSON *obj = BODY(J, "{\"s\":\"abc\",\"n\":1,\"e\":\"\"}", &res);
    const char *out = NULL;
    CHECK(obj != NULL);
    CHECK(json_get_string(obj, "s", 1, 3, &out) == NULL);
    CHECK_STR(out, "abc");
    CHECK(json_get_string(obj, "e", 0, 3, &out) == NULL);
    CHECK_STR(out, "");
    CHECK_STR(json_get_string(obj, "s", 4, 9, &out), "'s' must be 4 to 9 bytes");
    CHECK_STR(json_get_string(obj, "s", 0, 2, &out), "'s' must be 0 to 2 bytes");
    CHECK_STR(json_get_string(obj, "e", 1, 2, &out), "'e' must be 1 to 2 bytes");
    CHECK_STR(json_get_string(obj, "n", 0, 9, &out), "'n' must be a string");
    CHECK_STR(json_get_string(obj, "missing", 0, 9, &out), "'missing' must be a string");
}

int main(void)
{
    if (arena_init(64 * 1024) != 0)
        return 1;
    json_init();
    test_cookies();
    test_subnets();
    test_arena();
    test_response_headers();
    test_conn();
    test_json();
    TEST_DONE();
}
