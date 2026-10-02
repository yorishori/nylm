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

    /* whole numbers in a range */
    obj = BODY(J, "{\"n\":5,\"z\":0,\"neg\":-1,\"f\":1.5,\"big\":1e300,\"s\":\"5\","
                  "\"t\":true,\"no\":false,\"nul\":null}", &res);
    long n = 0;
    CHECK(obj != NULL);
    CHECK(json_get_int(obj, "n", 1, 5, &n) == NULL && n == 5); /* max */
    CHECK(json_get_int(obj, "n", 5, 9, &n) == NULL && n == 5); /* min */
    CHECK(json_get_int(obj, "z", 0, 0, &n) == NULL && n == 0);
    CHECK(json_get_int(obj, "neg", -1, 1, &n) == NULL && n == -1);
    CHECK_STR(json_get_int(obj, "n", 1, 4, &n), "'n' must be a whole number from 1 to 4");
    CHECK_STR(json_get_int(obj, "n", 6, 9, &n), "'n' must be a whole number from 6 to 9");
    CHECK(json_get_int(obj, "f", 0, 9, &n) != NULL);
    CHECK(json_get_int(obj, "big", 0, 9007199254740991L, &n) != NULL);
    CHECK(json_get_int(obj, "s", 0, 9, &n) != NULL);
    CHECK(json_get_int(obj, "t", 0, 9, &n) != NULL);
    CHECK(json_get_int(obj, "nul", 0, 9, &n) != NULL);
    CHECK(json_get_int(obj, "missing", 0, 9, &n) != NULL);

    /* booleans */
    int b = -1;
    CHECK(json_get_bool(obj, "t", &b) == NULL && b == 1);
    CHECK(json_get_bool(obj, "no", &b) == NULL && b == 0);
    CHECK_STR(json_get_bool(obj, "z", &b), "'z' must be true or false");
    CHECK(json_get_bool(obj, "nul", &b) != NULL);
    CHECK(json_get_bool(obj, "missing", &b) != NULL);

    /* text: length, then UTF-8 and control characters */
    obj = BODY(J, "{\"a\":\"Monstera \\u00e9\\u6728\\ud83c\\udf31\",\"nl\":\"a\\nb\","
                  "\"tab\":\"a\\tb\",\"e\":\"\"}", &res);
    CHECK(obj != NULL);
    CHECK(json_get_text(obj, "a", 1, 100, 0, &out) == NULL);
    CHECK_STR(out, "Monstera \xc3\xa9\xe6\x9c\xa8\xf0\x9f\x8c\xb1");
    CHECK(json_get_text(obj, "e", 0, 1, 0, &out) == NULL);
    CHECK(json_get_text(obj, "nl", 0, 9, 1, &out) == NULL);
    CHECK_STR(json_get_text(obj, "nl", 0, 9, 0, &out), "'nl' must be one line of UTF-8 text");
    CHECK_STR(json_get_text(obj, "tab", 0, 9, 1, &out),
              "'tab' must be UTF-8 text without control characters");
    CHECK_STR(json_get_text(obj, "e", 1, 9, 0, &out), "'e' must be 1 to 9 bytes");
}

static void test_text_valid(void)
{
    CHECK(text_valid("", 0));
    CHECK(text_valid("plain ascii ~", 0));
    CHECK(text_valid("two\nlines", 1));
    CHECK(!text_valid("two\nlines", 0));
    CHECK(!text_valid("cr\r", 1));
    CHECK(!text_valid("tab\t", 1));
    CHECK(!text_valid("del\x7f", 1));
    CHECK(!text_valid("esc\x1b[0m", 1));

    CHECK(text_valid("\xc3\xa9", 0));             /* U+00E9 */
    CHECK(text_valid("\xc2\xa0", 0));             /* U+00A0, first after C1 */
    CHECK(!text_valid("\xc2\x85", 0));            /* U+0085, C1 control */
    CHECK(!text_valid("\xc2\x9f", 0));            /* U+009F, C1 control */
    CHECK(text_valid("\xe2\x82\xac", 0));         /* U+20AC */
    CHECK(text_valid("\xef\xbf\xbf", 0));         /* U+FFFF */
    CHECK(text_valid("\xf0\x90\x80\x80", 0));     /* U+10000 */
    CHECK(text_valid("\xf4\x8f\xbf\xbf", 0));     /* U+10FFFF, max */

    CHECK(!text_valid("\x80", 0));                /* lone continuation */
    CHECK(!text_valid("\xc0\xaf", 0));            /* overlong '/' */
    CHECK(!text_valid("\xc1\xbf", 0));            /* overlong */
    CHECK(!text_valid("\xe0\x80\xaf", 0));        /* overlong */
    CHECK(!text_valid("\xf0\x80\x80\xaf", 0));    /* overlong */
    CHECK(!text_valid("\xed\xa0\x80", 0));        /* surrogate U+D800 */
    CHECK(!text_valid("\xf4\x90\x80\x80", 0));    /* U+110000, max + 1 */
    CHECK(!text_valid("\xf5\x80\x80\x80", 0));
    CHECK(!text_valid("\xff", 0));
    CHECK(!text_valid("\xc3", 0));                /* truncated */
    CHECK(!text_valid("\xe2\x82", 0));
    CHECK(!text_valid("\xf0\x9f\x8c", 0));
    CHECK(!text_valid("\xc3(", 0));               /* bad continuation */
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
    test_text_valid();
    TEST_DONE();
}
