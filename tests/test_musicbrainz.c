/* The MusicBrainz service without the network: ids, choosing genres from
 * MusicBrainz's answers, which albums are looked up, queueing and
 * recording. */
#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <unistd.h>

#include "../src/arena.h"
#include "../src/db.h"
#include "../src/json.h"
#include "../src/musicbrainz.h"
#include "../src/tags.h"
#include "test.h"

#define ID1 "f2c9c0f4-9c9b-4a7b-8f3a-1234567890ab"
#define ID2 "00000000-0000-0000-0000-000000000002"

static char dir[] = "/tmp/nylm-mb-XXXXXX";

static void test_ids(void)
{
    CHECK(mb_id_valid(ID1));
    CHECK(mb_id_valid("0123abcd-ef01-2345-6789-abcdef012345"));
    CHECK(!mb_id_valid("F2C9C0F4-9C9B-4A7B-8F3A-1234567890AB")); /* uppercase */
    CHECK(!mb_id_valid("f2c9c0f4-9c9b-4a7b-8f3a-1234567890a"));  /* max - 1 */
    CHECK(!mb_id_valid("f2c9c0f4-9c9b-4a7b-8f3a-1234567890abc")); /* max + 1 */
    CHECK(!mb_id_valid("f2c9c0f49-c9b-4a7b-8f3a-1234567890ab"));  /* a dash moved */
    CHECK(!mb_id_valid("f2c9c0f4x9c9b-4a7b-8f3a-1234567890ab"));
    CHECK(!mb_id_valid("g2c9c0f4-9c9b-4a7b-8f3a-1234567890ab"));  /* not hex */
    CHECK(!mb_id_valid("123456"));                                /* a Deezer id */
    CHECK(!mb_id_valid(""));
}

/* The genres chosen from json, joined by "|" ("" for none). */
static const char *chosen(const char *json)
{
    static char out[512];
    struct mb_genres g;
    cJSON *obj = cJSON_Parse(json);
    size_t n = obj != NULL ? mb_genres(obj, &g) : 99;
    out[0] = '\0';
    for (size_t i = 0; n != 99 && i < g.n; i++) {
        size_t len = strlen(out);
        snprintf(out + len, sizeof out - len, "%s%s", i > 0 ? "|" : "", g.v[i]);
    }
    return n == g.n ? out : "count differs";
}

static void test_genres(void)
{
    /* the most votes first; between equal votes MusicBrainz's order */
    CHECK_STR(chosen("{\"genres\":[{\"name\":\"pop\",\"count\":1},{\"name\":\"rock\",\"count\":5},"
                     "{\"name\":\"jazz\",\"count\":2},{\"name\":\"soul\",\"count\":2}]}"),
              "rock|jazz|soul|pop");
    CHECK_STR(chosen("{\"genres\":[{\"name\":\"a\",\"count\":1},{\"name\":\"b\",\"count\":1}]}"),
              "a|b");
    /* MB_GENRES at most */
    CHECK_STR(chosen("{\"genres\":[{\"name\":\"a\",\"count\":1},{\"name\":\"b\",\"count\":2},"
                     "{\"name\":\"c\",\"count\":3},{\"name\":\"d\",\"count\":4},"
                     "{\"name\":\"e\",\"count\":5},{\"name\":\"f\",\"count\":6}]}"),
              "f|e|d|c|b");
    /* names the genre rule allows: spaces, & and / */
    CHECK_STR(chosen("{\"genres\":[{\"name\":\"pop rock\",\"count\":3},"
                     "{\"name\":\"r&b\",\"count\":2},{\"name\":\"rock/pop\",\"count\":1}]}"),
              "pop rock|r&b|rock/pop");
    /* names that break it are left out, and the next one taken */
    CHECK_STR(chosen("{\"genres\":[{\"name\":\"children's music\",\"count\":9},"
                     "{\"name\":\"Rock\",\"count\":8},{\"name\":\"música\",\"count\":7},"
                     "{\"name\":\"\",\"count\":6},{\"name\":\"pop  rock\",\"count\":5},"
                     "{\"name\":\"jazz\",\"count\":1}]}"),
              "jazz");
    /* no votes, or not a number, or not a name */
    CHECK_STR(chosen("{\"genres\":[{\"name\":\"a\",\"count\":0},{\"name\":\"b\",\"count\":\"3\"},"
                     "{\"name\":3,\"count\":3},{\"count\":3},{\"name\":\"c\"},1,\"d\"]}"),
              "");
    /* a name given twice is taken once */
    CHECK_STR(chosen("{\"genres\":[{\"name\":\"a\",\"count\":3},{\"name\":\"a\",\"count\":2},"
                     "{\"name\":\"b\",\"count\":1}]}"),
              "a|b");
    /* no genres */
    CHECK_STR(chosen("{\"genres\":[]}"), "");
    CHECK_STR(chosen("{}"), "");
    CHECK_STR(chosen("{\"genres\":{}}"), "");
    CHECK_STR(chosen("[]"), "");
    /* MB_MAX_GENRE bytes, and one more */
    char json[512], name[MB_MAX_GENRE + 2];
    memset(name, 'a', MB_MAX_GENRE);
    name[MB_MAX_GENRE] = '\0';
    snprintf(json, sizeof json, "{\"genres\":[{\"name\":\"%s\",\"count\":1}]}", name);
    CHECK_STR(chosen(json), name);
    name[MB_MAX_GENRE] = 'a';
    name[MB_MAX_GENRE + 1] = '\0';
    snprintf(json, sizeof json, "{\"genres\":[{\"name\":\"%s\",\"count\":1}]}", name);
    CHECK_STR(chosen(json), "");
    /* only the first 200 are read */
    static char many[16384];
    size_t len = (size_t)snprintf(many, sizeof many, "{\"genres\":[");
    for (int i = 0; i < 200; i++)
        len += (size_t)snprintf(many + len, sizeof many - len, "{\"name\":\"Bad\",\"count\":9},");
    snprintf(many + len, sizeof many - len, "{\"name\":\"late\",\"count\":9}]}");
    CHECK_STR(chosen(many), "");
}

