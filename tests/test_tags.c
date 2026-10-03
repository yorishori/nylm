/* Music tags: the field table, the rules, preparing a write, reading and
 * writing through TagLib (pictures included) on copies of the files in
 * tests/data (MP3 with ID3v2.3; FLAC; FLAC with two genres and two titles;
 * MP3 with the track number "0/0", which TagLib drops on save). */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <taglib/tag_c.h>

#include "../src/arena.h"
#include "../src/tags.h"
#include "test.h"

static char dir[] = "/tmp/nylm-tags-XXXXXX";

/* One value, or none for NULL. */
static struct tag_values one(const char *s)
{
    static const char *slot[16];
    static int next;
    struct tag_values v = { 0, NULL };
    if (s != NULL) {
        next = (next + 1) % 16;
        slot[next] = s;
        v.n = 1;
        v.v = &slot[next];
    }
    return v;
}

static int ok(enum tag_field f, const char *s)
{
    struct tag_values v = one(s);
    return tags_check(f, &v) == NULL;
}

/* n values: list[0..n). */
static int ok_list(enum tag_field f, const char **list, size_t n)
{
    struct tag_values v = { n, list };
    return tags_check(f, &v) == NULL;
}

static void test_fields(void)
{
    for (int i = 0; i < TAG_FIELDS; i++) {
        CHECK(tags_field_of(tags_name[i]) == i);
        for (const char *k = tags_key[i], *n = tags_name[i]; *k != '\0' || *n != '\0'; k++, n++)
            CHECK(*n == (*k >= 'A' && *k <= 'Z' ? *k - 'A' + 'a' : *k)); /* same name */
    }
    CHECK(tags_field_of("TITLE") == -1);
    CHECK(tags_field_of("") == -1);
    CHECK(tags_field_of("comment") == -1);

    CHECK(tags_is_multi(TAG_GENRE) && tags_is_multi(TAG_COMPOSER));
    CHECK(!tags_is_multi(TAG_ARTIST) && !tags_is_multi(TAG_TITLE));
    for (int i = 0; i < TAG_SINGLE_FIELDS; i++)
        CHECK(!tags_is_multi((enum tag_field)i));

    CHECK(tags_is_editable(TAG_TITLE) && tags_is_editable(TAG_GENRE));
    CHECK(tags_is_editable(TAG_MUSICBRAINZ_TRACKID) && tags_is_editable(TAG_MUSICBRAINZ_ALBUMID));
    CHECK(!tags_is_editable(TAG_TITLESORT) && !tags_is_editable(TAG_ALBUMSORT));
    CHECK(!tags_is_editable(TAG_ARTISTSORT) && !tags_is_editable(TAG_ALBUMARTISTSORT));
    CHECK(!tags_is_editable(TAG_COMPOSERSORT) && !tags_is_editable(TAG_NAVIDROME_ID));

    int required = 0;
    for (int i = 0; i < TAG_FIELDS; i++)
        required += tags_is_required((enum tag_field)i);
    CHECK(required == 10);
    CHECK(tags_is_required(TAG_COMPILATION) && tags_is_required(TAG_COMPOSER));
    CHECK(!tags_is_required(TAG_BPM) && !tags_is_required(TAG_ISRC));

    const char *music[] = { "mp3", "mp2", "flac", "ogg", "oga", "opus", "m4a", "m4b", "m4p",
                            "mp4", "aac", "wav", "aif", "aiff", "aifc", "afc", "ape", "wv",
                            "tta", "mpc", "spx", "wma", "asf", "shn", "mka", "dsf", "dff",
                            "dsdiff" };
    for (size_t i = 0; i < sizeof music / sizeof music[0]; i++)
        CHECK(tags_is_music(music[i]));
    CHECK(!tags_is_music("MP3")); /* callers lower-case it */
    CHECK(!tags_is_music("jpg"));
    CHECK(!tags_is_music(""));
    CHECK(!tags_is_music("[blank]"));
    CHECK(!tags_is_music("mp"));
    CHECK(!tags_is_music("flacc"));
}

