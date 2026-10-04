/* Keys for possible duplicates: what makes two names the same. */
#include <string.h>

#include "../src/dupes.h"
#include "test.h"

static char out[DUPES_KEY_MAX + 1];

/* The key of s (a C string). */
static const char *key(const char *s)
{
    dupes_key(s, strlen(s), out, sizeof out);
    return out;
}

/* 1 if a and b have the same key. */
static int same(const char *a, const char *b)
{
    char ka[DUPES_KEY_MAX + 1];
    dupes_key(a, strlen(a), ka, sizeof ka);
    return strcmp(ka, key(b)) == 0;
}

static void test_names(void)
{
    CHECK_STR(key("Radiohead"), "radiohead");
    CHECK_STR(key("OK Computer"), "okcomputer");
    CHECK(same("OK Computer", "Ok computer"));
    CHECK(same("AC/DC", "ACDC"));
    CHECK(same("Guns N' Roses", "Guns N Roses"));
    CHECK(same("hip-hop", "Hip Hop"));
    CHECK(same("Simon & Garfunkel", "Simon and Garfunkel"));
    CHECK_STR(key("R&B"), "randb");
    CHECK(same("Beyoncé", "Beyonce"));
    CHECK(same("Beyonce\xcc\x81", "Beyonce"));      /* decomposed accent */
    CHECK(same("Motörhead", "Motorhead"));
    CHECK_STR(key("Æther Straße"), "aetherstrasse");
    CHECK_STR(key("Łódź Œuvre Þór"), "lodzoeuvrethor");
    CHECK_STR(key("ÀÿĀſ"), "ayas");                 /* the table's ends */
    CHECK_STR(key("2 × 3 ÷ 1"), "231");              /* signs are dropped */
    CHECK(same("\xe2\x80\x9cQuoted\xe2\x80\x9d \xe2\x80\x93 Live", "Quoted Live"));
    CHECK(same("Sigur Rós\xc2\xa0", "Sigur Ros"));   /* no-break space */
    CHECK(same("\xef\xbb\xbf" "BOM", "bom"));
    CHECK_STR(key("Мумий Тролль"), "МумийТролль");   /* another script as it is */
    CHECK_STR(key("坂本龍一、"), "坂本龍一");          /* CJK punctuation dropped */
    CHECK(!same("Cream", "Dream"));
    CHECK(!same("Beyonce", "Beyonse"));
}

static void test_the(void)
{
    CHECK(same("The Beatles", "Beatles"));
    CHECK(same("the beatles", "Beatles"));
    CHECK(same("  THE Who", "Who"));
    CHECK_STR(key("The The"), "the");
    CHECK_STR(key("The"), "the");
    CHECK_STR(key("The "), "the");                   /* nothing after it */
    CHECK_STR(key("The !!"), "the");
    CHECK_STR(key("Theatre"), "theatre");
    CHECK_STR(key("The-Dream"), "thedream");         /* only "The " */
    CHECK_STR(key("A The B"), "atheb");              /* only at the start */
}

static void test_bounds(void)
{
    CHECK(dupes_key("", 0, out, sizeof out) == 0 && out[0] == '\0');
    CHECK(dupes_key("!? -", 4, out, sizeof out) == 0);
    CHECK(dupes_key("\xff\xfe", 2, out, sizeof out) == 0);       /* not UTF-8 */
    CHECK_STR(key("a\xffz\xc3"), "az");                         /* bad bytes skipped */
    CHECK_STR(key("\xc0\xaf" "x"), "x");                         /* overlong */
    CHECK_STR(key("\xed\xa0\x80" "x"), "x");                     /* surrogate */
    CHECK(dupes_key("abc", 2, out, sizeof out) == 2);            /* only n bytes */
    CHECK_STR(out, "ab");
    CHECK(dupes_key("abc", 3, out, 0) == 0);                     /* no room at all */
    CHECK(dupes_key("abc", 3, out, 1) == 0 && out[0] == '\0');
    CHECK(dupes_key("&", 1, out, 3) == 0);                       /* "and" does not fit */
    CHECK(dupes_key("&", 1, out, 4) == 3);
    CHECK(dupes_key("é", 2, out, 2) == 1);
    CHECK(dupes_key("ж", 2, out, 2) == 0);                       /* never half a letter */
    CHECK(dupes_key("ж", 2, out, 3) == 2);

    static char longest[DUPES_KEY_MAX + 2];
    memset(longest, 'A', DUPES_KEY_MAX);
    CHECK(dupes_key(longest, DUPES_KEY_MAX, out, sizeof out) == DUPES_KEY_MAX); /* max */
    CHECK(out[DUPES_KEY_MAX - 1] == 'a' && out[DUPES_KEY_MAX] == '\0');
    memset(longest, 'A', DUPES_KEY_MAX + 1);
    CHECK(dupes_key(longest, DUPES_KEY_MAX + 1, out, sizeof out) == DUPES_KEY_MAX); /* cut */
}

/* dupe_key() in SQL: the key, NULL for NULL and for an empty key. */
static void test_sql(void)
{
    sqlite3 *db;
    CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
    CHECK(dupes_register(db) == SQLITE_OK);
    sqlite3_stmt *st;
    CHECK(sqlite3_prepare_v2(db,
        "SELECT dupe_key('The Beatles'), dupe_key(NULL), dupe_key('...'),"
        " dupe_key('Beyoncé') = dupe_key('beyonce')", -1, &st, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(st) == SQLITE_ROW);
    CHECK_STR((const char *)sqlite3_column_text(st, 0), "beatles");
    CHECK(sqlite3_column_type(st, 1) == SQLITE_NULL);
    CHECK(sqlite3_column_type(st, 2) == SQLITE_NULL);
    CHECK(sqlite3_column_int(st, 3) == 1);
    sqlite3_finalize(st);
    sqlite3_close(db);
}

int main(void)
{
    test_names();
    test_the();
    test_bounds();
    test_sql();
    TEST_DONE();
}