/* The language of release json as a genre, then its code: "genre code",
 * "-" for NULL. */
static const char *language(const char *json)
{
    static char out[128];
    const char *code = "x";
    cJSON *obj = cJSON_Parse(json);
    const char *name = obj != NULL ? mb_language(obj, &code) : "bad json";
    snprintf(out, sizeof out, "%s %s", name != NULL ? name : "-", code != NULL ? code : "-");
    cJSON_Delete(obj);
    return out;
}

static void test_language(void)
{
    CHECK_STR(language("{\"text-representation\":{\"language\":\"spa\"}}"), "spanish spa");
    CHECK_STR(language("{\"text-representation\":{\"language\":\"zxx\"}}"),
              "instrumental zxx");
    CHECK_STR(language("{\"text-representation\":{\"language\":\"afr\"}}"), "afrikaans afr");
    CHECK_STR(language("{\"text-representation\":{\"language\":\"zho\"}}"), "chinese zho");
    CHECK_STR(language("{\"text-representation\":{\"language\":\"mul\"}}"), "- mul");
    CHECK_STR(language("{\"text-representation\":{\"language\":\"xyz\"}}"), "- xyz");
    /* not a code: 2 or 4 letters, uppercase, not a string, null, absent */
    CHECK_STR(language("{\"text-representation\":{\"language\":\"sp\"}}"), "- -");
    CHECK_STR(language("{\"text-representation\":{\"language\":\"span\"}}"), "- -");
    CHECK_STR(language("{\"text-representation\":{\"language\":\"SPA\"}}"), "- -");
    CHECK_STR(language("{\"text-representation\":{\"language\":\"\"}}"), "- -");
    CHECK_STR(language("{\"text-representation\":{\"language\":3}}"), "- -");
    CHECK_STR(language("{\"text-representation\":{\"language\":null}}"), "- -");
    CHECK_STR(language("{\"text-representation\":{}}"), "- -");
    CHECK_STR(language("{\"text-representation\":\"spa\"}"), "- -");
    CHECK_STR(language("{}"), "- -");
}

/* A track whose recording performs works with the languages given (a JSON
 * list of lists of codes). */
static void add_track(char *json, size_t size, const char *works)
{
    cJSON *langs = cJSON_Parse(works);
    size_t len = strlen(json);
    len += (size_t)snprintf(json + len, size - len, "%s{\"recording\":{\"relations\":[",
                            json[len - 1] == '[' ? "" : ",");
    const cJSON *w;
    int i = 0;
    cJSON_ArrayForEach(w, langs) {
        char *codes = cJSON_PrintUnformatted(w);
        len += (size_t)snprintf(json + len, size - len,
                                "%s{\"type\":\"performance\",\"work\":{\"languages\":%s}}",
                                i++ > 0 ? "," : "", codes);
    }
    snprintf(json + len, size - len, "]}}");
    cJSON_Delete(langs);
}

/* The lyrics' languages of a release whose tracks are tracks (each a
 * JSON list of works' language lists), joined by "|". */
static const char *sung(int ntracks, const char *const *tracks)
{
    static char json[65536], out[256];
    snprintf(json, sizeof json, "{\"media\":[{\"tracks\":[");
    for (int i = 0; i < ntracks; i++)
        add_track(json, sizeof json, tracks[i]);
    size_t len = strlen(json);
    snprintf(json + len, sizeof json - len, "]}]}");
    cJSON *obj = cJSON_Parse(json);
    const char *names[MB_LANGUAGES];
    size_t n = obj != NULL ? mb_lyrics_languages(obj, names) : 0;
    out[0] = '\0';
    for (size_t i = 0; i < n; i++) {
        len = strlen(out);
        snprintf(out + len, sizeof out - len, "%s%s", i > 0 ? "|" : "", names[i]);
    }
    cJSON_Delete(obj);
    return obj != NULL ? out : "bad json";
}