static void test_check(void)
{
    char s[TAGS_MAX_VALUE + 2];

    /* required: absent or empty */
    CHECK(!ok(TAG_TITLE, NULL));
    CHECK(!ok(TAG_TITLE, ""));
    CHECK(strcmp(tags_check(TAG_DATE, &(struct tag_values){ 0, NULL }), "is required") == 0);
    CHECK(ok(TAG_ISRC, NULL));     /* not required */
    CHECK(ok(TAG_BPM, NULL));
    CHECK(ok(TAG_NAVIDROME_ID, NULL));
    CHECK(!ok_list(TAG_GENRE, NULL, 0));
    CHECK(!ok_list(TAG_COMPOSER, NULL, 0));

    /* text: UTF-8 without control characters, at most 500 bytes */
    CHECK(ok(TAG_TITLE, "Ça va – 日本"));
    CHECK(ok(TAG_TITLE, " leading and trailing "));
    CHECK(!ok(TAG_TITLE, "a\nb"));
    CHECK(!ok(TAG_TITLE, "a\tb"));
    CHECK(!ok(TAG_COPYRIGHT, "\xff"));
    CHECK(!ok(TAG_LABEL, "\xc3")); /* cut UTF-8 */
    memset(s, 'a', TAGS_MAX_VALUE);
    s[TAGS_MAX_VALUE] = '\0';
    CHECK(ok(TAG_ALBUM, s));       /* max */
    CHECK(ok(TAG_TITLESORT, s));
    s[TAGS_MAX_VALUE] = 'a';
    s[TAGS_MAX_VALUE + 1] = '\0';
    CHECK(!ok(TAG_ALBUM, s));      /* max + 1 */
    CHECK(!ok(TAG_ALBUMSORT, s));
    CHECK(ok(TAG_ALBUM, "a"));     /* min */

    /* one value only, for single-valued tags */
    const char *two[] = { "a", "b" };
    CHECK(!ok_list(TAG_ARTIST, two, 2));

    /* track and disc number: X/Y, positive, X <= Y, at most 4 digits */
    CHECK(ok(TAG_TRACKNUMBER, "1/1"));
    CHECK(ok(TAG_TRACKNUMBER, "3/12"));
    CHECK(ok(TAG_TRACKNUMBER, "03/12"));
    CHECK(ok(TAG_TRACKNUMBER, "12/12"));
    CHECK(ok(TAG_DISCNUMBER, "9999/9999"));
    CHECK(!ok(TAG_TRACKNUMBER, "13/12"));
    CHECK(!ok(TAG_TRACKNUMBER, "0/12"));
    CHECK(!ok(TAG_TRACKNUMBER, "1/0"));
    CHECK(!ok(TAG_TRACKNUMBER, "3"));
    CHECK(!ok(TAG_TRACKNUMBER, "3/"));
    CHECK(!ok(TAG_TRACKNUMBER, "/3"));
    CHECK(!ok(TAG_TRACKNUMBER, "1/2/3"));
    CHECK(!ok(TAG_TRACKNUMBER, "-1/2"));
    CHECK(!ok(TAG_TRACKNUMBER, " 1/2"));
    CHECK(!ok(TAG_TRACKNUMBER, "1/2 "));
    CHECK(!ok(TAG_TRACKNUMBER, "a/b"));
    CHECK(!ok(TAG_DISCNUMBER, "1/10000"));
    CHECK(!ok(TAG_DISCNUMBER, "00001/2"));
    int x = 0, y = 0;
    CHECK(tags_number("03/12", &x, &y) == 0 && x == 3 && y == 12);
    CHECK(tags_number("2/1", &x, &y) == -1);
    CHECK(tags_number("", &x, &y) == -1);

    /* date: a year; BPM: a positive whole number */
    CHECK(ok(TAG_DATE, "1999"));
    CHECK(ok(TAG_DATE, "1"));
    CHECK(ok(TAG_DATE, "9999"));
    CHECK(!ok(TAG_DATE, "0"));
    CHECK(!ok(TAG_DATE, "10000"));
    CHECK(!ok(TAG_DATE, "1999-01-02"));
    CHECK(!ok(TAG_DATE, "19a9"));
    CHECK(!ok(TAG_DATE, "-1999"));
    CHECK(ok(TAG_BPM, "120"));
    CHECK(ok(TAG_BPM, "0120"));
    CHECK(!ok(TAG_BPM, "0"));
    CHECK(!ok(TAG_BPM, "120.5"));
    CHECK(!ok(TAG_BPM, "fast"));
    CHECK(!ok(TAG_BPM, ""));        /* empty is not "absent" here */

    /* compilation */
    CHECK(ok(TAG_COMPILATION, "0"));
    CHECK(ok(TAG_COMPILATION, "1"));
    CHECK(!ok(TAG_COMPILATION, "2"));
    CHECK(!ok(TAG_COMPILATION, "yes"));
    CHECK(!ok(TAG_COMPILATION, "01"));

    /* genre: lowercase a-z, 0-9 and '-' */
    const char *genres[] = { "rock", "hip-hop", "80s", "-" };
    CHECK(ok_list(TAG_GENRE, genres, 4));
    const char *bad_genres[] = { "Rock", "hip hop", "rock_pop", "électro", "rock;pop", "" };
    for (size_t i = 0; i < sizeof bad_genres / sizeof bad_genres[0]; i++)
        CHECK(!ok_list(TAG_GENRE, &bad_genres[i], 1));
    const char *mixed[] = { "rock", "Pop" };
    CHECK(!ok_list(TAG_GENRE, mixed, 2));

    /* composer: any text, none empty, at most 64 */
    const char *composers[] = { "J. S. Bach", "Händel; and co" };
    CHECK(ok_list(TAG_COMPOSER, composers, 2));
    const char *empty[] = { "Bach", "" };
    CHECK(!ok_list(TAG_COMPOSER, empty, 2));
    const char *many[TAGS_MAX_VALUES + 1];
    for (int i = 0; i <= TAGS_MAX_VALUES; i++)
        many[i] = "x";
    CHECK(ok_list(TAG_COMPOSER, many, TAGS_MAX_VALUES));       /* max */
    CHECK(!ok_list(TAG_COMPOSER, many, TAGS_MAX_VALUES + 1));  /* max + 1 */
}

