#include "../src/static.h"
#include "test.h"

static const char *safe(const char *url_path)
{
    static char out[256];
    return static_safe_path(url_path, out, sizeof out) == 0 ? out : NULL;
}

int main(void)
{
    CHECK_STR(safe("/"), "index.html");
    CHECK_STR(safe("/app.js"), "app.js");
    CHECK_STR(safe("/css/site.css"), "css/site.css");
    CHECK_STR(safe("/docs/"), "docs/index.html");
    CHECK_STR(safe("/a.b.c"), "a.b.c");

    CHECK(safe("") == NULL);
    CHECK(safe("app.js") == NULL);
    CHECK(safe("/..") == NULL);
    CHECK(safe("/../etc/passwd") == NULL);
    CHECK(safe("/css/../../x") == NULL);
    CHECK(safe("/./app.js") == NULL);
    CHECK(safe("/.git/config") == NULL);
    CHECK(safe("/.env") == NULL);
    CHECK(safe("/css/.hidden") == NULL);
    CHECK(safe("//etc/passwd") == NULL);
    CHECK(safe("/a//b") == NULL);
    CHECK(safe("/a\\..\\b") == NULL);

    char tiny[8];
    CHECK(static_safe_path("/much-too-long", tiny, sizeof tiny) == -1);

    CHECK_STR(static_content_type("index.html"), "text/html; charset=utf-8");
    CHECK_STR(static_content_type("a/b.js"), "text/javascript; charset=utf-8");
    CHECK_STR(static_content_type("x.svg"), "image/svg+xml");
    CHECK_STR(static_content_type("noext"), "application/octet-stream");
    CHECK_STR(static_content_type("x.HTML"), "application/octet-stream");

    TEST_DONE();
}
