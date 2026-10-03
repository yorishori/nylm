/* Music library: which paths are in the library, that the schema matches
 * the tags in src/tags.h, the upgrade to album art, and reading a track's
 * pictures from the cache. */
#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <unistd.h>

#include "../src/arena.h"
#include "../src/db.h"
#include "../src/json.h"
#include "../src/music.h"
#include "test.h"

static void test_inside(void)
{
    CHECK(music_configure("/srv/music/", "/tmp") == 0);
    CHECK_STR(music_root(), "/srv/music");
    CHECK(music_inside("/srv/music"));
    CHECK(music_inside("/srv/music/a"));
    CHECK(music_inside("/srv/music/A b/c.flac"));
    CHECK(music_inside("/srv/music/.hidden/x.mp3"));
    CHECK(music_inside("/srv/music/a..b/c"));
    CHECK(!music_inside("/srv/music/"));          /* empty part */
    CHECK(!music_inside("/srv/music//a"));
    CHECK(!music_inside("/srv/music/a/"));
    CHECK(!music_inside("/srv/music/./a"));
    CHECK(!music_inside("/srv/music/../etc"));
    CHECK(!music_inside("/srv/music/a/.."));
    CHECK(!music_inside("/srv/musicx/a"));        /* only a prefix */
    CHECK(!music_inside("/srv/musi"));
    CHECK(!music_inside("srv/music/a"));
    CHECK(!music_inside(""));
    CHECK(!music_inside("/srv/music/a\nb"));
    CHECK(!music_inside("/srv/music/\xff"));

    static char longest[TAGS_MAX_PATH + 1];
    memcpy(longest, "/srv/music/", 11);
    memset(longest + 11, 'a', TAGS_MAX_PATH - 12);
    longest[TAGS_MAX_PATH - 1] = '\0';
    CHECK(music_inside(longest));                 /* max */
    longest[TAGS_MAX_PATH - 1] = 'a';
    longest[TAGS_MAX_PATH] = '\0';
    CHECK(!music_inside(longest));                /* max + 1 */

    CHECK(music_configure("music", "/tmp") == -1);
    CHECK(music_configure("/", "/tmp") == -1);
    CHECK(music_configure("", "/tmp") == 0 && music_root() == NULL);
    CHECK(!music_inside("/srv/music/a"));         /* nothing is, without a library */
}

static void test_values_json(void)
{
    struct tag_values v;
    CHECK(music_values_parse("[\"a\",\"b \\\"c\\\"\"]", &v) == 0 && v.n == 2);
    CHECK_STR(v.v[1], "b \"c\"");
    CHECK_STR(music_values_json(&v), "[\"a\",\"b \\\"c\\\"\"]");
    CHECK(music_values_parse("[]", &v) == 0 && v.n == 0);
    CHECK_STR(music_values_json(&v), "[]");
    CHECK(music_values_parse("[1]", &v) == -1);
    CHECK(music_values_parse("\"a\"", &v) == -1);
    CHECK(music_values_parse("[", &v) == -1);
}

static char dir[] = "/tmp/nylm-music-XXXXXX";
static char db_path[64];

/* Deletes the database (and its WAL files) in dir. */
static void remove_db(void)
{
    char extra[96];
    unlink(db_path);
    snprintf(extra, sizeof extra, "%s-wal", db_path);
    unlink(extra);
    snprintf(extra, sizeof extra, "%s-shm", db_path);
    unlink(extra);
}

/* A database that had only the first migration, with a track, gets has_art
 * removed and the track marked to be read again. */
