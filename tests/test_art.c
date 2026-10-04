/* Album art files: hashes, picture types, base64, and the folder (saving,
 * opening, loading, sweeping) in a temporary data folder. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/art.h"
#include "test.h"

#define H1 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" /* "abc" */
#define H2 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" /* "" */
#define H3 "0000000000000000000000000000000000000000000000000000000000000003"

static char dir[] = "/tmp/nylm-art-XXXXXX";
static char art_dir[64];

static void test_hash(void)
{
    char h[ART_HASH_LEN + 1];
    CHECK(art_hash((const unsigned char *)"abc", 3, h) == 0);
    CHECK_STR(h, H1);
    CHECK(art_hash((const unsigned char *)"", 0, h) == 0);
    CHECK_STR(h, H2);

    CHECK(art_hash_valid(H1));
    CHECK(art_hash_valid(H3));
    CHECK(!art_hash_valid(""));
    CHECK(!art_hash_valid(H1 "0"));                     /* 65 */
    CHECK(!art_hash_valid(&H1[1]));                     /* 63 */
    CHECK(!art_hash_valid("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"));
    CHECK(!art_hash_valid("g000000000000000000000000000000000000000000000000000000000000000"));
    CHECK(!art_hash_valid("../00000000000000000000000000000000000000000000000000000000000000"));
}

static void test_mime(void)
{
    const unsigned char jpeg[] = { 0xff, 0xd8, 0xff, 0xe0 };
    CHECK_STR(art_mime(jpeg, 4), "image/jpeg");
    CHECK_STR(art_mime(jpeg, 3), "image/jpeg");
    CHECK(art_mime(jpeg, 2) == NULL);
    CHECK_STR(art_mime((const unsigned char *)"\x89PNG\r\n\x1a\n....", 12), "image/png");
    CHECK(art_mime((const unsigned char *)"\x89PNG\r\n\x1a", 7) == NULL);
    CHECK_STR(art_mime((const unsigned char *)"GIF87a", 6), "image/gif");
    CHECK_STR(art_mime((const unsigned char *)"GIF89a....", 10), "image/gif");
    CHECK(art_mime((const unsigned char *)"GIF88a", 6) == NULL);
    CHECK_STR(art_mime((const unsigned char *)"RIFF\1\0\0\0WEBPVP8 ", 16), "image/webp");
    CHECK(art_mime((const unsigned char *)"RIFF\1\0\0\0WAVEfmt ", 16) == NULL);
    CHECK(art_mime((const unsigned char *)"<svg>", 5) == NULL);
    CHECK(art_mime((const unsigned char *)"", 0) == NULL);
}

/* Decodes s with room for max bytes; the result, and the bytes in out. */
static long dec(const char *s, size_t max, unsigned char *out)
{
    return art_base64_decode(s, strlen(s), out, max);
}

static void test_base64(void)
{
    unsigned char out[16];
    CHECK(dec("", 16, out) == 0);
    CHECK(dec("TQ==", 16, out) == 1 && out[0] == 'M');
    CHECK(dec("TWE=", 16, out) == 2 && memcmp(out, "Ma", 2) == 0);
    CHECK(dec("TWFu", 16, out) == 3 && memcmp(out, "Man", 3) == 0);
    CHECK(dec("TWFuTWFu", 16, out) == 6 && memcmp(out, "ManMan", 6) == 0);
    CHECK(dec("+/+/", 16, out) == 3 && out[0] == 0xfb && out[1] == 0xff && out[2] == 0xbf);
    CHECK(dec("AAAA", 16, out) == 3 && out[0] == 0 && out[2] == 0);

    CHECK(dec("TQ=", 16, out) == -1);    /* not a multiple of 4 */
    CHECK(dec("TWFuT", 16, out) == -1);
    CHECK(dec("T===", 16, out) == -1);   /* too much padding */
    CHECK(dec("====", 16, out) == -1);
    CHECK(dec("TQ=a", 16, out) == -1);   /* '=' inside */
    CHECK(dec("TQ==TWFu", 16, out) == -1);
    CHECK(dec("TR==", 16, out) == -1);   /* unused bits set */
    CHECK(dec("TWF=", 16, out) == -1);
    CHECK(dec("TW u", 16, out) == -1);   /* space */
    CHECK(dec("TWF\n", 16, out) == -1);  /* line break */
    CHECK(dec("TW-_", 16, out) == -1);   /* URL-safe alphabet */
    CHECK(dec("TWF\xc3", 16, out) == -1);

    CHECK(dec("TWFuTWFu", 6, out) == 6); /* max */
    CHECK(dec("TWFuTWFu", 5, out) == -2); /* max + 1 */
    CHECK(dec("TWE=", 2, out) == 2);
    CHECK(dec("TWE=", 1, out) == -2);
    CHECK(dec("", 0, out) == 0);
}

