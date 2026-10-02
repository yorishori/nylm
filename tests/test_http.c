#include "../src/http.h"
#include "test.h"

/* Parses a request head given as a string literal (copied: parsing edits it). */
static int parse(const char *text, struct request *req)
{
    static char buf[HTTP_MAX_HEAD + 64];
    size_t len = strlen(text);
    memcpy(buf, text, len + 1);
    return http_parse_head(buf, len, req);
}

static void test_request_line(void)
{
    struct request req;

    CHECK(parse("GET / HTTP/1.1\r\n\r\n", &req) == 0);
    CHECK_STR(req.method, "GET");
    CHECK_STR(req.path, "/");

    CHECK(parse("POST /api/notes?a=1&b=%20 HTTP/1.0\r\n\r\n", &req) == 0);
    CHECK_STR(req.method, "POST");
    CHECK_STR(req.path, "/api/notes"); /* query string dropped */

    CHECK(parse("GET /a%20b HTTP/1.1\r\n\r\n", &req) == 0);
    CHECK_STR(req.path, "/a b");

    CHECK(parse("PATCH / HTTP/1.1\r\n\r\n", &req) == 501);
    CHECK(parse("G(T / HTTP/1.1\r\n\r\n", &req) == 400);
    CHECK(parse("GET / HTTP/2.0\r\n\r\n", &req) == 505);
    CHECK(parse("GET / FTP/1.0\r\n\r\n", &req) == 400);
    CHECK(parse("GET /\r\n\r\n", &req) == 400);
    CHECK(parse("GET  / HTTP/1.1\r\n\r\n", &req) == 400);
    CHECK(parse("GET http://x/ HTTP/1.1\r\n\r\n", &req) == 400);
    CHECK(parse("GET /%00 HTTP/1.1\r\n\r\n", &req) == 400);
    CHECK(parse("GET /%0a HTTP/1.1\r\n\r\n", &req) == 400);
    CHECK(parse("GET /%zz HTTP/1.1\r\n\r\n", &req) == 400);
    CHECK(parse("GET /% HTTP/1.1\r\n\r\n", &req) == 400);
    CHECK(parse("GET / HTTP/1.1\n\n", &req) == 400);

    static char long_path[HTTP_MAX_PATH + 64];
    memcpy(long_path, "GET /", 5);
    memset(long_path + 5, 'a', HTTP_MAX_PATH);
    snprintf(long_path + 5 + HTTP_MAX_PATH, sizeof long_path - 5 - HTTP_MAX_PATH,
             " HTTP/1.1\r\n\r\n");
    CHECK(parse(long_path, &req) == 414);
}

static void test_headers(void)
{
    struct request req;

    CHECK(parse("GET / HTTP/1.1\r\nHost: example.com\r\nX-Pad:   v  \r\n\r\n", &req) == 0);
    CHECK(req.nheaders == 2);
    CHECK_STR(http_header(&req, "host"), "example.com");
    CHECK_STR(http_header(&req, "HOST"), "example.com");
    CHECK_STR(http_header(&req, "x-pad"), "v");
    CHECK(http_header(&req, "missing") == NULL);

    CHECK(parse("GET / HTTP/1.1\r\nNo colon\r\n\r\n", &req) == 400);
    CHECK(parse("GET / HTTP/1.1\r\n: empty name\r\n\r\n", &req) == 400);
    CHECK(parse("GET / HTTP/1.1\r\nBad Name: x\r\n\r\n", &req) == 400);
    CHECK(parse("GET / HTTP/1.1\r\nA: 1\r\n folded\r\n\r\n", &req) == 400);
    CHECK(parse("GET / HTTP/1.1\r\nA: x\x01y\r\n\r\n", &req) == 400);
    CHECK(parse("GET / HTTP/1.1\r\nA: x\r\n", &req) == 400); /* no blank line */
}

static void test_content_length(void)
{
    struct request req;

    CHECK(parse("POST / HTTP/1.1\r\nContent-Length: 12\r\n\r\n", &req) == 0);
    CHECK(req.content_length == 12);
    CHECK(parse("POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\n", &req) == 0);
    CHECK(req.content_length == 0);
    CHECK(parse("POST / HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n",
                &req) == 0);
    CHECK(parse("POST / HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n",
                &req) == 400);
    CHECK(parse("POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n", &req) == 400);
    CHECK(parse("POST / HTTP/1.1\r\nContent-Length: 1x\r\n\r\n", &req) == 400);
    CHECK(parse("POST / HTTP/1.1\r\nContent-Length: \r\n\r\n", &req) == 400);
    CHECK(parse("POST / HTTP/1.1\r\nContent-Length: 1048577\r\n\r\n", &req) == 413);
    CHECK(parse("POST / HTTP/1.1\r\nContent-Length: 99999999999999999999999\r\n\r\n",
                &req) == 413);
    CHECK(parse("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", &req) == 501);
}

static void test_url_decode(void)
{
    char s[64];

    snprintf(s, sizeof s, "%s", "/plain");
    CHECK(http_url_decode(s) == 0);
    CHECK_STR(s, "/plain");

    snprintf(s, sizeof s, "%s", "%41%42c%2F%2e");
    CHECK(http_url_decode(s) == 0);
    CHECK_STR(s, "ABc/.");

    snprintf(s, sizeof s, "%s", "a+b");
    CHECK(http_url_decode(s) == 0);
    CHECK_STR(s, "a+b"); /* '+' is literal in paths */

    snprintf(s, sizeof s, "%s", "%7f");
    CHECK(http_url_decode(s) == -1);
    snprintf(s, sizeof s, "%s", "%1");
    CHECK(http_url_decode(s) == -1);
    snprintf(s, sizeof s, "%s", "%g0");
    CHECK(http_url_decode(s) == -1);
}

int main(void)
{
    test_request_line();
    test_headers();
    test_content_length();
    test_url_decode();
    TEST_DONE();
}