static void test_upgrade(void)
{
    sqlite3 *db = db_open(db_path, music_migrations, 1);
    CHECK(db != NULL);
    if (db == NULL)
        return;
    CHECK(db_exec(db, "INSERT INTO tracks (path, size, ext, scanned, has_art, title)"
                      " VALUES ('/m/a.mp3', 1, 'mp3', 1700000000, 1, 'A')") == 0);
    CHECK(db_exec(db, "INSERT INTO changes (batch, track_id, field, value, state, done, note)"
                      " VALUES (4, 1, 'title', 'B', 'done', 1, 'ok')") == 0);
    sqlite3_close(db);
    db = db_open(db_path, music_migrations, music_migration_count);
    CHECK(db != NULL);
    if (db == NULL)
        return;
    sqlite3_stmt *st = db_prepare(db, "SELECT scanned, title FROM tracks");
    CHECK(st != NULL && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int64(st, 0) == 0);
    CHECK_STR((const char *)sqlite3_column_text(st, 1), "A");
    sqlite3_finalize(st);
    st = db_prepare(db, "SELECT count(*) FROM pragma_table_info('tracks') WHERE name = 'has_art'");
    CHECK(st != NULL && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 0);
    sqlite3_finalize(st);
    /* the changes are kept when the table is made again */
    st = db_prepare(db, "SELECT batch, track_id, field, value, state, done, note FROM changes");
    CHECK(st != NULL && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 4 &&
          sqlite3_column_int(st, 1) == 1 && sqlite3_column_int(st, 5) == 1);
    CHECK_STR((const char *)sqlite3_column_text(st, 2), "title");
    CHECK_STR((const char *)sqlite3_column_text(st, 6), "ok");
    CHECK(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    CHECK(db_exec(db, "DELETE FROM tracks") == 0);
    st = db_prepare(db, "SELECT track_id IS NULL FROM changes");
    CHECK(st != NULL && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1);
    sqlite3_finalize(st);
    sqlite3_close(db);
    remove_db();
}

/* The tracks columns after scanned are the single-valued tags in tags.h
 * order, changes accepts exactly the editable fields, and art and
 * track_pictures keep to their rules. */
static void test_schema(void)
{
    sqlite3 *db = db_open(db_path, music_migrations, music_migration_count);
    CHECK(db != NULL);
    if (db == NULL)
        return;

    sqlite3_stmt *st = db_prepare(db, "SELECT name FROM pragma_table_info('tracks')");
    int col = 0;
    while (st != NULL && sqlite3_step(st) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(st, 0);
        if (col >= 5 && col - 5 < TAG_SINGLE_FIELDS)
            CHECK_STR(name, tags_name[col - 5]);
        col++;
    }
    sqlite3_finalize(st);
    CHECK(col == 5 + TAG_SINGLE_FIELDS);

    st = db_prepare(db, "INSERT INTO changes (batch, field, value) VALUES (1, ?, '')");
    for (int i = 0; st != NULL && i < TAG_FIELDS; i++) {
        CHECK(sqlite3_bind_text(st, 1, tags_name[i], -1, SQLITE_STATIC) == SQLITE_OK);
        int rc = sqlite3_step(st);
        CHECK((rc == SQLITE_DONE) == tags_is_editable((enum tag_field)i));
        sqlite3_reset(st);
    }
    CHECK(st != NULL && sqlite3_bind_text(st, 1, MUSIC_COVER_FIELD, -1, SQLITE_STATIC) ==
                            SQLITE_OK && sqlite3_step(st) == SQLITE_DONE);
    sqlite3_reset(st);
    CHECK(sqlite3_bind_text(st, 1, "pictures", -1, SQLITE_STATIC) == SQLITE_OK &&
          sqlite3_step(st) != SQLITE_DONE);
    sqlite3_finalize(st);

    /* art: a hash is 64 lowercase hex characters; mime one nylm knows */
    const char *h = "'" "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef" "'";
    char sql[256];
    snprintf(sql, sizeof sql, "INSERT INTO art (hash, mime, size) VALUES (%s, 'image/png', 1)", h);
    CHECK(db_exec(db, sql) == 0);
    CHECK(db_exec(db, "INSERT INTO art (hash, size) VALUES (upper("
                      "'0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdee'), 1)")
          != 0);
    CHECK(db_exec(db, "INSERT INTO art (hash, size) VALUES ("
                      "'0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcde', 1)")
          != 0);
    CHECK(db_exec(db, "INSERT INTO art (hash, size) VALUES ("
                      "'0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg', 1)")
          != 0);
    CHECK(db_exec(db, "INSERT INTO art (hash, mime, size) VALUES ("
                      "'1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef',"
                      " 'image/svg+xml', 1)") != 0);
    /* track_pictures: of a track, of stored art; gone with the track */
    CHECK(db_exec(db, "INSERT INTO tracks (path, size, ext, scanned) VALUES ('/m/a', 1, 'mp3', 1)")
          == 0);
    snprintf(sql, sizeof sql, "INSERT INTO track_pictures VALUES (1, 0, %s, 'Front Cover', '')", h);
    CHECK(db_exec(db, sql) == 0);
    CHECK(db_exec(db, "INSERT INTO track_pictures VALUES (1, 1, '"
                      "2123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef', '', '')")
          != 0);
    CHECK(db_exec(db, "DELETE FROM art") != 0); /* still used */
    CHECK(db_exec(db, "DELETE FROM tracks") == 0);
    st = db_prepare(db, "SELECT count(*) FROM track_pictures");
    CHECK(st != NULL && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 0);
    sqlite3_finalize(st);
    sqlite3_close(db);
    remove_db();
}

/* Reads the pictures column (pictures, then every tag NULL but genre and
 * composer []) with music_track_tags(): its return value. */
static int track_pictures(const char *json, struct tags *t)
{
    char sql[1024];
    size_t at = (size_t)snprintf(sql, sizeof sql, "SELECT ?");
    for (int i = 0; i < TAG_SINGLE_FIELDS; i++)
        at += (size_t)snprintf(sql + at, sizeof sql - at, ", NULL");
    snprintf(sql + at, sizeof sql - at, ", '[]', '[]'");
    sqlite3 *db;
    CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
    sqlite3_stmt *st = db_prepare(db, sql);
    int rc = -2;
    if (st != NULL && (json == NULL ? sqlite3_bind_null(st, 1)
                                    : sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC)) ==
                          SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        rc = music_track_tags(st, 0, t);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return rc;
}

#define PIC(hash, type, desc) "{\"hash\":\"" hash "\",\"type\":\"" type "\",\"description\":\"" desc "\"}"
#define HASH "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static void test_track_pictures(void)
{
    struct tags t;
    char json[4096];
    CHECK(track_pictures("[]", &t) == 0 && t.npictures == 0);
    CHECK(track_pictures("[" PIC(HASH, "Front Cover", "") "," PIC(HASH, "", "back") "]", &t) == 0);
    CHECK(t.npictures == 2);
    CHECK_STR(t.pictures[0].hash, HASH);
    CHECK_STR(t.pictures[0].type, "Front Cover");
    CHECK_STR(t.pictures[1].description, "back");
    CHECK(t.value[TAG_TITLE].n == 0 && t.value[TAG_GENRE].n == 0);

    /* TAGS_MAX_PICTURES (max) and one more (max + 1) */
    size_t at = 0;
    for (int i = 0; i < TAGS_MAX_PICTURES; i++)
        at += (size_t)snprintf(json + at, sizeof json - at, "%s" PIC(HASH, "", ""),
                               i == 0 ? "[" : ",");
    snprintf(json + at, sizeof json - at, "]");
    CHECK(track_pictures(json, &t) == 0 && t.npictures == TAGS_MAX_PICTURES);
    snprintf(json + at, sizeof json - at, "," PIC(HASH, "", "") "]");
    CHECK(track_pictures(json, &t) == -1);

    CHECK(track_pictures(NULL, &t) == -1);
    CHECK(track_pictures("{}", &t) == -1);
    CHECK(track_pictures("[1]", &t) == -1);
    CHECK(track_pictures("[" PIC("abc", "", "") "]", &t) == -1);
    CHECK(track_pictures("[" PIC("0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef",
                                 "", "") "]", &t) == -1);
    CHECK(track_pictures("[{\"hash\":\"" HASH "\",\"description\":\"\"}]", &t) == -1);
    CHECK(track_pictures("[{\"hash\":\"" HASH "\",\"type\":\"\",\"description\":1}]", &t) == -1);

    /* a new cover: one front cover picture */
    CHECK(music_set_cover(&t, HASH) == 0 && t.npictures == 1);
    CHECK_STR(t.pictures[0].hash, HASH);
    CHECK_STR(t.pictures[0].type, "Front Cover");
    CHECK_STR(t.pictures[0].description, "");
    CHECK(t.pictures[0].data == NULL);
    CHECK(music_set_cover(&t, "abc") == -1);
    CHECK(music_set_cover(&t, "") == -1);
    CHECK(music_set_cover(&t, HASH "0") == -1);
}

int main(void)
{
    if (arena_init(1024 * 1024) != 0)
        return 1;
    json_init();
    test_inside();
    test_values_json();
    CHECK(mkdtemp(dir) != NULL);
    snprintf(db_path, sizeof db_path, "%s/music.db", dir);
    test_upgrade();
    test_schema();
    CHECK(rmdir(dir) == 0);
    test_track_pictures();
    TEST_DONE();
}
