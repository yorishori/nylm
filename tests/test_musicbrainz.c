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

/* Adding genres: no name twice, and no more than MB_GENRES + 1. */
static void test_add_genre(void)
{
    struct mb_genres g = { .n = 0 };
    mb_add_genre(&g, "rock");
    mb_add_genre(&g, "rock");
    CHECK(g.n == 1);
    const char *names[] = { "a", "b", "c", "d", "e", "f", "g" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        mb_add_genre(&g, names[i]);
    CHECK(g.n == MB_GENRES + 1);
    CHECK_STR(g.v[0], "rock");
    CHECK_STR(g.v[MB_GENRES], "e");
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

/* Runs sql on the music database; 0 or -1. */
static int run(const char *sql)
{
    return sqlite3_exec(music_db, sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

/* A track of album (by albumartist "A") with mbid (NULL: none). Its id. */
static long long track(const char *album, const char *mbid)
{
    static int next;
    char path[64];
    snprintf(path, sizeof path, "/m/%d.flac", ++next);
    sqlite3_stmt *st = db_prepare(music_db,
        "INSERT INTO tracks (path, size, ext, scanned, album, albumartist, musicbrainz_albumid)"
        " VALUES (?, 1, 'flac', 1, ?, 'A', ?)");
    long long id = st != NULL && sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                           sqlite3_bind_text(st, 2, album, -1, SQLITE_STATIC) == SQLITE_OK &&
                           sqlite3_bind_text(st, 3, mbid, -1, SQLITE_STATIC) == SQLITE_OK &&
                           sqlite3_step(st) == SQLITE_DONE
                       ? sqlite3_last_insert_rowid(music_db)
                       : -1;
    sqlite3_finalize(st);
    return id;
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

int main(void)
{
    if (arena_init(1024 * 1024) != 0 || mkdtemp(dir) == NULL)
        return 1;
    json_init();
    test_ids();
    test_genres();
    test_language();
    test_add_genre();
    test_release_group();
    test_albums();
    CHECK(rmdir(dir) == 0);
    TEST_DONE();
}
