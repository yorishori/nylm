/* Music tags: validation, reading, and writes through TagLib, on copies of
 * the files in tests/data (MP3 with ID3v2.3; FLAC; FLAC with two genres;
 * MP3 with the track number "0/0", which TagLib drops on save). */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/arena.h"
#include "../src/tags.h"
#include "test.h"

static char dir[] = "/tmp/nylm-tags-XXXXXX";

static void test_format(void)
{
    CHECK(tags_format_of("a.mp3") == TAGS_MP3);
    CHECK(tags_format_of("a.MP3") == TAGS_MP3);
    CHECK(tags_format_of("a.b.flac") == TAGS_FLAC);
    CHECK(tags_format_of("a.FLAC") == TAGS_FLAC);
    CHECK(tags_format_of("a.ogg") == TAGS_NONE);
    CHECK(tags_format_of("mp3") == TAGS_NONE);
    CHECK(tags_format_of("a.mp3.jpg") == TAGS_NONE);
    CHECK(tags_format_of("") == TAGS_NONE);
}

static int ok(enum tag_field f, const char *v)
{
    return tags_check_value(f, v) == NULL;
}

static void test_values(void)
{
    char s[TAGS_MAX_VALUE + 2];

    CHECK(ok(TAG_TITLE, "Song"));
    CHECK(ok(TAG_TITLE, "Ça va – 日本"));
    CHECK(!ok(TAG_TITLE, ""));        /* required */
    CHECK(!ok(TAG_ARTIST, ""));
    CHECK(!ok(TAG_ALBUM, ""));
    CHECK(!ok(TAG_ALBUMARTIST, ""));
    CHECK(!ok(TAG_TRACKNUMBER, ""));
    CHECK(ok(TAG_GENRE, ""));         /* removable */
    CHECK(ok(TAG_DATE, ""));
    CHECK(!ok(TAG_DISCNUMBER, ""));    /* required */
    CHECK(ok(TAG_COMPILATION, ""));
    CHECK(!ok(TAG_TITLE, " Song"));
    CHECK(!ok(TAG_TITLE, "Song "));
    CHECK(!ok(TAG_TITLE, "a\nb"));
    CHECK(!ok(TAG_TITLE, "a\tb"));
    CHECK(!ok(TAG_TITLE, "\xff"));    /* not UTF-8 */
    memset(s, 'a', TAGS_MAX_VALUE);
    s[TAGS_MAX_VALUE] = '\0';
    CHECK(ok(TAG_GENRE, s));          /* max */
    s[TAGS_MAX_VALUE] = 'a';
    s[TAGS_MAX_VALUE + 1] = '\0';
    CHECK(!ok(TAG_GENRE, s));         /* max + 1 */

    CHECK(ok(TAG_DATE, "1999"));
    CHECK(ok(TAG_DATE, "1999-12"));
    CHECK(ok(TAG_DATE, "2000-02-29"));
    CHECK(!ok(TAG_DATE, "1900-02-29"));
    CHECK(!ok(TAG_DATE, "1999-02-29"));
    CHECK(!ok(TAG_DATE, "1999-04-31"));
    CHECK(!ok(TAG_DATE, "1999-13"));
    CHECK(!ok(TAG_DATE, "1999-00"));
    CHECK(!ok(TAG_DATE, "1999-01-00"));
    CHECK(!ok(TAG_DATE, "99"));
    CHECK(!ok(TAG_DATE, "1999/01/01"));
    CHECK(!ok(TAG_DATE, "1999-1-1"));
    CHECK(!ok(TAG_DATE, "19a9"));

    CHECK(ok(TAG_TRACKNUMBER, "1"));
    CHECK(ok(TAG_TRACKNUMBER, "03"));
    CHECK(ok(TAG_TRACKNUMBER, "3/12"));
    CHECK(ok(TAG_TRACKNUMBER, "9999/9999"));
    CHECK(ok(TAG_DISCNUMBER, "1/1"));
    CHECK(!ok(TAG_TRACKNUMBER, "0"));
    CHECK(!ok(TAG_TRACKNUMBER, "12/3"));
    CHECK(!ok(TAG_TRACKNUMBER, "10000"));
    CHECK(!ok(TAG_TRACKNUMBER, "1/10000"));
    CHECK(!ok(TAG_TRACKNUMBER, "1/"));
    CHECK(!ok(TAG_TRACKNUMBER, "/1"));
    CHECK(!ok(TAG_TRACKNUMBER, "1/2/3"));
    CHECK(!ok(TAG_TRACKNUMBER, "-1"));
    CHECK(!ok(TAG_TRACKNUMBER, "A1"));

    /* genres: words of lowercase a-z and '-' with single spaces, several
     * separated by "; " */
    CHECK(ok(TAG_GENRE, "hip hop"));
    CHECK(ok(TAG_GENRE, "hip hop; drum and bass; lo-fi"));
    CHECK(!ok(TAG_GENRE, "hip  hop"));
    CHECK(!ok(TAG_GENRE, "hip hop ; jazz"));
    CHECK(!ok(TAG_GENRE, "hip hop;jazz"));
    CHECK(!ok(TAG_GENRE, "hip ; hop"));
    CHECK(!ok(TAG_GENRE, "rock;  pop-punk"));
    CHECK(ok(TAG_GENRE, "rock"));
    CHECK(ok(TAG_GENRE, "pop-punk"));
    CHECK(ok(TAG_GENRE, "rock; pop-punk; jazz"));
    CHECK(ok(TAG_GENRE, "-"));
    CHECK(!ok(TAG_GENRE, "Rock"));
    CHECK(!ok(TAG_GENRE, "rock;pop"));
    CHECK(!ok(TAG_GENRE, "rock,pop"));
    CHECK(!ok(TAG_GENRE, "rock;  pop"));
    CHECK(!ok(TAG_GENRE, "rock ; pop"));
    CHECK(!ok(TAG_GENRE, "rock; "));
    CHECK(!ok(TAG_GENRE, "rock;"));
    CHECK(!ok(TAG_GENRE, "; rock"));
    CHECK(!ok(TAG_GENRE, "rock; ; pop"));
    CHECK(!ok(TAG_GENRE, "rock2"));
    CHECK(!ok(TAG_GENRE, "rock_pop"));
    CHECK(!ok(TAG_GENRE, "rock/pop"));
    CHECK(!ok(TAG_GENRE, "électro"));

    /* lists: artist, album artist, composer (genre: see above) */
    CHECK(ok(TAG_ARTIST, "Some Artist"));
    CHECK(ok(TAG_ARTIST, "AC/DC; Sunn O)))"));
    CHECK(ok(TAG_ALBUMARTIST, "A; B; C"));
    CHECK(ok(TAG_COMPOSER, ""));             /* removable */
    CHECK(ok(TAG_COMPOSER, "Bach; Händel"));
    CHECK(!ok(TAG_ARTIST, ""));
    CHECK(!ok(TAG_ARTIST, "A;B"));
    CHECK(!ok(TAG_ARTIST, "A ; B"));
    CHECK(!ok(TAG_ARTIST, "A;  B"));
    CHECK(!ok(TAG_ARTIST, "A; ; B"));
    CHECK(!ok(TAG_ARTIST, "A;"));
    CHECK(!ok(TAG_ARTIST, "; A"));
    CHECK(!ok(TAG_COMPOSER, "A; B;"));
    CHECK(ok(TAG_TITLE, "A;B"));             /* not a list field */

    CHECK(ok(TAG_COMPILATION, "1"));
    CHECK(!ok(TAG_COMPILATION, "0"));
    CHECK(!ok(TAG_COMPILATION, "yes"));
}

