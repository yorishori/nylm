/* Music tags: validation, reading, and safe writes on copies of the files in
 * tests/data (MP3 with ID3v2.3 and no ID3v1; FLAC; FLAC with two genres). */
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
    CHECK(ok(TAG_DISCNUMBER, ""));
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
static void edit_of(const char *path, struct tags_edit *e)
{
    struct stat sb;
    memset(e, 0, sizeof *e);
    CHECK(stat(path, &sb) == 0);
    e->path = path;
    e->format = tags_format_of(path);
    e->size = (long long)sb.st_size;
    e->mtime = tags_mtime(&sb);
}

/* Temporary copies left in the test folder. */
static int leftovers(void)
{
    int n = 0;
    DIR *d = opendir(dir);
    struct dirent *de;
    while (d != NULL && (de = readdir(d)) != NULL)
        if (strncmp(de->d_name, TAGS_TMP_PREFIX, strlen(TAGS_TMP_PREFIX)) == 0)
            n++;
    if (d != NULL)
        closedir(d);
    return n;
}

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
    CHECK(t.multi == 1u << TAG_GENRE);

    /* not audio */
    snprintf(path, sizeof path, "%s/text.mp3", dir);
    FILE *f = fopen(path, "w");
    CHECK(f != NULL && fputs("not an mp3\n", f) >= 0 && fclose(f) == 0);
    CHECK(tags_read(path, TAGS_MP3, &t, err, sizeof err) == -1);
    snprintf(path, sizeof path, "%s/missing.flac", dir);
    CHECK(tags_read(path, TAGS_FLAC, &t, err, sizeof err) == -1);
}

static void test_write_mp3(void)
{
    char path[512], err[256];
    static char buf[16384];
    struct tags_edit e;
    struct tags t;
    struct stat sb;

    copy_fixture("tagged.mp3", "write.mp3", path, sizeof path);
    CHECK(chmod(path, 0640) == 0);
    long before = slurp(path, buf, sizeof buf);
    edit_of(path, &e);
    e.set[TAG_GENRE] = "Jazz";
    e.set[TAG_TITLE] = "Song Uno – ñ";
    e.set[TAG_DATE] = "";              /* remove */
    e.set[TAG_DISCNUMBER] = "1/1";     /* add */
    CHECK(tags_prepare(&e, err, sizeof err) == 0);
    CHECK(e.tmp[0] != '\0' && leftovers() == 1);
    CHECK(slurp(path, buf, sizeof buf) == before); /* original untouched so far */
    CHECK(tags_commit(&e, err, sizeof err) == 0);
    CHECK(e.tmp[0] == '\0' && leftovers() == 0);

    CHECK(tags_read(path, TAGS_MP3, &t, err, sizeof err) == 0);
    CHECK_STR(t.value[TAG_GENRE], "Jazz");
    CHECK_STR(t.value[TAG_TITLE], "Song Uno – ñ");
    CHECK(t.value[TAG_DATE] == NULL);
    CHECK_STR(t.value[TAG_DISCNUMBER], "1/1");
    CHECK_STR(t.value[TAG_ARTIST], "Some Artist");   /* untouched */
    CHECK_STR(t.value[TAG_TRACKNUMBER], "1/2");
    CHECK(stat(path, &sb) == 0 && (sb.st_mode & 0777) == 0640);

    /* no ID3v1 tag added */
    long len = slurp(path, buf, sizeof buf);
    CHECK(len > 128 && memcmp(buf + len - 128, "TAG", 3) != 0);
}

static void test_write_flac(void)
{
    char path[512], err[256];
    struct tags_edit e;
    struct tags t;

    copy_fixture("tagged.flac", "write.flac", path, sizeof path);
    edit_of(path, &e);
    e.set[TAG_ALBUM] = "Other Album";
    e.set[TAG_COMPILATION] = "1";
    CHECK(tags_prepare(&e, err, sizeof err) == 0);
    CHECK(tags_commit(&e, err, sizeof err) == 0);
    CHECK(tags_read(path, TAGS_FLAC, &t, err, sizeof err) == 0);
    CHECK_STR(t.value[TAG_ALBUM], "Other Album");
    CHECK_STR(t.value[TAG_COMPILATION], "1");
    CHECK_STR(t.value[TAG_GENRE], "Rock");

    /* setting a value it already has: still a valid write */
    edit_of(path, &e);
    e.set[TAG_ALBUM] = "Other Album";
    CHECK(tags_prepare(&e, err, sizeof err) == 0);
    tags_discard(&e);
    CHECK(leftovers() == 0);
}

