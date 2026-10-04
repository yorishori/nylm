/* Qobuz without the network (src/qobuz.c) and the HTTPS client's parsers
 * (src/https.c). */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <unistd.h>

#include "../src/arena.h"
#include "../src/art.h"
#include "../src/https.h"
#include "../src/qobuz.h"
#include "test.h"

static const char *album_id(const char *url)
{
    static char id[QOBUZ_MAX_ID + 1];
    return qobuz_album_id(url, id) == 0 ? id : NULL;
}

static void test_album_links(void)
{
    CHECK_STR(album_id("https://www.qobuz.com/us-en/album/the-wall-pink-floyd/0886445635850"),
              "0886445635850");
    CHECK_STR(album_id("https://www.qobuz.com/gb-en/album/x/abc123"), "abc123");
    CHECK_STR(album_id("https://www.qobuz.com/album/name_2-x/abc"), "abc");
    CHECK_STR(album_id("https://open.qobuz.com/album/zz9"), "zz9");
    CHECK_STR(album_id("https://play.qobuz.com/album/zz9/"), "zz9");
    CHECK_STR(album_id("http://play.qobuz.com/album/zz9?utm=1"), "zz9");
    CHECK_STR(album_id("https://play.qobuz.com/album/zz9#x"), "zz9");
    char max[128];
    snprintf(max, sizeof max, "https://open.qobuz.com/album/%0*d", QOBUZ_MAX_ID, 7);
    CHECK(album_id(max) != NULL);
    snprintf(max, sizeof max, "https://open.qobuz.com/album/%0*d", QOBUZ_MAX_ID + 1, 7);
    CHECK(album_id(max) == NULL);
    CHECK(album_id(NULL) == NULL);
    CHECK(album_id("") == NULL);
    CHECK(album_id("ftp://www.qobuz.com/album/abc") == NULL);
    CHECK(album_id("https://evil.com/album/abc") == NULL);
    CHECK(album_id("https://www.qobuz.com.evil.com/album/abc") == NULL);
    CHECK(album_id("https://www.qobuz.com/us-en/track/abc") == NULL);
    CHECK(album_id("https://www.qobuz.com/us-en/artist/x/abc") == NULL);
    CHECK(album_id("https://www.qobuz.com/album/") == NULL);
    CHECK(album_id("https://www.qobuz.com/album//abc") == NULL);
    CHECK(album_id("https://www.qobuz.com/album/a/b/c") == NULL);
    CHECK(album_id("https://www.qobuz.com/album/x/ab-c") == NULL);
    CHECK(album_id("https://www.qobuz.com/album/x y/abc") == NULL);
}

static void test_redirect(void)
{
    char code[QOBUZ_MAX_TOKEN + 1], token[QOBUZ_MAX_TOKEN + 1], user[QOBUZ_MAX_TOKEN + 1];
    CHECK(qobuz_redirect_parse("http://localhost/?code_autorisation=abc%2Bd", code, token, user) == 0);
    CHECK_STR(code, "abc+d");
    CHECK_STR(token, "");
    CHECK(qobuz_redirect_parse("http://localhost/?x=1&code=c1#frag", code, token, user) == 0);
    CHECK_STR(code, "c1");
    CHECK(qobuz_redirect_parse("http://localhost/?user_auth_token=t1&user_id=42", code, token,
                               user) == 0);
    CHECK_STR(code, "");
    CHECK_STR(token, "t1");
    CHECK_STR(user, "42");
    CHECK(qobuz_redirect_parse("http://localhost/?token=t2", code, token, user) == 0);
    CHECK_STR(token, "t2");
    /* A bare code. */
    CHECK(qobuz_redirect_parse("abcdef", code, token, user) == 0);
    CHECK_STR(code, "abcdef");
    /* Rejected: nothing usable, an address without a query, bad escapes,
     * spaces, control characters, too long. */
    CHECK(qobuz_redirect_parse("http://localhost/?x=1", code, token, user) == -1);
    CHECK(qobuz_redirect_parse("http://localhost/", code, token, user) == -1);
    CHECK(qobuz_redirect_parse("http://localhost/?code=%zz", code, token, user) == -1);
    CHECK(qobuz_redirect_parse("http://localhost/?code=%4", code, token, user) == -1);
    CHECK(qobuz_redirect_parse("http://localhost/?code=%00", code, token, user) == -1);
    CHECK(qobuz_redirect_parse("a b", code, token, user) == -1);
    CHECK(qobuz_redirect_parse("a\tb", code, token, user) == -1);
    CHECK(qobuz_redirect_parse("", code, token, user) == -1);
    static char longest[QOBUZ_MAX_REDIRECT + 2];
    memset(longest, 'a', QOBUZ_MAX_REDIRECT);
    CHECK(qobuz_redirect_valid(longest));
    longest[QOBUZ_MAX_REDIRECT] = 'a';
    CHECK(!qobuz_redirect_valid(longest));
    CHECK(!qobuz_redirect_valid(NULL));
    CHECK(!qobuz_redirect_valid("caf\xc3\xa9"));
}

