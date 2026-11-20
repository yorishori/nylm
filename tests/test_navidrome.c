#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>

#include "../src/arena.h"
#include "../src/https.h"
#include "../src/json.h"
#include "../src/music.h"
#include "../src/navidrome.h"
#include "test.h"

/* A string of n copies of c, in a static buffer. */
static const char *repeat(char c, size_t n)
{
    static char buf[1024];
    memset(buf, c, n);
    buf[n] = '\0';
    return buf;
}

static void test_local_split(void)
{
    char host[HTTPS_MAX_HOST + 1];
    int port;
    const char *path;
    CHECK(http_local_split("http://127.0.0.1:4533/rest/ping?u=a", host, &port, &path) == 0);
    CHECK_STR(host, "127.0.0.1");
    CHECK(port == 4533);
    CHECK_STR(path, "/rest/ping?u=a");
    CHECK(http_local_split("http://127.1.2.3", host, &port, &path) == 0);
    CHECK(port == 80);
    CHECK_STR(path, "/");
    CHECK(http_local_split("http://127.0.0.1:65535/", host, &port, &path) == 0);
    /* Only this machine, only plain IPv4 addresses. */
    CHECK(http_local_split("http://128.0.0.1/", host, &port, &path) == -1);
    CHECK(http_local_split("http://10.0.0.1/", host, &port, &path) == -1);
    CHECK(http_local_split("http://localhost/", host, &port, &path) == -1);
    CHECK(http_local_split("http://127.0.0.1.evil.com/", host, &port, &path) == -1);
    CHECK(http_local_split("http://127.0.0/", host, &port, &path) == -1);
    CHECK(http_local_split("http://127.0.0.256/", host, &port, &path) == -1);
    CHECK(http_local_split("https://127.0.0.1/", host, &port, &path) == -1);
    CHECK(http_local_split("http://user@127.0.0.1/", host, &port, &path) == -1);
    CHECK(http_local_split("http://127.0.0.1:0/", host, &port, &path) == -1);
    CHECK(http_local_split("http://127.0.0.1:65536/", host, &port, &path) == -1);
    CHECK(http_local_split("http://127.0.0.1:-1/", host, &port, &path) == -1);
    CHECK(http_local_split("http://127.0.0.1:+1/", host, &port, &path) == -1);
    CHECK(http_local_split("http://127.0.0.1:x/", host, &port, &path) == -1);
    CHECK(http_local_split("http://127.0.0.1?x", host, &port, &path) == -1);
    CHECK(http_local_split("http://127.0.0.1/a b", host, &port, &path) == -1);
    CHECK(http_local_split("http://127.0.0.1/a\r\nHost: x", host, &port, &path) == -1);
    CHECK(http_local_split(NULL, host, &port, &path) == -1);
}

static void test_url_valid(void)
{
    CHECK(navidrome_url_valid("http://127.0.0.1:4533"));
    CHECK(navidrome_url_valid("http://127.0.0.1:4533/"));
    CHECK(navidrome_url_valid("http://127.0.0.1:4533/navidrome"));
    CHECK(navidrome_url_valid("http://127.0.0.1/music/nd-1.x_y"));
    CHECK(!navidrome_url_valid("http://127.0.0.1:4533/a/../b"));
    CHECK(!navidrome_url_valid("http://127.0.0.1:4533/a//b"));
    CHECK(!navidrome_url_valid("http://127.0.0.1:4533/./b"));
    CHECK(!navidrome_url_valid("http://127.0.0.1:4533/a b"));
    CHECK(!navidrome_url_valid("http://127.0.0.1:4533/a%20b"));
    CHECK(!navidrome_url_valid("http://127.0.0.1:4533/?x=1"));
    CHECK(!navidrome_url_valid("http://192.168.1.2:4533"));
    CHECK(!navidrome_url_valid("https://127.0.0.1:4533"));
    CHECK(!navidrome_url_valid(""));
    CHECK(!navidrome_url_valid(NULL));
    /* At most NAVIDROME_MAX_URL bytes. */
    char url[NAVIDROME_MAX_URL + 2];
    int n = snprintf(url, sizeof url, "http://127.0.0.1/");
    memset(url + n, 'a', sizeof url - (size_t)n - 2);
    url[NAVIDROME_MAX_URL] = '\0';
    CHECK(strlen(url) == NAVIDROME_MAX_URL && navidrome_url_valid(url));
    url[NAVIDROME_MAX_URL] = 'a';
    url[NAVIDROME_MAX_URL + 1] = '\0';
    CHECK(!navidrome_url_valid(url));
}