/* Every refusal leaves the original byte for byte and no copy behind. */
static void test_refusals(void)
{
    char path[512], err[256];
    static char a[16384], b[16384];
    struct tags_edit e;
    struct tags t;

    /* changed since the scan */
    copy_fixture("tagged.flac", "stale.flac", path, sizeof path);
    long len = slurp(path, a, sizeof a);
    edit_of(path, &e);
    e.mtime--;
    e.set[TAG_GENRE] = "Pop";
    CHECK(tags_prepare(&e, err, sizeof err) == TAGS_STALE);
    edit_of(path, &e);
    e.size++;
    e.set[TAG_GENRE] = "Pop";
    CHECK(tags_prepare(&e, err, sizeof err) == TAGS_STALE);
    CHECK(slurp(path, b, sizeof b) == len && memcmp(a, b, (size_t)len) == 0);

    /* changed between prepare and commit */
    edit_of(path, &e);
    e.set[TAG_GENRE] = "Pop";
    CHECK(tags_prepare(&e, err, sizeof err) == 0);
    struct timespec times[2] = { { 0, UTIME_OMIT }, { 1000000000, 0 } };
    CHECK(utimensat(AT_FDCWD, path, times, 0) == 0);
    CHECK(tags_commit(&e, err, sizeof err) == TAGS_STALE);
    CHECK(leftovers() == 0);
    CHECK(slurp(path, b, sizeof b) == len && memcmp(a, b, (size_t)len) == 0);

    /* gone */
    snprintf(path, sizeof path, "%s/gone.flac", dir);
    e.path = path;
    CHECK(tags_prepare(&e, err, sizeof err) == TAGS_STALE);

    /* several values in a tag to change; other tags can still change */
    copy_fixture("multi.flac", "locked.flac", path, sizeof path);
    len = slurp(path, a, sizeof a);
    edit_of(path, &e);
    e.set[TAG_GENRE] = "Rock";
    CHECK(tags_prepare(&e, err, sizeof err) == TAGS_LOCKED);
    CHECK(slurp(path, b, sizeof b) == len && memcmp(a, b, (size_t)len) == 0);
    edit_of(path, &e);
    e.set[TAG_TITLE] = "Renamed";
    CHECK(tags_prepare(&e, err, sizeof err) == 0 && tags_commit(&e, err, sizeof err) == 0);
    CHECK(tags_read(path, TAGS_FLAC, &t, err, sizeof err) == 0);
    CHECK_STR(t.value[TAG_TITLE], "Renamed");
    CHECK_STR(t.value[TAG_GENRE], "Rock; Pop");

    /* not audio */
    snprintf(path, sizeof path, "%s/text.mp3", dir);
    edit_of(path, &e);
    e.set[TAG_GENRE] = "Pop";
    CHECK(tags_prepare(&e, err, sizeof err) == -1);

    /* a symlink is never followed */
    char link[512];
    copy_fixture("tagged.flac", "target.flac", path, sizeof path);
    snprintf(link, sizeof link, "%s/link.flac", dir);
    CHECK(symlink(path, link) == 0);
    edit_of(link, &e); /* stat follows: size and mtime of the target */
    e.set[TAG_GENRE] = "Pop";
    CHECK(tags_prepare(&e, err, sizeof err) == TAGS_STALE);

    /* prepared, then dropped */
    copy_fixture("tagged.mp3", "dropped.mp3", path, sizeof path);
    len = slurp(path, a, sizeof a);
    edit_of(path, &e);
    e.set[TAG_GENRE] = "Pop";
    CHECK(tags_prepare(&e, err, sizeof err) == 0);
    tags_discard(&e);
    CHECK(slurp(path, b, sizeof b) == len && memcmp(a, b, (size_t)len) == 0);

    CHECK(leftovers() == 0);
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