/* Copies tests/data/name into the test folder as `as`; its path in out. */
static void copy_fixture(const char *name, const char *as, char *out, size_t size)
{
    char from[256], buf[8192];
    snprintf(from, sizeof from, "tests/data/%s", name);
    snprintf(out, size, "%s/%s", dir, as);
    FILE *in = fopen(from, "rb"), *to = fopen(out, "wb");
    CHECK(in != NULL && to != NULL);
    size_t n;
    while (in != NULL && to != NULL && (n = fread(buf, 1, sizeof buf, in)) > 0)
        CHECK(fwrite(buf, 1, n, to) == n);
    if (in != NULL)
        fclose(in);
    if (to != NULL)
        CHECK(fclose(to) == 0);
}

/* The whole file into a static buffer; its length, or -1. */
static long slurp(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -1;
    size_t n = fread(buf, 1, size, f);
    fclose(f);
    return (long)n;
}

/* An edit of path as it is now, changing nothing yet. */
static void test_read(void)
{
    char path[512], err[256];
    struct tags t;

    copy_fixture("tagged.mp3", "read.mp3", path, sizeof path);
    CHECK(tags_read(path, TAGS_MP3, &t, err, sizeof err) == 0);
    CHECK_STR(t.value[TAG_TITLE], "Song One");
    CHECK_STR(t.value[TAG_ALBUMARTIST], "Some Artist");
    CHECK_STR(t.value[TAG_TRACKNUMBER], "1/2");
    CHECK_STR(t.value[TAG_DATE], "2001");
    CHECK(t.value[TAG_DISCNUMBER] == NULL);
    CHECK(t.multi == 0 && t.pictures == 0 && t.seconds == 1);

    copy_fixture("multi.flac", "read.flac", path, sizeof path);
    CHECK(tags_read(path, TAGS_FLAC, &t, err, sizeof err) == 0);
    CHECK_STR(t.value[TAG_GENRE], "Rock; Pop");
    CHECK_STR(t.value[TAG_ARTIST], "Some Artist; Other Artist");
    CHECK_STR(t.value[TAG_TITLE], "Song Two; Other Title");
    CHECK(t.value[TAG_COMPOSER] == NULL);
    CHECK(t.multi == ((1u << TAG_GENRE) | (1u << TAG_ARTIST) | (1u << TAG_TITLE)));

    /* not audio */
    snprintf(path, sizeof path, "%s/text.mp3", dir);
    FILE *f = fopen(path, "w");
    CHECK(f != NULL && fputs("not an mp3\n", f) >= 0 && fclose(f) == 0);
    CHECK(tags_read(path, TAGS_MP3, &t, err, sizeof err) == -1);
    snprintf(path, sizeof path, "%s/missing.flac", dir);
    CHECK(tags_read(path, TAGS_FLAC, &t, err, sizeof err) == -1);
}