/* A bundle with what the real one has, and decoys. */
static const char bundle[] =
    "var x=1;production:{api:{appId:\"12345\",appSecret:\"x\"}}"
    "production:{api:{appId:\"798273057\",appSecret:\"05a4851e74ee47fda346f50cfdfc4f09\"}"
    "privateKey: \"6lz8C03UDIC7\","
    "X.initialSeed(\"QUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFB\",window.utimezone.paris)"
    "c.initialSeed(\"ZjY5YTc3MzQ2ODZjYjk0Mjc2MjkzNz\",window.utimezone.london)"
    "c.initialSeed(\"c2hvcnQ\",window.utimezone.berlin)"
    "name:\"Europe/London\",info:\"hhNGI3YWMzODE\",extras:"
    "\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\""
    "name:\"Europe/Berlin\",info:\"a\",extras:\"b\"";

static void test_bundle(void)
{
    char path[128];
    CHECK(qobuz_bundle_path("<html><script src=\"/resources/8.2.0-b034/bundle.js\"></script>",
                            path, sizeof path) == 0);
    CHECK_STR(path, "/resources/8.2.0-b034/bundle.js");
    CHECK(qobuz_bundle_path("<script src=\"/resources/8.2.0-b034/bundle.js\"", path, 32) == 0);
    CHECK(qobuz_bundle_path("<script src=\"/resources/8.2.0-b034/bundle.js\"", path, 31) == -1);
    CHECK(qobuz_bundle_path("<script src=\"/resources/8.2-b034/bundle.js\"", path, sizeof path) == -1);
    CHECK(qobuz_bundle_path("<script src=\"/resources/8.2.0-B034/bundle.js\"", path, sizeof path) == -1);
    CHECK(qobuz_bundle_path("<script src=\"/resources/8.2.0-b034/other.js\"", path, sizeof path) == -1);
    CHECK(qobuz_bundle_path("", path, sizeof path) == -1);

    char app[QOBUZ_APP_ID_LEN + 1];
    CHECK(qobuz_app_id(bundle, app) == 0);
    CHECK_STR(app, "798273057");
    CHECK(qobuz_app_id("production:{api:{appId:\"12345\"", app) == -1);
    CHECK(qobuz_app_id("", app) == -1);

    char key[QOBUZ_MAX_TOKEN + 1];
    qobuz_private_key(bundle, key);
    CHECK_STR(key, "6lz8C03UDIC7");
    qobuz_private_key("oauthKey:\"abcdefg\"", key);
    CHECK_STR(key, "abcdefg");
    qobuz_private_key("privateKey:\"abc\"", key); /* too short */
    CHECK_STR(key, "");

    /* london: seed + info + extras, less 44, is the base64 of the secret;
     * berlin is too short; X.initialSeed is not a match. */
    char secrets[QOBUZ_MAX_SECRETS][QOBUZ_MAX_SECRET + 1];
    CHECK(qobuz_secrets(bundle, secrets, QOBUZ_MAX_SECRETS) == 1);
    CHECK_STR(secrets[0], "f69a7734686cb9427629378a4b7ac381");
    CHECK(qobuz_secrets(bundle, secrets, 0) == 0);
    CHECK(qobuz_secrets("", secrets, QOBUZ_MAX_SECRETS) == 0);
}

