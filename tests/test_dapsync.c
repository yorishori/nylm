#define _XOPEN_SOURCE 700

#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "../src/arena.h"
#include "../src/dapsync.h"
#include "test.h"

static char root[] = "/tmp/nylm-test-dapsync-XXXXXX";

/* root/rel */
static const char *at(const char *rel)
{
    static char buf[4][1024];
    static int i;
    i = (i + 1) % 4;
    snprintf(buf[i], sizeof buf[i], "%s/%s", root, rel);
    return buf[i];
}

/* Writes text to root/rel, making its folders, with time mtime (0: now). */
static void put(const char *rel, const char *text, long long mtime)
{
    char path[1024];
    snprintf(path, sizeof path, "%s", at(rel));
    for (char *p = strchr(path + strlen(root) + 1, '/'); p != NULL; p = strchr(p + 1, '/')) {
        *p = '\0';
        mkdir(path, 0755);
        *p = '/';
    }
    FILE *f = fopen(path, "w");
    if (f != NULL) {
        fputs(text, f);
        fclose(f);
    }
    if (mtime > 0) {
        const struct timespec t[2] = { { (time_t)mtime, 0 }, { (time_t)mtime, 0 } };
        utimensat(AT_FDCWD, path, t, 0);
    }
}

/* The text of root/rel ("" if it can not be read). */
static const char *text_at(const char *rel)
{
    static char buf[4096];
    buf[0] = '\0';
    FILE *f = fopen(at(rel), "r");
    if (f != NULL) {
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = '\0';
        fclose(f);
    }
    return buf;
}

static int exists(const char *rel)
{
    struct stat sb;
    return lstat(at(rel), &sb) == 0;
}

static int remove_one(const char *path, const struct stat *sb, int type, struct FTW *ftw)
{
    (void)sb;
    (void)ftw;
    return type == FTW_DP ? rmdir(path) : unlink(path);
}

static void test_dest_path(void)
{
    char out[600];
    CHECK(dap_dest_path("Artist/Album/01 - Title.flac", out, sizeof out) == 0);
    CHECK_STR(out, "Artist/Album/01 - Title.flac");
    /* What FAT refuses becomes _, no dot or space at the ends. */
    CHECK(dap_dest_path("Inbox/What?: \"live\" <1>|*/a*b?.mp3", out, sizeof out) == 0);
    CHECK_STR(out, "Inbox/What__ _live_ _1___/a_b_.mp3");
    CHECK(dap_dest_path("Inbox/.hidden/Album. /x.flac", out, sizeof out) == 0);
    CHECK_STR(out, "Inbox/_hidden/Album/x.flac");
    CHECK(dap_dest_path("a/back\\slash.FLAC", out, sizeof out) == 0);
    CHECK_STR(out, "a/back_slash.FLAC");
    /* A long file name is cut, its extension kept. */
    char rel[400], want[300];
    snprintf(rel, sizeof rel, "a/%0300d.flac", 0);
    CHECK(dap_dest_path(rel, out, sizeof out) == 0);
    snprintf(want, sizeof want, "a/%0250d.flac", 0);
    CHECK_STR(out, want);
    /* A name with no extension, or a dot first. */
    CHECK(dap_dest_path("a/README", out, sizeof out) == 0);
    CHECK_STR(out, "a/README");
    CHECK(dap_dest_path("a/.flac", out, sizeof out) == 0);
    CHECK_STR(out, "a/_flac");
    /* Not a relative path of names. */
    CHECK(dap_dest_path("/a/b.flac", out, sizeof out) == -1);
    CHECK(dap_dest_path("a/../b.flac", out, sizeof out) == -1);
    CHECK(dap_dest_path("a//b.flac", out, sizeof out) == -1);
    CHECK(dap_dest_path("./b.flac", out, sizeof out) == -1);
    CHECK(dap_dest_path("a/", out, sizeof out) == -1);
    CHECK(dap_dest_path("", out, sizeof out) == -1);
    CHECK(dap_dest_path("a\n/b", out, sizeof out) == -1);
    CHECK(dap_dest_path(NULL, out, sizeof out) == -1);
    /* It fits, or it fails. */
    CHECK(dap_dest_path("ab/cd.flac", out, 11) == 0);
    CHECK(dap_dest_path("ab/cd.flac", out, 10) == -1);
}

static void test_fold(void)
{
    char out[32];
    dap_fold("Artist/ALBUM/Ünïcode.FLAC", out, sizeof out);
    CHECK_STR(out, "artist/album/Ünïcode.flac");
    dap_fold("ABCDEF", out, 4);
    CHECK_STR(out, "abc");
}