/* A change of field from old to value, as the write service builds it. */
static struct tags_change change(enum tag_field field, const char *old, const char *value)
{
    struct tags_change c;
    memset(&c, 0, sizeof c);
    c.field = field;
    c.old = old;
    c.value = value;
    return c;
}

static void test_write_mp3(void)
{
    char path[512], err[256];
    struct tags t;
    struct stat sb;

    copy_fixture("tagged.mp3", "write.mp3", path, sizeof path);
    CHECK(chmod(path, 0640) == 0);
    struct tags_change c[] = {
        change(TAG_GENRE, "Rock", "jazz"),
        change(TAG_TITLE, "Song One", "Song Uno – ñ"),
        change(TAG_DATE, "2001", ""),          /* remove */
        change(TAG_DISCNUMBER, NULL, "1/1"),   /* add */
    };
    CHECK(tags_write(path, TAGS_MP3, c, 4) == 1);
    for (int i = 0; i < 4; i++)
        CHECK(!c[i].failed && c[i].note[0] == '\0');

    CHECK(tags_read(path, TAGS_MP3, &t, err, sizeof err) == 0);
    CHECK_STR(t.value[TAG_GENRE], "jazz");
    CHECK_STR(t.value[TAG_TITLE], "Song Uno – ñ");
    CHECK(t.value[TAG_DATE] == NULL);
    CHECK_STR(t.value[TAG_DISCNUMBER], "1/1");
    CHECK_STR(t.value[TAG_ARTIST], "Some Artist"); /* untouched */
    CHECK_STR(t.value[TAG_TRACKNUMBER], "1/2");
    CHECK(stat(path, &sb) == 0 && (sb.st_mode & 0777) == 0640); /* written in place */
}