static void test_signature(void)
{
    char sig[33];
    CHECK(qobuz_signature("5966783", 27, 1700000000, "abc", sig) == 0);
    CHECK_STR(sig, "321b160c45f67dcda41038e6e1b31524");

    char out[16];
    CHECK(qobuz_urlencode("a b&c=d/é", out, sizeof out) == -1);
    char big[64];
    CHECK(qobuz_urlencode("a b&c=d/é~-_.", big, sizeof big) == 0);
    CHECK_STR(big, "a%20b%26c%3Dd%2F%C3%A9~-_.");
    CHECK(qobuz_urlencode("abc", out, 4) == 0);
    CHECK(qobuz_urlencode("abc", out, 3) == -1);
    CHECK(qobuz_urlencode("", out, 1) == 0);
}

static const char *tag(const struct tags *t, enum tag_field f)
{
    return t->value[f].n == 1 ? t->value[f].v[0] : t->value[f].n == 0 ? "(none)" : "(many)";
}

static void test_tags(void)
{
    cJSON *album = cJSON_Parse(
        "{\"title\":\"Album\",\"version\":\"Deluxe\",\"artist\":{\"name\":\"Band\"},"
        "\"release_date_original\":\"2019-05-10\",\"media_count\":2,"
        "\"copyright\":\"(P) 2019 Label (C) 2019 Label\",\"label\":{\"name\":\"Label\"},"
        "\"upc\":\"0123\",\"tracks\":{\"items\":["
        "{\"id\":1,\"title\":\"One\",\"track_number\":1,\"media_number\":1},"
        "{\"id\":2,\"title\":\"Two\",\"track_number\":2,\"media_number\":1},"
        "{\"id\":3,\"title\":\"Three (Live)\",\"version\":\"live\",\"track_number\":1,"
        "\"media_number\":2,\"performer\":{\"name\":\"Guest\"},\"composer\":{\"name\":\"Comp\"},"
        "\"isrc\":\"XX\"}]}}");
    CHECK(album != NULL);
    const cJSON *items = cJSON_GetObjectItem(cJSON_GetObjectItem(album, "tracks"), "items");
    struct tags t;
    CHECK(qobuz_track_tags(album, cJSON_GetArrayItem(items, 2), &t) == NULL);
    CHECK_STR(tag(&t, TAG_TITLE), "Three (Live)");
    CHECK_STR(tag(&t, TAG_ALBUM), "Album (Deluxe)");
    CHECK_STR(tag(&t, TAG_ARTIST), "Guest");
    CHECK_STR(tag(&t, TAG_ALBUMARTIST), "Band");
    CHECK_STR(tag(&t, TAG_TRACKNUMBER), "1/1");
    CHECK_STR(tag(&t, TAG_DISCNUMBER), "2/2");
    CHECK_STR(tag(&t, TAG_DATE), "2019");
    CHECK_STR(tag(&t, TAG_COMPOSER), "Comp");
    CHECK_STR(tag(&t, TAG_COPYRIGHT), "\xe2\x84\x97 2019 Label \xc2\xa9 2019 Label");
    CHECK_STR(tag(&t, TAG_LABEL), "Label");
    CHECK_STR(tag(&t, TAG_ISRC), "XX");
    CHECK_STR(tag(&t, TAG_BARCODE), "0123");
    CHECK_STR(tag(&t, TAG_COMPILATION), "0");
    CHECK_STR(tag(&t, TAG_GENRE), "(none)");
    CHECK(qobuz_track_tags(album, cJSON_GetArrayItem(items, 0), &t) == NULL);
    CHECK_STR(tag(&t, TAG_TRACKNUMBER), "1/2");
    CHECK_STR(tag(&t, TAG_DISCNUMBER), "1/2");
    CHECK_STR(tag(&t, TAG_ARTIST), "Band");
    CHECK_STR(tag(&t, TAG_COMPOSER), "(none)");
    cJSON_Delete(album);

    /* What is missing. */
    cJSON *bare = cJSON_Parse("{\"title\":\"A\",\"artist\":{\"name\":\"B\"},"
                              "\"release_date_original\":\"0000-01-01\"}");
    cJSON *no_title = cJSON_Parse("{\"track_number\":1}");
    cJSON *no_number = cJSON_Parse("{\"title\":\"T\"}");
    cJSON *ok = cJSON_Parse("{\"title\":\"T\",\"track_number\":3}");
    CHECK_STR(qobuz_track_tags(bare, no_title, &t), "the track has no title");
    CHECK_STR(qobuz_track_tags(bare, no_number, &t), "the track has no number");
    CHECK_STR(qobuz_track_tags(no_number, ok, &t), "the album has no title or artist");
    CHECK(qobuz_track_tags(bare, ok, &t) == NULL);
    CHECK_STR(tag(&t, TAG_TRACKNUMBER), "3/3"); /* the album lists no tracks */
    CHECK_STR(tag(&t, TAG_DISCNUMBER), "1/1");
    CHECK_STR(tag(&t, TAG_DATE), "(none)");     /* year 0000 is not one */
    cJSON_Delete(bare);
    cJSON_Delete(no_title);
    cJSON_Delete(no_number);
    cJSON_Delete(ok);
}

