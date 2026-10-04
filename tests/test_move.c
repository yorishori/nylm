/* The naming rule of the move service: move_target(). */

#include "../src/move.h"
#include "../src/tags.h"
#include "test.h"

static char out[TAGS_MAX_PATH];

/* The target of a track with these tags (ext "flac"), or its problem. */
static const char *name(const char *artist, const char *album, const char *title,
                        const char *track, const char *disc)
{
    const char *why = move_target(artist, album, title, track, disc, "flac", out, sizeof out);
    return why != NULL ? why : out;
}

/* s repeated to n bytes. */
static const char *repeat(const char *s, size_t n)
{
    static char buf[2048];
    size_t len = strlen(s);
    for (size_t i = 0; i < n; i++)
        buf[i] = s[i % len];
    buf[n] = '\0';
    return buf;
}

static void test_names(void)
{
    CHECK_STR(name("Pink Floyd", "The Wall", "Hey You", "3/12", "1/1"),
              "Pink Floyd/The Wall/03 - Hey You.flac");
    /* No disc number: one disc. */
    CHECK_STR(name("A", "B", "T", "3/12", NULL), "A/B/03 - T.flac");
    CHECK_STR(name("A", "B", "T", "3/12", ""), "A/B/03 - T.flac");
    /* Several discs. */
    CHECK_STR(name("A", "B", "T", "3/12", "2/2"), "A/B/2-03 - T.flac");
    CHECK_STR(name("A", "B", "T", "3/12", "10/12"), "A/B/10-03 - T.flac");
    /* Padding: at least two digits, else those of the total. */
    CHECK_STR(name("A", "B", "T", "1/1", NULL), "A/B/01 - T.flac");
    CHECK_STR(name("A", "B", "T", "9/99", NULL), "A/B/09 - T.flac");
    CHECK_STR(name("A", "B", "T", "7/100", NULL), "A/B/007 - T.flac");
    CHECK_STR(name("A", "B", "T", "100/100", NULL), "A/B/100 - T.flac");
    /* The extension as given. */
    CHECK(move_target("A", "B", "T", "1/2", NULL, "mp3", out, sizeof out) == NULL);
    CHECK_STR(out, "A/B/01 - T.mp3");
    /* UTF-8 is kept. */
    CHECK_STR(name("結束バンド", "結束バンド", "ギターと孤独と蒼い惑星", "1/14", NULL),
              "結束バンド/結束バンド/01 - ギターと孤独と蒼い惑星.flac");
}

static void test_unsafe(void)
{
    CHECK_STR(name("AC/DC", "Back: In Black?", "What \"*<>|\\ now", "1/2", NULL),
              "AC_DC/Back_ In Black_/01 - What ______ now.flac");
    CHECK_STR(name("A\tB", "C\x7f", "T\x01", "1/2", NULL), "A_B/C_/01 - T_.flac");
    /* Spaces around and dots at the end go; a leading dot (a hidden file,
     * which the scan skips) becomes '_'. */
    CHECK_STR(name("  A  ", "Album...", " . T . ", "1/2", NULL), "A/Album/01 - _ T.flac");
    CHECK_STR(name(".hidden", "..", "...", "1/2", NULL), "_hidden/_/01 - _.flac");
    CHECK_STR(name("A", " ", "T", "1/2", NULL), "A/_/01 - T.flac");
}

static void test_long(void)
{
    char want[1024];
    /* A part of 255 bytes is kept; 256 is cut to 255. */
    CHECK(move_target(repeat("a", 255), "B", "T", "1/2", NULL, "flac", out, sizeof out) == NULL);
    snprintf(want, sizeof want, "%s/B/01 - T.flac", repeat("a", 255));
    CHECK_STR(out, want);
    CHECK(move_target(repeat("a", 256), "B", "T", "1/2", NULL, "flac", out, sizeof out) == NULL);
    CHECK_STR(out, want);
    /* The file name keeps its number and extension: 255 bytes in all. */
    CHECK(move_target("A", "B", repeat("t", 300), "1/2", NULL, "flac", out, sizeof out) == NULL);
    CHECK(strlen(strrchr(out, '/') + 1) == 255);
    CHECK(strncmp(strrchr(out, '/') + 1, "01 - ttt", 8) == 0);
    CHECK(strcmp(out + strlen(out) - 6, "t.flac") == 0);
    /* A cut never splits a character ("é" is 2 bytes): 254 bytes, not 255. */
    CHECK(move_target(repeat("é", 300), "B", "T", "1/2", NULL, "flac", out, sizeof out) == NULL);
    CHECK(strchr(out, '/') - out == 254);
    /* A cut that leaves a space or dot at the end drops it. */
    CHECK(move_target(repeat("abcd ", 300), "B", "T", "1/2", NULL, "flac", out, sizeof out) == NULL);
    CHECK(strchr(out, '/') - out == 254);
    /* The whole path must fit out. */
    char small[8];
    CHECK_STR(move_target("A", "B", "T", "1/2", NULL, "flac", small, sizeof small),
              "its path would be too long");
    CHECK(move_target("A", "B", "T", "1/2", NULL, "flac", out, strlen("A/B/01 - T.flac") + 1) == NULL);
    CHECK(move_target("A", "B", "T", "1/2", NULL, "flac", out, strlen("A/B/01 - T.flac")) != NULL);
}

static void test_rejected(void)
{
    CHECK_STR(name(NULL, "B", "T", "1/2", NULL), "it has no album artist");
    CHECK_STR(name("", "B", "T", "1/2", NULL), "it has no album artist");
    CHECK_STR(name("A", NULL, "T", "1/2", NULL), "it has no album");
    CHECK_STR(name("A", "", "T", "1/2", NULL), "it has no album");
    CHECK_STR(name("A", "B", NULL, "1/2", NULL), "it has no title");
    CHECK_STR(name("A", "B", "", "1/2", NULL), "it has no title");
    CHECK_STR(name("A", "B", "T", NULL, NULL), "its track number is not X/Y");
    CHECK_STR(name("A", "B", "T", "", NULL), "its track number is not X/Y");
    CHECK_STR(name("A", "B", "T", "3", NULL), "its track number is not X/Y");
    CHECK_STR(name("A", "B", "T", "3/2", NULL), "its track number is not X/Y");
    CHECK_STR(name("A", "B", "T", "0/2", NULL), "its track number is not X/Y");
    CHECK_STR(name("A", "B", "T", "1/2", "2"), "its disc number is not X/Y");
    CHECK_STR(name("A", "B", "T", "1/2", "3/2"), "its disc number is not X/Y");
    CHECK_STR(move_target("A", "B", "T", "1/2", NULL, "", out, sizeof out),
              "its file has no music extension");
    CHECK_STR(move_target("A", "B", "T", "1/2", NULL, NULL, out, sizeof out),
              "its file has no music extension");
    CHECK_STR(move_target("A", "B", "T", "1/2", NULL, repeat("x", TAGS_MAX_EXT + 1), out,
                          sizeof out), "its file has no music extension");
    CHECK(move_target("A", "B", "T", "1/2", NULL, repeat("x", TAGS_MAX_EXT), out, sizeof out) ==
          NULL);
}

int main(void)
{
    test_names();
    test_unsafe();
    test_long();
    test_rejected();
    TEST_DONE();
}