static void test_names(void)
{
    CHECK(navidrome_user_valid("admin"));
    CHECK(navidrome_user_valid("Zoë & co"));
    CHECK(navidrome_user_valid(repeat('u', NAVIDROME_MAX_USER)));
    CHECK(!navidrome_user_valid(repeat('u', NAVIDROME_MAX_USER + 1)));
    CHECK(!navidrome_user_valid(""));
    CHECK(!navidrome_user_valid("a\nb"));
    CHECK(!navidrome_user_valid("\xff"));
    CHECK(!navidrome_user_valid(NULL));
    CHECK(navidrome_password_valid("p"));
    CHECK(navidrome_password_valid(repeat('p', NAVIDROME_MAX_PASSWORD)));
    CHECK(!navidrome_password_valid(repeat('p', NAVIDROME_MAX_PASSWORD + 1)));
    CHECK(!navidrome_password_valid(""));
    CHECK(!navidrome_password_valid("a\tb"));
    CHECK(navidrome_id_valid("4WvGx0QEhPFq5c2Hj2t0WA"));
    CHECK(navidrome_id_valid("a1b2-c3_d4"));
    CHECK(navidrome_id_valid(repeat('i', NAVIDROME_MAX_ID)));
    CHECK(!navidrome_id_valid(repeat('i', NAVIDROME_MAX_ID + 1)));
    CHECK(!navidrome_id_valid(""));
    CHECK(!navidrome_id_valid("a&u=x"));
    CHECK(!navidrome_id_valid("a b"));
    CHECK(!navidrome_id_valid("a/b"));
    CHECK(!navidrome_id_valid(NULL));
}

static void test_call_url(void)
{
    char url[512];
    /* The Subsonic API documentation's example: sesame + c19b2d. */
    CHECK(navidrome_call_url("http://127.0.0.1:4533", "ping", "admin", "sesame", "c19b2d", "",
                             url, sizeof url) == 0);
    CHECK_STR(url, "http://127.0.0.1:4533/rest/ping?u=admin&t=26719a1196d2a940705a59634eb18eab"
                   "&s=c19b2d&v=1.16.1&c=nylm&f=json");
    /* The user name is escaped; extra comes last; the password never shows. */
    CHECK(navidrome_call_url("http://127.0.0.1/nd", "getPlaylist", "a b&c=d", "sesame",
                             "c19b2d", "&id=x1", url, sizeof url) == 0);
    const char *start = "http://127.0.0.1/nd/rest/getPlaylist?u=a%20b%26c%3Dd&t=";
    CHECK(strncmp(url, start, strlen(start)) == 0);
    CHECK(strstr(url, "&f=json&id=x1") != NULL);
    CHECK(strstr(url, "sesame") == NULL);
    /* Too small for it: refused, never cut. */
    CHECK(navidrome_call_url("http://127.0.0.1", "ping", "admin", "sesame", "c19b2d", "", url,
                             40) == -1);
}

static void test_answer(void)
{
    char err[300];
    int refused = -1;
    const char *ok = "{\"subsonic-response\":{\"status\":\"ok\",\"version\":\"1.16.1\","
                     "\"serverVersion\":\"0.64.2\"}}";
    cJSON *r = navidrome_answer(ok, strlen(ok), &refused, err, sizeof err);
    CHECK(r != NULL && refused == 0);
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r, "serverVersion")),
              "0.64.2");
    const char *failed = "{\"subsonic-response\":{\"status\":\"failed\",\"error\":{\"code\":40,"
                         "\"message\":\"Wrong username or password\"}}}";
    CHECK(navidrome_answer(failed, strlen(failed), &refused, err, sizeof err) == NULL);
    CHECK(refused == 1);
    CHECK_STR(err, "Navidrome says: Wrong username or password (code 40)");
    const char *bare = "{\"subsonic-response\":{\"status\":\"failed\"}}";
    CHECK(navidrome_answer(bare, strlen(bare), &refused, err, sizeof err) == NULL);
    CHECK(refused == 1);
    CHECK_STR(err, "Navidrome says: an error (code 0)");
    const char *html = "<html>not found</html>";
    CHECK(navidrome_answer(html, strlen(html), &refused, err, sizeof err) == NULL);
    CHECK(refused == 0);
    CHECK(strstr(err, "not one nylm can read") != NULL);
    const char *other = "{\"status\":\"ok\"}";
    CHECK(navidrome_answer(other, strlen(other), &refused, err, sizeof err) == NULL);
    CHECK(refused == 0);
    CHECK(navidrome_answer(NULL, 0, &refused, err, sizeof err) == NULL);
}