/* Reads a whole file into a malloc()ed buffer. */
static unsigned char *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf = malloc(1 << 20);
    *size = f != NULL && buf != NULL ? fread(buf, 1, 1 << 20, f) : 0;
    if (f != NULL)
        fclose(f);
    return buf;
}

/* A downloaded file gets Qobuz's tags (no genre, its old ones gone), the
 * cover as its only picture, and its place in the library. */
static void test_tag_file(void)
{
    char path[] = "/tmp/nylm-test-qobuz-XXXXXX";
    int fd = mkstemp(path);
    size_t size, cover_size;
    unsigned char *audio = slurp("tests/data/tagged.flac", &size);
    unsigned char *cover = slurp("tests/data/cover.jpg", &cover_size);
    CHECK(fd >= 0 && size > 0 && cover_size > 0 && write(fd, audio, size) == (ssize_t)size);
    close(fd);
    char flac[64];
    snprintf(flac, sizeof flac, "%s.flac", path);
    CHECK(rename(path, flac) == 0);

    cJSON *album = cJSON_Parse(
        "{\"title\":\"AC/DC Live\",\"artist\":{\"name\":\"AC/DC\"},\"media_count\":1,"
        "\"release_date_original\":\"1992-10-27\",\"tracks\":{\"items\":["
        "{\"id\":1,\"title\":\"Thunderstruck\",\"track_number\":1},"
        "{\"id\":2,\"title\":\"Shoot\",\"track_number\":2}]}}");
    const cJSON *track = cJSON_GetArrayItem(
        cJSON_GetObjectItem(cJSON_GetObjectItem(album, "tracks"), "items"), 0);
    struct tag_picture pic = { .type = "Front Cover", .description = "", .data = cover,
                               .size = cover_size, .mime = "image/jpeg" };
    CHECK(art_hash(cover, cover_size, pic.hash) == 0);
    char rel[TAGS_MAX_PATH], err[256] = "";
    CHECK(qobuz_tag_file(flac, "flac", album, track, &pic, rel, sizeof rel, err, sizeof err) == 0);
    CHECK_STR(rel, "AC_DC/AC_DC Live/01 - Thunderstruck.flac");

    struct tags t;
    char rerr[256];
    CHECK(tags_read(flac, &t, NULL, NULL, rerr, sizeof rerr) == 0);
    CHECK_STR(tag(&t, TAG_TITLE), "Thunderstruck");
    CHECK_STR(tag(&t, TAG_ALBUMARTIST), "AC/DC");
    CHECK_STR(tag(&t, TAG_TRACKNUMBER), "1/2");
    CHECK_STR(tag(&t, TAG_DISCNUMBER), "1/1");
    CHECK_STR(tag(&t, TAG_DATE), "1992");
    CHECK_STR(tag(&t, TAG_TITLESORT), "Thunderstruck");
    CHECK_STR(tag(&t, TAG_GENRE), "(none)"); /* the file had Rock */
    CHECK(t.npictures == 1 && strcmp(t.pictures[0].hash, pic.hash) == 0 &&
          strcmp(t.pictures[0].type, "Front Cover") == 0);

    /* A file that is not audio is refused. */
    FILE *f = fopen(flac, "w");
    CHECK(f != NULL && fputs("not audio", f) >= 0 && fclose(f) == 0);
    CHECK(qobuz_tag_file(flac, "flac", album, track, NULL, rel, sizeof rel, err, sizeof err) == -1);
    CHECK(strncmp(err, "the file can not be read", 24) == 0);
    unlink(flac);
    cJSON_Delete(album);
    free(audio);
    free(cover);
}