/* The bytes of file name in the art folder into buf; its length or -1. */
static long file_bytes(const char *name, char *buf, size_t size)
{
    char path[256];
    snprintf(path, sizeof path, "%s/%s", art_dir, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -1;
    size_t n = fread(buf, 1, size, f);
    fclose(f);
    return (long)n;
}

static int exists(const char *name)
{
    char path[256];
    struct stat sb;
    snprintf(path, sizeof path, "%s/%s", art_dir, name);
    return lstat(path, &sb) == 0;
}

static void touch(const char *name)
{
    char path[256];
    snprintf(path, sizeof path, "%s/%s", art_dir, name);
    FILE *f = fopen(path, "w");
    CHECK(f != NULL && fclose(f) == 0);
}

/* sweep's keep: only H1. */
static int keep_h1(const char *hash, void *ctx)
{
    (*(int *)ctx)++;
    return strcmp(hash, H1) == 0;
}

static int keep_error(const char *hash, void *ctx)
{
    (void)hash;
    (void)ctx;
    return -1;
}

static void test_files(void)
{
    char buf[64];
    unsigned char got[8];
    CHECK(mkdtemp(dir) != NULL);
    char music[64];
    snprintf(music, sizeof music, "%s/music", dir);
    CHECK(mkdir(music, 0700) == 0);
    snprintf(art_dir, sizeof art_dir, "%s/music/art", dir);

    /* not configured yet: nothing works */
    CHECK(art_save(ART_MUSIC, H1, 0, (const unsigned char *)"abc", 3) == -1);
    static char longest[5000];
    memset(longest, 'a', sizeof longest - 1);
    CHECK(art_configure(longest) == -1);
    CHECK(art_configure(dir) == 0);

    /* save makes the folder (700), then the file; saving again keeps it */
    CHECK(art_save(ART_MUSIC, H1, 0, (const unsigned char *)"abc", 3) == 0);
    struct stat sb;
    CHECK(stat(art_dir, &sb) == 0 && (sb.st_mode & 0777) == 0700);
    CHECK(file_bytes(H1, buf, sizeof buf) == 3 && memcmp(buf, "abc", 3) == 0);
    CHECK(art_save(ART_MUSIC, H1, 0, (const unsigned char *)"xyz", 3) == 0);
    CHECK(file_bytes(H1, buf, sizeof buf) == 3 && memcmp(buf, "abc", 3) == 0);
    CHECK(art_save(ART_MUSIC, H1, 1, (const unsigned char *)"thumb", 5) == 0);
    CHECK(file_bytes(H1 ".thumb", buf, sizeof buf) == 5);
    CHECK(art_save(ART_MUSIC, H2, 0, (const unsigned char *)"", 0) == 0);
    CHECK(file_bytes(H2, buf, sizeof buf) == 0);
    CHECK(art_save(ART_MUSIC, "../x", 0, (const unsigned char *)"abc", 3) == -1);
    CHECK(!exists(".tmp-" H1));

    /* Each store has its own folder: the plants' photos are not album art. */
    char plants[64], photo[160];
    snprintf(plants, sizeof plants, "%s/plants", dir);
    CHECK(mkdir(plants, 0700) == 0);
    CHECK(art_open(ART_PLANTS, H1, 0) == -1 && errno == ENOENT);
    CHECK(art_save(ART_PLANTS, H3, 0, (const unsigned char *)"leaf", 4) == 0);
    snprintf(photo, sizeof photo, "%s/plants/photos/%s", dir, H3);
    CHECK(stat(photo, &sb) == 0 && sb.st_size == 4);
    CHECK(!exists(H3));
    CHECK(art_load(ART_PLANTS, H3, got, sizeof got) == 4);
    CHECK(unlink(photo) == 0);
    *strrchr(photo, '/') = '\0';
    CHECK(rmdir(photo) == 0 && rmdir(plants) == 0);

    /* open: the file or its thumbnail; missing ENOENT */
    int fd = art_open(ART_MUSIC, H1, 0);
    CHECK(fd >= 0 && read(fd, buf, sizeof buf) == 3);
    if (fd >= 0)
        close(fd);
    fd = art_open(ART_MUSIC, H1, 1);
    CHECK(fd >= 0 && read(fd, buf, sizeof buf) == 5 && memcmp(buf, "thumb", 5) == 0);
    if (fd >= 0)
        close(fd);
    errno = 0;
    CHECK(art_open(ART_MUSIC, H2, 1) == -1 && errno == ENOENT);
    CHECK(art_open(ART_MUSIC, H3, 0) == -1 && errno == ENOENT);
    CHECK(art_open(ART_MUSIC, "../../etc/passwd", 0) == -1 && errno == ENOENT);
    CHECK(art_open(ART_MUSIC, H1 "0", 0) == -1);

    /* a symlink is never followed */
    char link[256];
    snprintf(link, sizeof link, "%s/%s", art_dir, H3);
    CHECK(symlink("/etc/passwd", link) == 0);
    CHECK(art_open(ART_MUSIC, H3, 0) == -1);
    CHECK(art_load(ART_MUSIC, H3, got, sizeof got) == -1);

    /* load: the whole file, at most max bytes */
    CHECK(art_load(ART_MUSIC, H1, got, 3) == 3 && memcmp(got, "abc", 3) == 0); /* max */
    CHECK(art_load(ART_MUSIC, H1, got, 2) == -2);                               /* max + 1 */
    CHECK(art_load(ART_MUSIC, H2, got, sizeof got) == 0);
    CHECK(art_load(ART_MUSIC, H2, got, 0) == 0);
    CHECK(art_load(ART_MUSIC, "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", got,
                   sizeof got) == -1);

    /* sweep: drops what keep() refuses and temporary files, leaves other
     * names (and the symlink, which is not a picture: H3 is refused) */
    touch("notes.txt");
    touch(H1 ".jpg");
    CHECK(art_sweep(ART_MUSIC, keep_error, NULL) == -1);
    touch(".tmp-" H2);
    int asked = 0;
    CHECK(art_sweep(ART_MUSIC, keep_h1, &asked) == 3); /* H2, H3 (the link), .tmp-H2 */
    CHECK(asked == 4);                      /* H1, H1.thumb, H2, H3 */
    CHECK(exists(H1) && exists(H1 ".thumb") && exists("notes.txt") && exists(H1 ".jpg"));
    CHECK(!exists(H2) && !exists(H3) && !exists(".tmp-" H2));
    CHECK(stat("/etc/passwd", &sb) == 0);

    /* clean up */
    const char *left[] = { H1, H1 ".thumb", "notes.txt", H1 ".jpg" };
    for (size_t i = 0; i < sizeof left / sizeof left[0]; i++) {
        char path[256];
        snprintf(path, sizeof path, "%s/%s", art_dir, left[i]);
        CHECK(unlink(path) == 0);
    }
    CHECK(rmdir(art_dir) == 0 && rmdir(music) == 0 && rmdir(dir) == 0);
}

int main(void)
{
    test_hash();
    test_mime();
    test_base64();
    test_files();
    TEST_DONE();
}