static void test_local_path(void)
{
    char out[64];
    CHECK(navidrome_local_path("/music", "/srv/m", "/music/A/B/01 - x.flac", out,
                               sizeof out) == 0);
    CHECK_STR(out, "/srv/m/A/B/01 - x.flac");
    CHECK(navidrome_local_path("/music", "/srv/m", "A/B/01.flac", out, sizeof out) == 0);
    CHECK_STR(out, "/srv/m/A/B/01.flac");
    CHECK(navidrome_local_path("/srv/m", "/srv/m", "/srv/m/x.mp3", out, sizeof out) == 0);
    CHECK_STR(out, "/srv/m/x.mp3");
    /* Outside the folder, or not a path in it. */
    CHECK(navidrome_local_path("/music", "/srv/m", "/musicx/A.flac", out, sizeof out) == -1);
    CHECK(navidrome_local_path("/music", "/srv/m", "/other/A.flac", out, sizeof out) == -1);
    CHECK(navidrome_local_path("/music", "/srv/m", "/music", out, sizeof out) == -1);
    CHECK(navidrome_local_path("/music", "/srv/m", "/music/", out, sizeof out) == -1);
    CHECK(navidrome_local_path("/music", "/srv/m", "/music/../etc/passwd", out, sizeof out) ==
          -1);
    CHECK(navidrome_local_path("/music", "/srv/m", "A/../../x", out, sizeof out) == -1);
    CHECK(navidrome_local_path("/music", "/srv/m", "A//x", out, sizeof out) == -1);
    CHECK(navidrome_local_path("/music", "/srv/m", "./x", out, sizeof out) == -1);
    CHECK(navidrome_local_path("/music", "/srv/m", "A/x\n", out, sizeof out) == -1);
    CHECK(navidrome_local_path("/music", "/srv/m", "", out, sizeof out) == -1);
    CHECK(navidrome_local_path("/music", "/srv/m", NULL, out, sizeof out) == -1);
    /* It fits exactly, or not at all. */
    CHECK(navidrome_local_path("/music", "/srv/m", repeat('a', sizeof out - 8), out,
                               sizeof out) == 0);
    CHECK(navidrome_local_path("/music", "/srv/m", repeat('a', sizeof out - 7), out,
                               sizeof out) == -1);
}

static void test_configure(void)
{
    CHECK(music_configure(NULL, "/tmp") == 0);
    CHECK(navidrome_configure("http://127.0.0.1:4533", NULL) == -1); /* needs NYLM_MUSIC */
    CHECK(navidrome_url() == NULL);
    CHECK(music_configure("/srv/music/", "/tmp") == 0);
    CHECK(navidrome_configure(NULL, NULL) == 0);
    CHECK(navidrome_url() == NULL);
    CHECK(navidrome_configure("", "") == 0);
    CHECK(navidrome_url() == NULL);
    CHECK(navidrome_configure("http://127.0.0.1:4533/nd//", NULL) == -1);
    CHECK(navidrome_configure("http://127.0.0.1:4533/nd/", NULL) == 0);
    CHECK_STR(navidrome_url(), "http://127.0.0.1:4533/nd");
    CHECK_STR(navidrome_music(), "/srv/music");
    CHECK(navidrome_configure("http://127.0.0.1:4533", "/music/") == 0);
    CHECK_STR(navidrome_url(), "http://127.0.0.1:4533");
    CHECK_STR(navidrome_music(), "/music");
    CHECK(navidrome_configure("http://127.0.0.1:4533", "music") == -1);
    CHECK(navidrome_url() == NULL);
    CHECK(navidrome_configure("http://127.0.0.1:4533", "/") == -1);
    CHECK(navidrome_configure("http://127.0.0.1:4533", "/a/../b") == -1);
    CHECK(navidrome_configure("http://10.0.0.1:4533", NULL) == -1);
    CHECK(navidrome_url() == NULL);

    /* Nothing answers at port 1: why, without the token or the password. */
    char err[300];
    int refused = -1;
    CHECK(navidrome_configure("http://127.0.0.1:1", NULL) == 0);
    CHECK(navidrome_call("ping", "", "admin", "secret", &refused, err, sizeof err) == NULL);
    CHECK(refused == 0);
    CHECK(strncmp(err, "Navidrome does not answer at http://127.0.0.1:1: ", 49) == 0);
    CHECK(strstr(err, "secret") == NULL && strstr(err, "t=") == NULL);
    CHECK(navidrome_check_login("admin", "", &refused, err, sizeof err) == -1);
    CHECK(refused == 1);
    CHECK(navidrome_configure(NULL, NULL) == 0);
    CHECK(navidrome_call("ping", "", "admin", "secret", &refused, err, sizeof err) == NULL);
    CHECK_STR(err, "Navidrome is not set up (NYLM_NAVIDROME)");
}

int main(void)
{
    if (arena_init(1 << 20) != 0)
        return 1;
    json_init();
    test_local_split();
    test_url_valid();
    test_names();
    test_call_url();
    test_answer();
    test_local_path();
    test_configure();
    TEST_DONE();
}