static void test_prepare(void)
{
    struct tags t;
    char note[TAGS_MAX_NOTE];

    /* the sort tags of changed fields mirror them; others stay */
    memset(&t, 0, sizeof t);
    const char *composers[] = { "Bach", "Händel" };
    t.value[TAG_TITLE] = one("New Title");
    t.value[TAG_TITLESORT] = one("Old, The");
    t.value[TAG_ALBUM] = one("Album");
    t.value[TAG_ALBUMSORT] = one("Album, The");
    t.value[TAG_COMPOSER] = (struct tag_values){ 2, composers };
    t.value[TAG_DISCNUMBER] = one("1/2");
    note[0] = '\0';
    CHECK(tags_prepare(&t, (1u << TAG_TITLE) | (1u << TAG_COMPOSER) | (1u << TAG_ARTIST), note,
                       sizeof note) == 0);
    CHECK_STR(t.value[TAG_TITLESORT].v[0], "New Title");
    CHECK_STR(t.value[TAG_ALBUMSORT].v[0], "Album, The");
    CHECK_STR(t.value[TAG_COMPOSERSORT].v[0], "Bach; Händel");
    CHECK(t.value[TAG_ARTISTSORT].n == 0); /* its source is absent */
    CHECK_STR(t.value[TAG_DISCNUMBER].v[0], "1/2");
    CHECK(note[0] == '\0');

    /* an invalid or missing disc number becomes 1/1 */
    t.value[TAG_DISCNUMBER] = one("2/1");
    CHECK(tags_prepare(&t, 0, note, sizeof note) == 0);
    CHECK_STR(t.value[TAG_DISCNUMBER].v[0], "1/1");
    CHECK(strstr(note, "disc number set to 1/1") != NULL);
    t.value[TAG_DISCNUMBER] = one(NULL);
    note[0] = '\0';
    CHECK(tags_prepare(&t, 0, note, sizeof note) == 0);
    CHECK_STR(t.value[TAG_DISCNUMBER].v[0], "1/1");

    /* COMPOSERSORT over 500 bytes is cut at a character boundary */
    static char a[300], b[300];
    memset(a, 'a', 299);
    for (int i = 0; i + 1 < 299; i += 2)
        memcpy(b + i, "\xc3\xa9", 2); /* "é" */
    b[298] = '\0';
    const char *long_list[] = { a, b };
    t.value[TAG_COMPOSER] = (struct tag_values){ 2, long_list };
    note[0] = '\0';
    CHECK(tags_prepare(&t, 1u << TAG_COMPOSER, note, sizeof note) == 0);
    const char *cs = t.value[TAG_COMPOSERSORT].v[0];
    CHECK(strlen(cs) == 499); /* 299 + 2 + 198: the 500th byte would split an "é" */
    CHECK(tags_check(TAG_COMPOSERSORT, &t.value[TAG_COMPOSERSORT]) == NULL);
    CHECK(strstr(note, "COMPOSERSORT cut to 500 bytes") != NULL);
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

/* The whole file into buf; its length, or -1. */
static long slurp(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -1;
    size_t n = fread(buf, 1, size, f);
    fclose(f);
    return (long)n;
}

static void test_read(void)
{
    char path[512], err[256];
    struct tags t;

    copy_fixture("tagged.mp3", "read.mp3", path, sizeof path);
    CHECK(tags_read(path, &t, NULL, NULL, err, sizeof err) == 0);
    CHECK_STR(t.value[TAG_TITLE].v[0], "Song One");
    CHECK_STR(t.value[TAG_ALBUMARTIST].v[0], "Some Artist");
    CHECK_STR(t.value[TAG_TRACKNUMBER].v[0], "1/2");
    CHECK_STR(t.value[TAG_DATE].v[0], "2001");
    CHECK(t.value[TAG_GENRE].n == 1 && strcmp(t.value[TAG_GENRE].v[0], "Rock") == 0);
    CHECK(t.value[TAG_DISCNUMBER].n == 0);
    CHECK(t.value[TAG_COMPOSER].n == 0);
    CHECK_STR(t.value[TAG_COMPILATION].v[0], "0"); /* absent reads as 0 */
    CHECK(t.npictures == 0);

    /* several values: genre keeps them, a single-valued tag joins them */
    copy_fixture("multi.flac", "read.flac", path, sizeof path);
    CHECK(tags_read(path, &t, NULL, NULL, err, sizeof err) == 0);
    CHECK(t.value[TAG_GENRE].n == 2);
    CHECK_STR(t.value[TAG_GENRE].v[0], "Rock");
    CHECK_STR(t.value[TAG_GENRE].v[1], "Pop");
    CHECK(t.value[TAG_TITLE].n == 1);
    CHECK_STR(t.value[TAG_TITLE].v[0], "Song Two; Other Title");
    CHECK_STR(t.value[TAG_ARTIST].v[0], "Some Artist; Other Artist");

    /* any music extension, any case: TagLib picks the type from it */
    copy_fixture("tagged.flac", "upper.FLAC", path, sizeof path);
    CHECK(tags_read(path, &t, NULL, NULL, err, sizeof err) == 0);
    CHECK_STR(t.value[TAG_TITLE].v[0], "Song Two");

    /* not audio, missing */
    snprintf(path, sizeof path, "%s/text.mp3", dir);
    FILE *f = fopen(path, "w");
    CHECK(f != NULL && fputs("not an mp3\n", f) >= 0 && fclose(f) == 0);
    CHECK(tags_read(path, &t, NULL, NULL, err, sizeof err) == -1);
    snprintf(path, sizeof path, "%s/missing.flac", dir);
    CHECK(tags_read(path, &t, NULL, NULL, err, sizeof err) == -1);
}

static const char picture_data[] = "\xff\xd8\xff\xe0 not really a jpeg";
/* SHA-256 of picture_data (with its NUL) */
#define PICTURE_HASH "12603fef261932ae066a6391abab0cf3a6a339b6ce27e0f5841937323a89c463"

/* Adds a picture to the file through TagLib: n copies, the k-th with the
 * byte k after picture_data, so each is different. */
static void add_pictures(const char *path, int n, const char *description)
{
    TagLib_File *f = taglib_file_new(path);
    CHECK(f != NULL);
    if (f == NULL)
        return;
    for (int k = 0; k < n; k++) {
        char data[sizeof picture_data + 1];
        memcpy(data, picture_data, sizeof picture_data);
        data[sizeof picture_data] = (char)k;
        TAGLIB_COMPLEX_PROPERTY_PICTURE(pic, data, k == 0 ? sizeof picture_data : sizeof data,
                                        description, "image/jpeg",
                                        k == 0 ? "Front Cover" : "Back Cover");
        CHECK(k == 0 ? taglib_complex_property_set(f, "PICTURE", pic)
                     : taglib_complex_property_set_append(f, "PICTURE", pic));
    }
    CHECK(taglib_file_save(f));
    taglib_file_free(f);
}

static void add_picture(const char *path)
{
    add_pictures(path, 1, "test");
}

/* on_picture that counts the pictures and checks it gets their bytes. */
static int count_pictures(void *ctx, const struct tag_picture *p, const unsigned char *data,
                          size_t size)
{
    int *n = ctx;
    CHECK(size >= sizeof picture_data && memcmp(data, picture_data, sizeof picture_data) == 0);
    CHECK(strlen(p->hash) == 64);
    (*n)++;
    return 0;
}

/* on_picture that fails. */
static int refuse_picture(void *ctx, const struct tag_picture *p, const unsigned char *data,
                          size_t size)
{
    (void)ctx;
    (void)p;
    (void)data;
    (void)size;
    return -1;
}

static void test_pictures(void)
{
    char path[512], err[256];
    struct tags t;
    int n = 0;

    /* one picture: its hash, type and description; the bytes to on_picture */
    copy_fixture("tagged.flac", "pic.flac", path, sizeof path);
    add_picture(path);
    CHECK(tags_read(path, &t, count_pictures, &n, err, sizeof err) == 0);
    CHECK(n == 1 && t.npictures == 1);
    CHECK_STR(t.pictures[0].hash, PICTURE_HASH);
    CHECK_STR(t.pictures[0].type, "Front Cover");
    CHECK_STR(t.pictures[0].description, "test");
    CHECK(tags_read(path, &t, refuse_picture, NULL, err, sizeof err) == -1);
    CHECK(strstr(err, "a picture could not be stored") != NULL);

    /* several, in order; at most TAGS_MAX_PICTURES (max, max + 1) */
    copy_fixture("tagged.mp3", "pics.mp3", path, sizeof path);
    add_pictures(path, TAGS_MAX_PICTURES, "");
    n = 0;
    CHECK(tags_read(path, &t, count_pictures, &n, err, sizeof err) == 0);
    CHECK(n == TAGS_MAX_PICTURES && t.npictures == TAGS_MAX_PICTURES);
    CHECK_STR(t.pictures[0].hash, PICTURE_HASH);
    CHECK_STR(t.pictures[1].type, "Back Cover");
    CHECK_STR(t.pictures[1].description, "");
    for (size_t i = 1; i < t.npictures; i++)
        CHECK(strcmp(t.pictures[i].hash, t.pictures[i - 1].hash) != 0);
    add_pictures(path, TAGS_MAX_PICTURES + 1, "");
    n = 0;
    CHECK(tags_read(path, &t, count_pictures, &n, err, sizeof err) == 0);
    CHECK(n == TAGS_MAX_PICTURES && t.npictures == TAGS_MAX_PICTURES);

    /* a description over 500 bytes is cut; one that is not text is "" */
    static char longest[TAGS_MAX_VALUE + 2];
    memset(longest, 'd', TAGS_MAX_VALUE + 1);
    copy_fixture("tagged.flac", "desc.flac", path, sizeof path);
    add_pictures(path, 1, longest);
    CHECK(tags_read(path, &t, NULL, NULL, err, sizeof err) == 0);
    CHECK(t.npictures == 1 && strlen(t.pictures[0].description) == TAGS_MAX_VALUE);
    add_pictures(path, 1, "a\nb");
    CHECK(tags_read(path, &t, NULL, NULL, err, sizeof err) == 0);
    CHECK(t.npictures == 1 && strcmp(t.pictures[0].description, "") == 0);

    /* the same pictures: by their bytes, in order */
    struct tags a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    struct tag_picture pa[2], pb[2];
    memset(pa, 0, sizeof pa);
    memset(pb, 0, sizeof pb);
    memset(pa[0].hash, 'a', 64);
    memset(pa[1].hash, 'b', 64);
    memset(pb[0].hash, 'a', 64);
    memset(pb[1].hash, 'b', 64);
    pa[0].type = "Front Cover";
    pb[0].type = "Other";
    a.pictures = pa;
    b.pictures = pb;
    CHECK(tags_same_pictures(&a, &b)); /* none */
    a.npictures = b.npictures = 2;
    CHECK(tags_same_pictures(&a, &b)); /* the type does not matter */
    b.npictures = 1;
    CHECK(!tags_same_pictures(&a, &b));
    b.npictures = 2;
    pb[1].hash[0] = 'c';
    CHECK(!tags_same_pictures(&a, &b));
}

static void test_write(void)
{
    char path[512], err[256];
    struct tags now, want, t;
    struct stat sb;

    copy_fixture("tagged.mp3", "write.mp3", path, sizeof path);
    CHECK(chmod(path, 0640) == 0);
    CHECK(tags_read(path, &now, NULL, NULL, err, sizeof err) == 0);
    want = now;
    const char *genres[] = { "rock", "pop" }, *composers[] = { "Bach", "Händel" };
    want.value[TAG_TITLE] = one("Song Uno – ñ");
    want.value[TAG_GENRE] = (struct tag_values){ 2, genres };
    want.value[TAG_COMPOSER] = (struct tag_values){ 2, composers };
    want.value[TAG_DISCNUMBER] = one("1/1");
    want.value[TAG_ISRC] = one("USRC17607839");
    want.value[TAG_MUSICBRAINZ_ALBUMID] = one("1b1a7e2c-0000-4000-8000-000000000000");
    want.value[TAG_COMPILATION] = one("1");
    CHECK(tags_write(path, &now, &want, err, sizeof err) == TAGS_WRITTEN);
    CHECK(tags_read(path, &t, NULL, NULL, err, sizeof err) == 0);
    CHECK_STR(t.value[TAG_TITLE].v[0], "Song Uno – ñ");
    CHECK(t.value[TAG_GENRE].n == 2 && strcmp(t.value[TAG_GENRE].v[1], "pop") == 0);
    CHECK(t.value[TAG_COMPOSER].n == 2 && strcmp(t.value[TAG_COMPOSER].v[0], "Bach") == 0);
    CHECK_STR(t.value[TAG_DISCNUMBER].v[0], "1/1");
    CHECK_STR(t.value[TAG_ISRC].v[0], "USRC17607839");
    CHECK_STR(t.value[TAG_COMPILATION].v[0], "1");
    CHECK_STR(t.value[TAG_ARTIST].v[0], "Some Artist"); /* untouched */
    CHECK(stat(path, &sb) == 0 && (sb.st_mode & 0777) == 0640); /* written in place */

    /* remove a tag, keep a picture */
    add_picture(path);
    CHECK(tags_read(path, &now, NULL, NULL, err, sizeof err) == 0 && now.npictures == 1);
    want = now;
    want.value[TAG_ISRC] = one(NULL);
    want.value[TAG_COMPILATION] = one("0");
    CHECK(tags_write(path, &now, &want, err, sizeof err) == TAGS_WRITTEN);
    CHECK(tags_read(path, &t, NULL, NULL, err, sizeof err) == 0);
    CHECK(t.value[TAG_ISRC].n == 0 && t.npictures == 1);
    CHECK_STR(t.pictures[0].hash, PICTURE_HASH);
    CHECK_STR(t.value[TAG_COMPILATION].v[0], "0");

    /* FLAC */
    copy_fixture("tagged.flac", "write.flac", path, sizeof path);
    CHECK(tags_read(path, &now, NULL, NULL, err, sizeof err) == 0);
    want = now;
    want.value[TAG_ALBUM] = one("Other Album");
    want.value[TAG_ALBUMSORT] = one("Other Album");
    CHECK(tags_write(path, &now, &want, err, sizeof err) == TAGS_WRITTEN);
    CHECK(tags_read(path, &t, NULL, NULL, err, sizeof err) == 0);
    CHECK_STR(t.value[TAG_ALBUM].v[0], "Other Album");
    CHECK_STR(t.value[TAG_ALBUMSORT].v[0], "Other Album");
}

/* Refusals leave the file byte for byte as it was. */
static void test_refusals(void)
{
    char path[512], err[256];
    static char a[16384], b[16384];
    struct tags now, want;

    /* the file no longer has what the cache says */
    copy_fixture("tagged.flac", "stale.flac", path, sizeof path);
    long len = slurp(path, a, sizeof a);
    CHECK(tags_read(path, &now, NULL, NULL, err, sizeof err) == 0);
    want = now;
    want.value[TAG_TITLE] = one("New");
    now.value[TAG_DATE] = one("1999");
    CHECK(tags_write(path, &now, &want, err, sizeof err) == TAGS_NOT_WRITTEN);
    CHECK(strstr(err, "changed since it was scanned (DATE)") != NULL);
    now.value[TAG_DATE] = one("2001");
    struct tag_picture pic;
    memset(&pic, 0, sizeof pic);
    memcpy(pic.hash, PICTURE_HASH, sizeof pic.hash);
    now.pictures = &pic;
    now.npictures = 1;
    CHECK(tags_write(path, &now, &want, err, sizeof err) == TAGS_NOT_WRITTEN);
    CHECK(strstr(err, "(the pictures)") != NULL);
    CHECK(slurp(path, b, sizeof b) == len && memcmp(a, b, (size_t)len) == 0);

    /* not audio, missing, a symlink, a folder */
    snprintf(path, sizeof path, "%s/text.mp3", dir);
    CHECK(tags_write(path, &now, &want, err, sizeof err) == TAGS_NOT_WRITTEN);
    snprintf(path, sizeof path, "%s/gone.flac", dir);
    CHECK(tags_write(path, &now, &want, err, sizeof err) == TAGS_NOT_WRITTEN);
    CHECK(strstr(err, "not found") != NULL);
    char link[512];
    copy_fixture("tagged.flac", "target.flac", path, sizeof path);
    snprintf(link, sizeof link, "%s/link.flac", dir);
    CHECK(symlink(path, link) == 0);
    CHECK(tags_write(link, &now, &want, err, sizeof err) == TAGS_NOT_WRITTEN);
    CHECK(strstr(err, "regular") != NULL);
    CHECK(tags_write(dir, &now, &want, err, sizeof err) == TAGS_NOT_WRITTEN);
}

/* A file that does not read back as written: TagLib drops the track number
 * "0/0" on save. */
static void test_read_back(void)
{
    char path[512], err[256];
    struct tags now, want;

    copy_fixture("odd.mp3", "odd.mp3", path, sizeof path);
    CHECK(tags_read(path, &now, NULL, NULL, err, sizeof err) == 0);
    CHECK_STR(now.value[TAG_TRACKNUMBER].v[0], "0/0");
    want = now;
    want.value[TAG_MOOD] = one("calm");
    CHECK(tags_write(path, &now, &want, err, sizeof err) == TAGS_WRITTEN_BAD);
    CHECK(strstr(err, "after saving, TRACKNUMBER reads nothing") != NULL);
}

int main(void)
{
    if (mkdtemp(dir) == NULL || arena_init(1024 * 1024) != 0)
        return 1;

    test_fields();
    test_check();
    test_prepare();
    test_read();
    test_pictures();
    test_write();
    test_refusals();
    test_read_back();

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