static void test_m3u8(void)
{
    const struct dap_song songs[] = {
        { "Artist/Album/01 - One.flac", "Artist", "One", 180 },
        { "Other/x.mp3", NULL, "Line\nbreak", -1 },
        { "Third/y.mp3", "Only artist", NULL, 5 },
    };
    const char *text = dap_m3u8(songs, 3);
    CHECK_STR(text, "#EXTM3U\n"
                    "#EXTINF:180,Artist - One\nArtist/Album/01 - One.flac\n"
                    "#EXTINF:-1,Line break\nOther/x.mp3\n"
                    "#EXTINF:5,Only artist\nThird/y.mp3\n");
    CHECK_STR(dap_m3u8(songs, 0), "#EXTM3U\n");
}

/* What the report saw. */
struct seen {
    int copied, kept, deleted, failed, stop_after;
    char failures[2000];
};

static int report(const struct dap_progress *p, const char *what, const char *path,
                  const char *why, void *ctx)
{
    struct seen *s = ctx;
    (void)p;
    s->copied += strcmp(what, "copied") == 0;
    s->kept += strcmp(what, "kept") == 0;
    s->deleted += strcmp(what, "deleted") == 0;
    s->failed += strcmp(what, "failed") == 0;
    if (why != NULL) {
        size_t len = strlen(s->failures);
        snprintf(s->failures + len, sizeof s->failures - len, "%s: %s\n", path, why);
    }
    return s->stop_after > 0 && s->copied >= s->stop_after ? -1 : 0;
}