#define SUNG(...) sung(sizeof (const char *[]){ __VA_ARGS__ } / sizeof (const char *), \
                       (const char *[]){ __VA_ARGS__ })

static void test_lyrics_languages(void)
{
    /* Bonito Generation: any track counts, the most sung first */
    CHECK_STR(SUNG("[[\"eng\"]]", "[[\"eng\"]]", "[[\"eng\",\"jpn\"]]", "[[\"jpn\"]]"),
              "english|japanese");
    CHECK_STR(SUNG("[[\"jpn\"]]", "[[\"eng\"]]", "[[\"eng\"]]"), "english|japanese");
    /* equal counts: the first sung first */
    CHECK_STR(SUNG("[[\"spa\"]]", "[[\"eng\"]]"), "spanish|english");
    /* at most MB_LANGUAGES */
    CHECK_STR(SUNG("[[\"fra\"]]", "[[\"spa\",\"eng\"]]", "[[\"eng\",\"deu\"]]",
                   "[[\"eng\",\"spa\"]]"),
              "english|spanish|french");
    /* a track counts once for a language, however many of its works have it */
    CHECK_STR(SUNG("[[\"eng\"],[\"eng\"],[\"eng\"]]", "[[\"spa\"]]", "[[\"spa\"]]"),
              "spanish|english");
    /* instrumental only when nothing is sung */
    CHECK_STR(SUNG("[[\"zxx\"]]", "[[\"zxx\"]]"), "instrumental");
    CHECK_STR(SUNG("[[\"zxx\"]]", "[[\"zxx\"]]", "[[\"eng\"]]"), "english");
    /* unknown, several (mul), not a code: left out */
    CHECK_STR(SUNG("[[\"mul\"]]", "[[\"xyz\"]]", "[[\"ENG\"]]", "[[1]]", "[[\"\"]]"), "");
    CHECK_STR(SUNG("[[\"mul\",\"kor\"]]"), "korean");
    /* no works, or works without languages */
    CHECK_STR(SUNG("[]", "[[]]"), "");
    CHECK_STR(sung(0, NULL), "");
    /* only performance relations count */
    cJSON *r = cJSON_Parse("{\"media\":[{\"tracks\":[{\"recording\":{\"relations\":["
                           "{\"type\":\"samples material\",\"work\":{\"languages\":[\"eng\"]}},"
                           "{\"work\":{\"languages\":[\"spa\"]}},"
                           "{\"type\":\"performance\",\"work\":{\"languages\":[\"jpn\"]}}]}}]}]}");
    const char *names[MB_LANGUAGES];
    CHECK(mb_lyrics_languages(r, names) == 1 && strcmp(names[0], "japanese") == 0);
    cJSON_Delete(r);
    /* not shaped as expected */
    const char *odd[] = { "{}", "{\"media\":{}}", "{\"media\":[{}]}", "{\"media\":[{\"tracks\":[{}]}]}",
                          "{\"media\":[{\"tracks\":[{\"recording\":{\"relations\":[{\"type\":"
                          "\"performance\",\"work\":{\"languages\":\"eng\"}}]}}]}]}" };
    for (size_t i = 0; i < sizeof odd / sizeof odd[0]; i++) {
        r = cJSON_Parse(odd[i]);
        CHECK(r != NULL && mb_lyrics_languages(r, names) == 0);
        cJSON_Delete(r);
    }
    /* only the first MB_TRACKS tracks are read, across media */
    static char many[MB_TRACKS * 120];
    size_t len = (size_t)snprintf(many, sizeof many, "{\"media\":[{\"tracks\":[");
    for (int i = 0; i < MB_TRACKS; i++)
        len += (size_t)snprintf(many + len, sizeof many - len, "%s{}", i > 0 ? "," : "");
    snprintf(many + len, sizeof many - len, "]},{\"tracks\":[{\"recording\":{\"relations\":["
             "{\"type\":\"performance\",\"work\":{\"languages\":[\"eng\"]}}]}}]}]}");
    r = cJSON_Parse(many);
    CHECK(r != NULL && mb_lyrics_languages(r, names) == 0);
    cJSON_Delete(r);
}