static void test_write_flac(void)
{
    char path[512], err[256];
    struct tags t;

    copy_fixture("tagged.flac", "write.flac", path, sizeof path);
    struct tags_change c[] = {
        change(TAG_ALBUM, "Some Album", "Other Album"),
        change(TAG_COMPILATION, NULL, "1"),
    };
    CHECK(tags_write(path, TAGS_FLAC, c, 2) == 1);
    CHECK(!c[0].failed && !c[1].failed);
    CHECK(tags_read(path, TAGS_FLAC, &t, err, sizeof err) == 0);
    CHECK_STR(t.value[TAG_ALBUM], "Other Album");
    CHECK_STR(t.value[TAG_COMPILATION], "1");
    CHECK_STR(t.value[TAG_GENRE], "Rock");

    /* a valid and an invalid change: only the valid one is written */
    struct tags_change d[] = {
        change(TAG_GENRE, "Rock", "pop"),
        change(TAG_TITLE, "Song Two", ""),     /* required */
        change(TAG_DATE, "2001", "2001-02-30"),
        change(TAG_ARTIST, "Some Artist", " Spaced"),
        change(TAG_GENRE, "Rock", "blues"),    /* the same tag twice */
    };
    CHECK(tags_write(path, TAGS_FLAC, d, 5) == 1);
    CHECK(!d[0].failed);
    CHECK(d[1].failed && strstr(d[1].note, "can not be empty") != NULL);
    CHECK(d[2].failed && strstr(d[2].note, "must be a date") != NULL);
    CHECK(d[3].failed && strstr(d[3].note, "space") != NULL);
    CHECK(d[4].failed && strstr(d[4].note, "twice") != NULL);
    CHECK(tags_read(path, TAGS_FLAC, &t, err, sizeof err) == 0);
    CHECK_STR(t.value[TAG_GENRE], "pop");
    CHECK_STR(t.value[TAG_TITLE], "Song Two");
    CHECK_STR(t.value[TAG_DATE], "2001");
}