static void test_apply(void)
{
    if (mkdtemp(root) == NULL) {
        CHECK(0);
        return;
    }
    /* The library, and the DAP's folder as a sync finds it. */
    put("lib/A/01.flac", "one", 1700000000);
    put("lib/A/02.mp3", "two two", 1700000100);
    put("lib/B/03.flac", "three", 1700000200);
    put("lib/A/cover.jpg", "jpeg", 1700000300);
    put("outside/keep.txt", "not on the DAP", 0);
    put("dap/Music/Artist/Album/01.flac", "one", 1700000001); /* same, within 2 s */
    put("dap/Music/Artist/Album/02.mp3", "old", 1700000100);  /* differs: copied again */
    put("dap/Music/Old/gone.flac", "old", 0);                 /* not selected: deleted */
    put("dap/Music/Artist/Album/" DAP_PART, "half", 0);       /* a cut-off copy */
    mkdir(at("dap/Music/Empty"), 0755);
    put("dap/Other/mine.mp3", "outside the folder", 0);       /* never touched */
    if (symlink(at("outside"), at("dap/Music/Link")) != 0)
        perror("symlink");

    char s1[1024], s2[1024], s3[1024], s4[1024];
    snprintf(s1, sizeof s1, "%s", at("lib/A/01.flac"));
    snprintf(s2, sizeof s2, "%s", at("lib/A/02.mp3"));
    snprintf(s3, sizeof s3, "%s", at("lib/B/03.flac"));
    snprintf(s4, sizeof s4, "%s", at("lib/A/cover.jpg"));
    struct dap_file files[] = {
        { s1, "Artist/Album/01.flac", 3, 1700000000 },
        { s2, "Artist/Album/02.mp3", 7, 1700000100 },
        { s3, "New/03.flac", 5, 1700000200 },
        { s4, "artist/ALBUM/01.FLAC", 4, 1700000300 }, /* the same name on FAT */
        { s4, "Link", 4, 1700000300 },                 /* a link's name: the link goes */
        { s4, "../escape.flac", 4, 1700000300 },       /* out of the folder: refused */
    };
    struct dap_text texts[] = { { "Road.m3u8", "#EXTM3U\nNew/03.flac\n" } };
    struct dap_plan plan = { files, 6, texts, 1 };
    int dir = open(at("dap/Music"), O_RDONLY | O_DIRECTORY);
    struct dap_progress p;
    struct seen seen = { 0 };
    char err[300] = "";
    CHECK(dap_apply(dir, &plan, 1, &p, report, &seen, err, sizeof err) == 0);
    CHECK_STR(err, "");
    CHECK(p.kept == 1 && seen.kept == 1);
    CHECK(p.copied == 4 && seen.copied == 4); /* 02, 03, over the link, the playlist */
    CHECK(p.deleted == 2 && seen.deleted == 2); /* gone.flac, the cut-off copy */
    CHECK(p.failed == 2 && seen.failed == 2);
    CHECK(p.bytes == 16 && p.total == 20); /* the refused one was to be copied too */
    CHECK(strstr(seen.failures, "artist/ALBUM/01.FLAC: its name on the DAP is that of another "
                                "file") != NULL);
    CHECK(strstr(seen.failures, "../escape.flac: can not make its folder") != NULL);
    CHECK_STR(text_at("dap/Music/Artist/Album/01.flac"), "one");
    CHECK_STR(text_at("dap/Music/Artist/Album/02.mp3"), "two two");
    CHECK_STR(text_at("dap/Music/New/03.flac"), "three");
    CHECK_STR(text_at("dap/Music/Road.m3u8"), "#EXTM3U\nNew/03.flac\n");
    struct stat sb;
    CHECK(stat(at("dap/Music/New/03.flac"), &sb) == 0 && sb.st_mtime == 1700000200);
    CHECK(!exists("dap/Music/Old/gone.flac") && !exists("dap/Music/Old"));
    CHECK(!exists("dap/Music/Empty"));
    CHECK(!exists("dap/Music/Artist/Album/" DAP_PART));
    /* The link itself was replaced, never followed. */
    CHECK(lstat(at("dap/Music/Link"), &sb) == 0 && S_ISREG(sb.st_mode));
    CHECK_STR(text_at("dap/Music/Link"), "jpeg");
    /* Nothing outside the folder changed. */
    CHECK_STR(text_at("outside/keep.txt"), "not on the DAP");
    CHECK(!exists("outside/x.flac") && !exists("dap/escape.flac"));
    CHECK_STR(text_at("dap/Other/mine.mp3"), "outside the folder");
    CHECK_STR(text_at("lib/A/02.mp3"), "two two");

    /* Again: everything is there. */
    struct dap_plan same = { files, 3, texts, 1 };
    memset(&seen, 0, sizeof seen);
    CHECK(dap_apply(dir, &same, 1, &p, report, &seen, err, sizeof err) == 0);
    CHECK(p.kept == 4 && p.copied == 0 && p.deleted == 1 && p.failed == 0 && p.total == 0);
    /* A playlist that changed is written again. */
    struct dap_text other[] = { { "Road.m3u8", "#EXTM3U\n" } };
    struct dap_plan changed = { files, 3, other, 1 };
    CHECK(dap_apply(dir, &changed, 1, &p, report, &seen, err, sizeof err) == 0);
    CHECK(p.kept == 3 && p.copied == 1);
    CHECK_STR(text_at("dap/Music/Road.m3u8"), "#EXTM3U\n");

    /* A source gone: it fails, the rest goes on. */
    put("lib/B/04.flac", "four", 1700000400);
    char s5[1024];
    snprintf(s5, sizeof s5, "%s", at("lib/B/04.flac"));
    struct dap_file more[] = { files[0], files[1], files[2], { s5, "New/04.flac", 4, 1700000400 },
                               { at("lib/nope.flac"), "New/nope.flac", 1, 1 } };
    struct dap_plan with_more = { more, 5, NULL, 0 };
    memset(&seen, 0, sizeof seen);
    CHECK(dap_apply(dir, &with_more, 1, &p, report, &seen, err, sizeof err) == 0);
    CHECK(p.copied == 1 && p.failed == 1 && p.deleted == 1); /* Road.m3u8 is not planned */
    CHECK(strstr(seen.failures, "New/nope.flac: can not read") != NULL);

    /* 4 GiB or more: FAT fails it (and needs no room for it); exFAT does not. */
    struct dap_file big[] = { files[0], { s5, "Big/big.flac", DAP_FAT_LIMIT + 1, 1 } };
    struct dap_plan with_big = { big, 2, NULL, 0 };
    memset(&seen, 0, sizeof seen);
    CHECK(dap_apply(dir, &with_big, 1, &p, report, &seen, err, sizeof err) == 0);
    CHECK(p.failed == 1 && strstr(seen.failures, "4 GiB or more: FAT can not hold it") != NULL);

    /* Not enough room: what is not planned goes, nothing is copied. */
    struct statvfs vs;
    CHECK(fstatvfs(dir, &vs) == 0);
    long long room = (long long)vs.f_bavail * (long long)vs.f_frsize;
    struct dap_file huge[] = { { s5, "Huge/h.flac", room + 1000000, 1 } };
    struct dap_plan with_huge = { huge, 1, NULL, 0 };
    memset(&seen, 0, sizeof seen);
    CHECK(dap_apply(dir, &with_huge, 0, &p, report, &seen, err, sizeof err) == -1);
    CHECK(strstr(err, "take something off the selection") != NULL);
    CHECK(p.copied == 0 && !exists("dap/Music/Huge/h.flac"));
    CHECK(p.deleted > 0 && !exists("dap/Music/New/03.flac"));

    /* The report says stop. */
    struct dap_plan again = { files, 3, NULL, 0 };
    memset(&seen, 0, sizeof seen);
    seen.stop_after = 1;
    CHECK(dap_apply(dir, &again, 1, &p, report, &seen, err, sizeof err) == -1);
    CHECK_STR(err, "stopped");
    close(dir);
    CHECK(nftw(root, remove_one, 16, FTW_DEPTH | FTW_PHYS) == 0);
}

int main(void)
{
    if (arena_init(1 << 24) != 0)
        return 1;
    test_dest_path();
    test_fold();
    test_m3u8();
    test_apply();
    TEST_DONE();
}