/* Adding genres: no name twice, and no more than MB_ALL_GENRES. */
static void test_add_genre(void)
{
    struct mb_genres g = { .n = 0 };
    mb_add_genre(&g, "rock");
    mb_add_genre(&g, "rock");
    CHECK(g.n == 1);
    const char *names[] = { "a", "b", "c", "d", "e", "f", "g", "h", "i" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        mb_add_genre(&g, names[i]);
    CHECK(g.n == MB_ALL_GENRES);
    CHECK_STR(g.v[0], "rock");
    CHECK_STR(g.v[MB_ALL_GENRES - 1], "g");
}

/* The language after the voted genres: left out when "instrumental" is
 * among them. */
static void test_add_language(void)
{
    struct mb_genres g = { .n = 0 };
    CHECK(mb_add_language(&g, "english") == 1);                /* no genres */
    CHECK(g.n == 1 && strcmp(g.v[0], "english") == 0);
    g.n = 0;
    mb_add_genre(&g, "ambient");
    mb_add_genre(&g, "instrumental");
    CHECK(mb_add_language(&g, "english") == 0);
    CHECK(g.n == 2);
    CHECK(mb_add_language(&g, "instrumental") == 0);           /* zxx, already there */
    CHECK(g.n == 2);
    g.n = 0;
    mb_add_genre(&g, "instrumental hip hop");                  /* not the same genre */
    CHECK(mb_add_language(&g, "english") == 1);
    CHECK(g.n == 2 && strcmp(g.v[1], "english") == 0);
    g.n = 0;
    mb_add_genre(&g, "ambient");
    CHECK(mb_add_language(&g, "instrumental") == 1);           /* zxx */
    CHECK(g.n == 2 && strcmp(g.v[1], "instrumental") == 0);
}

static void test_release_group(void)
{
    cJSON *r = cJSON_Parse("{\"release-group\":{\"id\":\"" ID1 "\"}}");
    CHECK_STR(mb_release_group(r), ID1);
    CHECK(mb_release_group(cJSON_Parse("{\"release-group\":{\"id\":\"x\"}}")) == NULL);
    CHECK(mb_release_group(cJSON_Parse("{\"release-group\":{\"id\":1}}")) == NULL);
    CHECK(mb_release_group(cJSON_Parse("{\"release-group\":\"" ID1 "\"}")) == NULL);
    CHECK(mb_release_group(cJSON_Parse("{}")) == NULL);
}

/* name normalised, or "error". */
static const char *normal(const char *name, size_t size)
{
    static char out[MB_MAX_NAME];
    return mb_normalize(name, out, size) == 0 ? out : "error";
}

static void test_normalize(void)
{
    CHECK_STR(normal("OK Computer", MB_MAX_NAME), "ok computer");
    CHECK_STR(normal("The Pixies", MB_MAX_NAME), "pixies");
    CHECK_STR(normal("the  PIXIES ", MB_MAX_NAME), "pixies");
    CHECK_STR(normal("The", MB_MAX_NAME), "the");               /* nothing after it */
    CHECK_STR(normal("Theatre", MB_MAX_NAME), "theatre");
    CHECK_STR(normal("Them, The", MB_MAX_NAME), "them the");
    /* ASCII punctuation out, white space one space, trimmed */
    CHECK_STR(normal("  Simon & Garfunkel!\t", MB_MAX_NAME), "simon garfunkel");
    CHECK_STR(normal("AC/DC", MB_MAX_NAME), "acdc");
    CHECK_STR(normal("Mr. Bungle - (Live)", MB_MAX_NAME), "mr bungle live");
    /* typographic quotes, dashes and the ellipsis out */
    CHECK_STR(normal("Don\u2019t \u201cStop\u201d \u2013 Now\u2026", MB_MAX_NAME),
              "dont stop now");
    CHECK_STR(normal("Don't Stop", MB_MAX_NAME), "dont stop");
    /* À..Þ lowercase too, but × (between them) and the rest of UTF-8 kept */
    CHECK_STR(normal("ROSAL\u00cdA", MB_MAX_NAME), "rosal\u00eda");
    CHECK_STR(normal("\u00c0\u00c9\u00d1\u00d6\u00d8\u00de", MB_MAX_NAME),
              "\u00e0\u00e9\u00f1\u00f6\u00f8\u00fe");
    CHECK_STR(normal("\u00d7 \u00df \u00e9 \u0100 \u2022", MB_MAX_NAME),
              "\u00d7 \u00df \u00e9 \u0100 \u2022");
    CHECK_STR(normal("a\u00c3", MB_MAX_NAME), "a\u00e3");               /* c3 83 */
    CHECK_STR(normal("\xc3", MB_MAX_NAME), "\xc3");                    /* cut short */
    CHECK_STR(normal("\u00c9", 3), "\u00e9");                          /* 2 bytes + NUL */
    CHECK_STR(normal("\u00c9", 2), "error");
    /* empty, only punctuation */
    CHECK_STR(normal("", MB_MAX_NAME), "");
    CHECK_STR(normal(" .-! ", MB_MAX_NAME), "");
    /* the room: "abc" needs 4 bytes */
    CHECK_STR(normal("abc", 4), "abc");
    CHECK_STR(normal("abc", 3), "error");
    CHECK_STR(normal("a b", 3), "error");
    CHECK_STR(normal("a!!", 2), "a");
}

/* The search path for album by artist, or "error". */
static const char *search_path(const char *album, const char *artist, size_t size)
{
    static char out[MB_MAX_QUERY];
    return mb_search_path(album, artist, out, size) == 0 ? out : "error";
}

static void test_search_path(void)
{
    CHECK_STR(search_path("OK Computer", "Radiohead", MB_MAX_QUERY),
              "release/?query=release%3A%22OK%20Computer%22%20AND%20artist%3A%22Radiohead%22"
              "&limit=25&fmt=json");
    /* quotes and backslashes escaped, & and ? encoded */
    CHECK_STR(search_path("a\"b\\c", "x&y?", MB_MAX_QUERY),
              "release/?query=release%3A%22a%5C%22b%5C%5Cc%22%20AND%20artist%3A%22x%26y%3F%22"
              "&limit=25&fmt=json");
    CHECK_STR(search_path("", "", MB_MAX_QUERY),
              "release/?query=release%3A%22%22%20AND%20artist%3A%22%22&limit=25&fmt=json");
    /* the longest names a tag holds fit, even all quotes */
    char longest[TAGS_MAX_VALUE + 1];
    memset(longest, '"', TAGS_MAX_VALUE);
    longest[TAGS_MAX_VALUE] = '\0';
    CHECK(strcmp(search_path(longest, longest, MB_MAX_QUERY), "error") != 0);
    /* out of room */
    const char *whole = search_path("a", "b", MB_MAX_QUERY);
    size_t len = strlen(whole);
    CHECK(strcmp(search_path("a", "b", len + 1), "error") != 0);
    CHECK_STR(search_path("a", "b", len), "error");
}

/* The release picked from search for album by artist, or "-". */
static const char *picked(const char *json, const char *album, const char *artist)
{
    static char out[MB_ID_LEN + 1];
    cJSON *obj = cJSON_Parse(json);
    const char *id = obj != NULL ? mb_pick_release(obj, album, artist) : "bad json";
    snprintf(out, sizeof out, "%s", id != NULL ? id : "-");
    cJSON_Delete(obj);
    return out;
}

#define REL(id, title, status, credit) \
    "{\"id\":\"" id "\",\"title\":\"" title "\",\"status\":\"" status "\",\"artist-credit\":" credit "}"
#define BY(name) "[{\"name\":\"" name "\"}]"

static void test_pick(void)
{
    /* the same names, normalised */
    CHECK_STR(picked("{\"releases\":[" REL(ID1, "OK COMPUTER", "Official", BY("Radiohead")) "]}",
                     "OK Computer", "Radiohead"), ID1);
    CHECK_STR(picked("{\"releases\":[" REL(ID1, "Doolittle", "Official", BY("Pixies")) "]}",
                     "Doolittle", "The Pixies"), ID1);
    /* an Official one before an earlier other */
    CHECK_STR(picked("{\"releases\":[" REL(ID2, "A", "Bootleg", BY("B")) ","
                     REL(ID1, "A", "Official", BY("B")) "]}", "A", "B"), ID1);
    /* else the first with the same names */
    CHECK_STR(picked("{\"releases\":[" REL(ID2, "A", "Promotion", BY("B")) ","
                     REL(ID1, "A", "Bootleg", BY("B")) "]}", "A", "B"), ID2);
    /* names that differ: never taken, even a single result */
    CHECK_STR(picked("{\"releases\":[" REL(ID1, "A (Deluxe)", "Official", BY("B")) "]}", "A", "B"),
              "-");
    CHECK_STR(picked("{\"releases\":[" REL(ID1, "A", "Official", BY("C")) "]}", "A", "B"), "-");
    /* the whole credit: names and join phrases */
    CHECK_STR(picked("{\"releases\":[" REL(ID1, "A", "Official",
                     "[{\"name\":\"Simon\",\"joinphrase\":\" & \"},{\"name\":\"Garfunkel\"}]")
                     "]}", "A", "Simon & Garfunkel"), ID1);
    CHECK_STR(picked("{\"releases\":[" REL(ID1, "A", "Official",
                     "[{\"name\":\"Simon\",\"joinphrase\":\" & \"},{\"name\":\"Garfunkel\"}]")
                     "]}", "A", "Simon"), "-");
    /* not an id, no title, no credit, a credit without a name */
    CHECK_STR(picked("{\"releases\":[" REL("123456", "A", "Official", BY("B")) "]}", "A", "B"), "-");
    CHECK_STR(picked("{\"releases\":[{\"id\":\"" ID1 "\",\"artist-credit\":" BY("B") "}]}",
                     "A", "B"), "-");
    CHECK_STR(picked("{\"releases\":[{\"id\":\"" ID1 "\",\"title\":\"A\"}]}", "A", "B"), "-");
    CHECK_STR(picked("{\"releases\":[" REL(ID1, "A", "Official", "[]") "]}", "A", "B"), "-");
    CHECK_STR(picked("{\"releases\":[" REL(ID1, "A", "Official", "[{\"name\":1}]") "]}", "A", "B"),
              "-");
    /* no status is not Official, but may be the first */
    CHECK_STR(picked("{\"releases\":[{\"id\":\"" ID1 "\",\"title\":\"A\",\"artist-credit\":"
                     BY("B") "}]}", "A", "B"), ID1);
    /* nothing found */
    CHECK_STR(picked("{\"releases\":[]}", "A", "B"), "-");
    CHECK_STR(picked("{}", "A", "B"), "-");
    CHECK_STR(picked("{\"releases\":{}}", "A", "B"), "-");
    /* only the first 25 are read */
    static char many[8192];
    size_t len = (size_t)snprintf(many, sizeof many, "{\"releases\":[");
    for (int i = 0; i < 25; i++)
        len += (size_t)snprintf(many + len, sizeof many - len, REL(ID2, "X", "Official", BY("B")) ",");
    snprintf(many + len, sizeof many - len, REL(ID1, "A", "Official", BY("B")) "]}");
    CHECK_STR(picked(many, "A", "B"), "-");
}

/* Runs sql on the music database; 0 or -1. */
static int run(const char *sql)
{
    return sqlite3_exec(music_db, sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

/* A track of album by artist (NULL: none) with mbid (NULL: none). Its id. */
static long long track_by(const char *album, const char *artist, const char *mbid)
{
    static int next;
    char path[64];
    snprintf(path, sizeof path, "/m/%d.flac", ++next);
    sqlite3_stmt *st = db_prepare(music_db,
        "INSERT INTO tracks (path, size, ext, scanned, album, albumartist, musicbrainz_albumid)"
        " VALUES (?, 1, 'flac', 1, ?, ?, ?)");
    long long id = st != NULL && sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                           sqlite3_bind_text(st, 2, album, -1, SQLITE_STATIC) == SQLITE_OK &&
                           sqlite3_bind_text(st, 3, artist, -1, SQLITE_STATIC) == SQLITE_OK &&
                           sqlite3_bind_text(st, 4, mbid, -1, SQLITE_STATIC) == SQLITE_OK &&
                           sqlite3_step(st) == SQLITE_DONE
                       ? sqlite3_last_insert_rowid(music_db)
                       : -1;
    sqlite3_finalize(st);
    return id;
}

/* A track of album by "A" with mbid (NULL: none). Its id. */
static long long track(const char *album, const char *mbid)
{
    return track_by(album, "A", mbid);
}

/* Runs sql with %lld replaced by id. */
static int run_id(const char *fmt, long long id)
{
    char sql[512];
    snprintf(sql, sizeof sql, fmt, id);
    return run(sql);
}

/* The albums mb_next_albums(max) gives, as "album:tracks" joined by " ". */
static const char *next_albums(int max)
{
    static char out[512];
    struct mb_album a[MB_ALBUMS];
    int n = mb_next_albums(a, max);
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        size_t len = strlen(out);
        snprintf(out + len, sizeof out - len, "%s%s:%zu", i > 0 ? " " : "", a[i].album,
                 a[i].ntracks);
    }
    return n < 0 ? "error" : out;
}

/* The pending genre of track id, or "-". */
static const char *pending_genre(long long id)
{
    static char out[256];
    sqlite3_stmt *st = db_prepare(music_db,
        "SELECT value FROM changes WHERE state = 'pending' AND field = 'genre' AND track_id = ?");
    snprintf(out, sizeof out, "-");
    if (st != NULL && sqlite3_bind_int64(st, 1, id) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        snprintf(out, sizeof out, "%s", (const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return out;
}

static void test_albums(void)
{
    char path[64];
    snprintf(path, sizeof path, "%s/music.db", dir);
    music_db = db_open(path, music_migrations, music_migration_count);
    CHECK(music_db != NULL);
    CHECK_STR(next_albums(MB_ALBUMS), "");                   /* empty library */

    long long a1 = track("a1", ID1), a1b = track("a1", ID1); /* looked up */
    track("a2", ID1);                                        /* ids differ */
    track("a2", ID2);
    track("a3", NULL);                                       /* no id */
    long long a4 = track("a4", ID2), a4b = track("a4", ID2); /* one has a genre */
    track("a5", "F2C9C0F4-9C9B-4A7B-8F3A-1234567890AB");     /* not an id */
    track("a6", "00000000-0000-0000-0000-000000000006");     /* found before */
    track("a7", "00000000-0000-0000-0000-000000000007");     /* failed before */
    long long a8 = track("a8", "00000000-0000-0000-0000-000000000008"); /* genre removed */
    long long a9 = track("a9", "00000000-0000-0000-0000-000000000009"); /* genre planned */
    long long a10 = track("a10", NULL);                      /* id planned */
    long long a11 = track("a11", "00000000-0000-0000-0000-000000000011"); /* id removed */
    CHECK(run_id("INSERT INTO track_values VALUES (%lld, 'genre', 0, 'rock')", a4b) == 0);
    CHECK(run("INSERT INTO musicbrainz_lookups (mbid, track, state) VALUES"
              " ('00000000-0000-0000-0000-000000000006', 1, 'none'),"
              " ('00000000-0000-0000-0000-000000000007', 1, 'failed'),"
              " ('00000000-0000-0000-0000-000000000007', 1, 'skipped')") == 0);
    CHECK(run_id("INSERT INTO track_values VALUES (%lld, 'genre', 0, 'rock')", a8) == 0);
    CHECK(run_id("INSERT INTO changes (batch, track_id, field, value)"
                 " VALUES (1, %lld, 'genre', '[]')", a8) == 0);
    CHECK(run_id("INSERT INTO changes (batch, track_id, field, value)"
                 " VALUES (1, %lld, 'genre', '[\"pop\"]')", a9) == 0);
    CHECK(run_id("INSERT INTO changes (batch, track_id, field, value) VALUES (1, %lld,"
                 " 'musicbrainz_albumid', '00000000-0000-0000-0000-000000000010')", a10) == 0);
    CHECK(run_id("INSERT INTO changes (batch, track_id, field, value)"
                 " VALUES (1, %lld, 'musicbrainz_albumid', '')", a11) == 0);
    CHECK_STR(next_albums(MB_ALBUMS), "a1:2 a10:1 a4:1 a7:1 a8:1");
    CHECK_STR(next_albums(2), "a1:2 a10:1");
    CHECK_STR(next_albums(1), "a1:2");

    /* queueing: the tracks still without a genre, with the album's id */
    struct mb_album a[MB_ALBUMS];
    CHECK(mb_next_albums(a, MB_ALBUMS) == 5);
    struct mb_genres g = { .n = 2, .name = { "pop rock", "r&b" } };
    g.v[0] = g.name[0];
    g.v[1] = g.name[1];
    CHECK(run_id("INSERT INTO track_values VALUES (%lld, 'genre', 0, 'jazz')", a1b) == 0);
    CHECK(mb_queue(&a[0], &g, 7) == 1);                      /* a1b got a genre meanwhile */
    CHECK_STR(pending_genre(a1), "[\"pop rock\",\"r&b\"]");
    CHECK_STR(pending_genre(a1b), "-");
    CHECK(mb_queue(&a[0], &g, 7) == 0);                      /* now it has one */
    CHECK(run_id("UPDATE tracks SET musicbrainz_albumid = NULL WHERE id = %lld", a4) == 0);
    CHECK(mb_queue(&a[2], &g, 7) == 0);                      /* its id changed */
    CHECK(mb_queue(&a[4], &g, 7) == 1);                      /* the pending [] replaced */
    CHECK_STR(pending_genre(a8), "[\"pop rock\",\"r&b\"]");

    /* recording: a lookup that found something is not asked again */
    CHECK(mb_record(&a[1], "not_found", NULL, "no release") == 0);
    CHECK(mb_record(&a[3], "failed", &g, "MusicBrainz answered 500") == 0);
    CHECK_STR(next_albums(MB_ALBUMS), "a7:1");               /* a1, a8 have genres now */
    sqlite3_stmt *st = db_prepare(music_db,
        "SELECT track, album, albumartist, state, genres, note FROM musicbrainz_lookups"
        " ORDER BY id DESC LIMIT 1");
    CHECK(st != NULL && sqlite3_step(st) == SQLITE_ROW);
    CHECK(sqlite3_column_int64(st, 0) == a[3].tracks[0]);
    CHECK_STR((const char *)sqlite3_column_text(st, 1), "a7");
    CHECK_STR((const char *)sqlite3_column_text(st, 2), "A");
    CHECK_STR((const char *)sqlite3_column_text(st, 3), "failed");
    CHECK_STR((const char *)sqlite3_column_text(st, 4), "[\"pop rock\",\"r&b\"]");
    CHECK_STR((const char *)sqlite3_column_text(st, 5), "MusicBrainz answered 500");
    sqlite3_finalize(st);
    CHECK(mb_record(&a[3], "unknown", NULL, "") == -1);      /* the table's states only */

    /* a10 is a10 by its planned id; the file has none */
    CHECK_STR(a[1].mbid, "00000000-0000-0000-0000-000000000010");

    sqlite3_close(music_db);
    music_db = NULL;
    unlink(path);
}

/* The albums mb_next_unidentified(max) gives, as "artist/album:tracks"
 * joined by " ". */
static const char *unidentified(int max)
{
    static char out[512];
    struct mb_album a[MB_ALBUMS];
    int n = mb_next_unidentified(a, max);
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        size_t len = strlen(out);
        snprintf(out + len, sizeof out - len, "%s%s/%s:%zu", i > 0 ? " " : "", a[i].albumartist,
                 a[i].album, a[i].ntracks);
    }
    return n < 0 ? "error" : out;
}

/* The pending MusicBrainz album id of track id, or "-". */
static const char *pending_id(long long id)
{
    static char out[64];
    sqlite3_stmt *st = db_prepare(music_db,
        "SELECT value FROM changes WHERE state = 'pending' AND field = 'musicbrainz_albumid'"
        " AND track_id = ?");
    snprintf(out, sizeof out, "-");
    if (st != NULL && sqlite3_bind_int64(st, 1, id) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        snprintf(out, sizeof out, "%s", (const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return out;
}

static void test_unidentified(void)
{
    char path[64];
    snprintf(path, sizeof path, "%s/ids.db", dir);
    music_db = db_open(path, music_migrations, music_migration_count);
    CHECK(music_db != NULL);
    CHECK_STR(unidentified(MB_ALBUMS), "");                       /* empty library */

    long long u1 = track("u1", NULL), u1b = track("u1", NULL);    /* no id */
    track("u2", NULL);                                            /* one has an id */
    track("u2", ID1);
    track("u3", "123456");                                        /* not an id */
    track("u3", "F2C9C0F4-9C9B-4A7B-8F3A-1234567890AB");
    track_by("u4", NULL, NULL);                                   /* no album artist */
    track_by(NULL, "A", NULL);                                    /* no album */
    long long u5 = track("old", NULL);                            /* album renamed */
    long long u6 = track("u6", NULL);                             /* id planned */
    long long u7 = track("u7", ID2);                              /* id removed */
    track("u8", NULL);                                            /* unsure before */
    track("u9", NULL);                                            /* failed before */
    track_by("u8", "b", NULL);                                    /* other names */
    track("u10", "");                                             /* empty id */
    CHECK(run_id("INSERT INTO changes (batch, track_id, field, value)"
                 " VALUES (1, %lld, 'album', 'u5')", u5) == 0);
    CHECK(run_id("INSERT INTO changes (batch, track_id, field, value) VALUES (1, %lld,"
                 " 'musicbrainz_albumid', '" ID1 "')", u6) == 0);
    CHECK(run_id("INSERT INTO changes (batch, track_id, field, value)"
                 " VALUES (1, %lld, 'musicbrainz_albumid', '')", u7) == 0);
    CHECK(run("INSERT INTO musicbrainz_searches (track, album, albumartist, state) VALUES"
              " (1, 'u8', 'A', 'unsure'), (1, 'u9', 'A', 'failed'), (1, 'u9', 'A', 'skipped')")
          == 0);
    CHECK_STR(unidentified(MB_ALBUMS), "A/u1:2 A/u10:1 A/u3:2 A/u5:1 A/u7:1 A/u9:1 b/u8:1");
    CHECK_STR(unidentified(2), "A/u1:2 A/u10:1");
    CHECK_STR(unidentified(1), "A/u1:2");

    /* queueing: the tracks still without a valid id */
    struct mb_album a[MB_ALBUMS];
    CHECK(mb_next_unidentified(a, MB_ALBUMS) == 7);
    CHECK(run_id("UPDATE tracks SET musicbrainz_albumid = '" ID2 "' WHERE id = %lld", u1b) == 0);
    CHECK(mb_queue_id(&a[0], ID1, 9) == 1);                       /* u1b got one meanwhile */
    CHECK_STR(pending_id(u1), ID1);
    CHECK_STR(pending_id(u1b), "-");
    CHECK(mb_queue_id(&a[0], ID2, 9) == 0);                       /* now u1 has one */
    CHECK(mb_queue_id(&a[4], ID1, 9) == 1);                       /* the pending '' replaced */
    CHECK_STR(pending_id(u7), ID1);

    /* recording: an answered search is not asked again by these names */
    CHECK(mb_record_search(&a[1], "unsure", NULL, "3 releases found") == 0);
    CHECK(mb_record_search(&a[2], "not_found", NULL, "") == 0);
    CHECK(mb_record_search(&a[3], "failed", NULL, "MusicBrainz answered 500") == 0);
    CHECK(mb_record_search(&a[5], "queued", ID2, "a release with these names") == 0);
    CHECK_STR(unidentified(MB_ALBUMS), "A/u5:1 b/u8:1");          /* u1, u7 have ids now */
    sqlite3_stmt *st = db_prepare(music_db,
        "SELECT track, album, albumartist, state, mbid, note FROM musicbrainz_searches"
        " ORDER BY id DESC LIMIT 1");
    CHECK(st != NULL && sqlite3_step(st) == SQLITE_ROW);
    CHECK(sqlite3_column_int64(st, 0) == a[5].tracks[0]);
    CHECK_STR((const char *)sqlite3_column_text(st, 1), "u9");
    CHECK_STR((const char *)sqlite3_column_text(st, 2), "A");
    CHECK_STR((const char *)sqlite3_column_text(st, 3), "queued");
    CHECK_STR((const char *)sqlite3_column_text(st, 4), ID2);
    CHECK_STR((const char *)sqlite3_column_text(st, 5), "a release with these names");
    sqlite3_finalize(st);
    CHECK(mb_record_search(&a[3], "unknown", NULL, "") == -1);    /* the table's states only */
    CHECK(mb_record_search(&a[3], "queued", "123456", "") == -1); /* an id or nothing */

    /* u5 is searched by its planned name */
    CHECK_STR(a[3].album, "u5");
    CHECK_STR(a[3].mbid, "");

    sqlite3_close(music_db);
    music_db = NULL;
    unlink(path);
}

int main(void)
{
    if (arena_init(1024 * 1024) != 0 || mkdtemp(dir) == NULL)
        return 1;
    json_init();
    test_ids();
    test_genres();
    test_language();
    test_add_genre();
    test_add_language();
    test_lyrics_languages();
    test_release_group();
    test_normalize();
    test_search_path();
    test_pick();
    test_albums();
    test_unidentified();
    CHECK(rmdir(dir) == 0);
    TEST_DONE();
}
