/* Music library: which paths are in the library, and that the schema
 * matches the tags in src/tags.h. */
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

/* The tracks columns after has_art are the single-valued tags in tags.h
 * order, and changes accepts exactly the editable fields. */
static void test_schema(void)
{
    char dir[] = "/tmp/nylm-music-XXXXXX", path[64];
    CHECK(mkdtemp(dir) != NULL);
    snprintf(path, sizeof path, "%s/music.db", dir);
    sqlite3 *db = db_open(path, music_migrations, music_migration_count);
    CHECK(db != NULL);
    if (db == NULL)
        return;

    sqlite3_stmt *st = db_prepare(db, "SELECT name FROM pragma_table_info('tracks')");
    int col = 0;
    while (st != NULL && sqlite3_step(st) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(st, 0);
        if (col >= 6 && col - 6 < TAG_SINGLE_FIELDS)
            CHECK_STR(name, tags_name[col - 6]);
        col++;
    }
    sqlite3_finalize(st);
    CHECK(col == 6 + TAG_SINGLE_FIELDS);

    st = db_prepare(db, "INSERT INTO changes (batch, field, value) VALUES (1, ?, '')");
    for (int i = 0; st != NULL && i < TAG_FIELDS; i++) {
        CHECK(sqlite3_bind_text(st, 1, tags_name[i], -1, SQLITE_STATIC) == SQLITE_OK);
        int rc = sqlite3_step(st);
        CHECK((rc == SQLITE_DONE) == tags_is_editable((enum tag_field)i));
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);

    unlink(path);
    char extra[96];
    snprintf(extra, sizeof extra, "%s/music.db-wal", dir);
    unlink(extra);
    snprintf(extra, sizeof extra, "%s/music.db-shm", dir);
    unlink(extra);
    CHECK(rmdir(dir) == 0);
}

int main(void)
{
    if (arena_init(1024 * 1024) != 0)
        return 1;
    json_init();
    test_inside();
    test_values_json();
    test_schema();
    TEST_DONE();
}