static void test_https_split(void)
{
    char host[HTTPS_MAX_HOST + 1];
    int port;
    const char *path;
    CHECK(https_split("https://www.qobuz.com/api.json/0.2/x?a=1", host, &port, &path) == 0);
    CHECK_STR(host, "www.qobuz.com");
    CHECK(port == 443);
    CHECK_STR(path, "/api.json/0.2/x?a=1");
    CHECK(https_split("https://a.b:8443", host, &port, &path) == 0);
    CHECK(port == 8443);
    CHECK_STR(path, "/");
    CHECK(https_split("http://a.b/", host, &port, &path) == -1);
    CHECK(https_split("https:///x", host, &port, &path) == -1);
    CHECK(https_split("https://a.b:0/", host, &port, &path) == -1);
    CHECK(https_split("https://a.b:65536/", host, &port, &path) == -1);
    CHECK(https_split("https://a.b:x/", host, &port, &path) == -1);
    CHECK(https_split("https://user@a.b/", host, &port, &path) == -1);
    CHECK(https_split("https://a.b/x y", host, &port, &path) == -1);
    CHECK(https_split("https://a.b/x\r\nHost: evil", host, &port, &path) == -1);
    CHECK(https_split("https://a.b?x", host, &port, &path) == -1);
    CHECK(https_split(NULL, host, &port, &path) == -1);
    char url[400];
    snprintf(url, sizeof url, "https://%0*d/", HTTPS_MAX_HOST, 0);
    CHECK(https_split(url, host, &port, &path) == 0);
    snprintf(url, sizeof url, "https://%0*d/", HTTPS_MAX_HOST + 1, 0);
    CHECK(https_split(url, host, &port, &path) == -1);
}

static long dechunk(const char *s, char *buf)
{
    size_t n = strlen(s);
    memcpy(buf, s, n + 1);
    return https_dechunk(buf, n);
}

static void test_dechunk(void)
{
    char buf[256];
    CHECK(dechunk("5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n", buf) == 11);
    CHECK_STR(buf, "hello world");
    CHECK(dechunk("A;ext=1\r\n0123456789\r\n0\r\nX-Trailer: 1\r\n\r\n", buf) == 10);
    CHECK_STR(buf, "0123456789");
    CHECK(dechunk("0\r\n\r\n", buf) == 0);
    CHECK(dechunk("", buf) == -1);
    CHECK(dechunk("5\r\nhello\r\n", buf) == -1);              /* no last chunk */
    CHECK(dechunk("5\r\nhell\r\n0\r\n\r\n", buf) == -1);      /* short */
    CHECK(dechunk("5\r\nhello!\r\n0\r\n\r\n", buf) == -1);    /* long */
    CHECK(dechunk("x\r\nhello\r\n0\r\n\r\n", buf) == -1);     /* not hex */
    CHECK(dechunk("0\r\n\r\nextra", buf) == -1);
    CHECK(dechunk("0\r\n", buf) == -1);
    CHECK(dechunk("1000000000000000\r\n", buf) == -1);       /* 16 digits */
}

int main(void)
{
    if (arena_init(1 << 20) != 0)
        return 1;
    test_album_links();
    test_redirect();
    test_bundle();
    test_signature();
    test_tags();
    test_tag_file();
    test_https_split();
    test_dechunk();
    TEST_DONE();
}