/* Refusals leave the file byte for byte as it was. */
static void test_refusals(void)
{
    char path[512], err[256];
    static char a[16384], b[16384];
    struct tags t;

    /* the file no longer has the value the change was queued against */
    copy_fixture("tagged.flac", "stale.flac", path, sizeof path);
    long len = slurp(path, a, sizeof a);
    struct tags_change c[] = {
        change(TAG_GENRE, "Pop", "jazz"),
        change(TAG_DISCNUMBER, "1", "2"),  /* absent in the file */
    };
    CHECK(tags_write(path, TAGS_FLAC, c, 2) == 0);
    CHECK(c[0].failed && strstr(c[0].note, "it now has Rock") != NULL);
    CHECK(c[1].failed && strstr(c[1].note, "it now has no value") != NULL);
    CHECK(slurp(path, b, sizeof b) == len && memcmp(a, b, (size_t)len) == 0);

    /* all changes invalid: nothing written */
    struct tags_change bad = change(TAG_TITLE, "Song Two", "a\nb");
    CHECK(tags_write(path, TAGS_FLAC, &bad, 1) == 0 && bad.failed);
    CHECK(slurp(path, b, sizeof b) == len && memcmp(a, b, (size_t)len) == 0);

    /* several values: a list field (artist, album artist, genre, composer)
     * is replaced by the one new string once the file still has the
     * queued-against values; other tags are refused */
    copy_fixture("multi.flac", "locked.flac", path, sizeof path);
    len = slurp(path, a, sizeof a);
    struct tags_change m = change(TAG_TITLE, "Song Two; Other Title", "Me");
    CHECK(tags_write(path, TAGS_FLAC, &m, 1) == 0);
    CHECK(m.failed && strstr(m.note, "title has several values") != NULL);
    m = change(TAG_GENRE, "Rock", "rock");
    CHECK(tags_write(path, TAGS_FLAC, &m, 1) == 0);
    CHECK(m.failed && strstr(m.note, "it now has Rock; …") != NULL);
    CHECK(slurp(path, b, sizeof b) == len && memcmp(a, b, (size_t)len) == 0);
    struct tags_change r[] = {
        change(TAG_GENRE, "Rock; Pop", "rock; pop"),
        change(TAG_ARTIST, "Some Artist; Other Artist", "Some Artist; Another"),
        change(TAG_COMPOSER, NULL, "Composer A; Composer B"),
    };
    CHECK(tags_write(path, TAGS_FLAC, r, 3) == 1);
    CHECK(!r[0].failed && !r[1].failed && !r[2].failed);
    CHECK(r[0].note[0] == '\0');
    CHECK(tags_read(path, TAGS_FLAC, &t, err, sizeof err) == 0);
    CHECK_STR(t.value[TAG_GENRE], "rock; pop");
    CHECK_STR(t.value[TAG_ARTIST], "Some Artist; Another");
    CHECK_STR(t.value[TAG_COMPOSER], "Composer A; Composer B");
    CHECK_STR(t.value[TAG_TITLE], "Song Two; Other Title");
    CHECK(t.multi == 1u << TAG_TITLE); /* the list fields are one value now */

    /* not audio, missing, a symlink, a folder */
    struct tags_change g = change(TAG_GENRE, NULL, "pop");
    snprintf(path, sizeof path, "%s/text.mp3", dir);
    CHECK(tags_write(path, TAGS_MP3, &g, 1) == 0 && g.failed);
    snprintf(path, sizeof path, "%s/gone.flac", dir);
    CHECK(tags_write(path, TAGS_FLAC, &g, 1) == 0 && strstr(g.note, "not found") != NULL);
    char link[512];
    copy_fixture("tagged.flac", "target.flac", path, sizeof path);
    snprintf(link, sizeof link, "%s/link.flac", dir);
    CHECK(symlink(path, link) == 0);
    g = change(TAG_GENRE, "Rock", "pop");
    CHECK(tags_write(link, TAGS_FLAC, &g, 1) == 0 && strstr(g.note, "regular") != NULL);
    CHECK(tags_write(dir, TAGS_FLAC, &g, 1) == 0 && g.failed);
}

/* TagLib normalising another tag on save: done, with a warning. */
static void test_warning(void)
{
    char path[512], err[256];
    struct tags t;

    copy_fixture("odd.mp3", "odd.mp3", path, sizeof path);
    CHECK(tags_read(path, TAGS_MP3, &t, err, sizeof err) == 0);
    CHECK_STR(t.value[TAG_TRACKNUMBER], "0/0");
    struct tags_change c = change(TAG_GENRE, "Rock", "jazz");
    CHECK(tags_write(path, TAGS_MP3, &c, 1) == 1);
    CHECK(!c.failed);
    CHECK(strstr(c.note, "TagLib also changed: TRACKNUMBER") != NULL);
    CHECK(tags_read(path, TAGS_MP3, &t, err, sizeof err) == 0);
    CHECK_STR(t.value[TAG_GENRE], "jazz");
}

int main(void)
{
    if (mkdtemp(dir) == NULL || arena_init(1024 * 1024) != 0)
        return 1;

    test_format();
    test_values();
    test_read();
    test_write_mp3();
    test_write_flac();
    test_refusals();
    test_warning();

    /* the test folder holds only files */
    DIR *d = opendir(dir);
    struct dirent *de;
    char path[512];
    while (d != NULL && (de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        CHECK(unlink(path) == 0);
    }
    if (d != NULL)
        closedir(d);
    CHECK(rmdir(dir) == 0);
    TEST_DONE();
}
