/* The notes app's ids and the names of tags that would clash. */

#include "../src/notes.h"
#include "test.h"

static void test_id_valid(void)
{
    CHECK(notes_id_valid("0123456789abcdef0123456789abcdef"));
    CHECK(notes_id_valid("ffffffffffffffffffffffffffffffff"));
    CHECK(!notes_id_valid(""));
    CHECK(!notes_id_valid("0123456789abcdef0123456789abcde"));   /* 31 */
    CHECK(!notes_id_valid("0123456789abcdef0123456789abcdef0")); /* 33 */
    CHECK(!notes_id_valid("0123456789ABCDEF0123456789abcdef"));  /* upper case */
    CHECK(!notes_id_valid("0123456789abcdeg0123456789abcdef"));  /* not hex */
    CHECK(!notes_id_valid("0123456789abcdef-123456789abcdef"));
    CHECK(!notes_id_valid("0123456789abcdef 123456789abcdef"));
}

static void test_new_id(void)
{
    char a[NOTES_ID_LEN + 1], b[NOTES_ID_LEN + 1];
    CHECK(notes_new_id(a) == 0 && notes_id_valid(a));
    CHECK(notes_new_id(b) == 0 && notes_id_valid(b));
    CHECK(strcmp(a, b) != 0);
}

static void test_suffix_name(void)
{
    char out[NOTES_TAG_MAX + 1];
    CHECK(notes_suffix_name("work", 2, out) == 0);
    CHECK_STR(out, "work 2");
    CHECK(notes_suffix_name("work", 99, out) == 0);
    CHECK_STR(out, "work 99");
    CHECK(notes_suffix_name("", 2, out) == 0);
    CHECK_STR(out, " 2");
    CHECK(notes_suffix_name("work", 1, out) == -1);
    CHECK(notes_suffix_name("work", 100, out) == -1);

    /* 48 bytes + " 2" is exactly the maximum: nothing cut */
    const char *n48 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    CHECK(notes_suffix_name(n48, 2, out) == 0);
    CHECK(strlen(out) == NOTES_TAG_MAX && strcmp(out + 48, " 2") == 0);
    /* 50 bytes: cut to 48, or 47 for a two-digit number */
    const char *n50 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    CHECK(notes_suffix_name(n50, 2, out) == 0);
    CHECK(strlen(out) == NOTES_TAG_MAX && strncmp(out, n50, 48) == 0);
    CHECK(notes_suffix_name(n50, 10, out) == 0);
    CHECK(strlen(out) == NOTES_TAG_MAX && strcmp(out + 47, " 10") == 0);

    /* a character of several bytes across the cut goes whole */
    char n[NOTES_TAG_MAX + 1];
    memset(n, 'c', 47);
    memcpy(n + 47, "\xc3\xa9", 3); /* 47 bytes + "é": 49 bytes */
    CHECK(notes_suffix_name(n, 2, out) == 0);
    CHECK(strlen(out) == 49 && strncmp(out, n, 47) == 0 && strcmp(out + 47, " 2") == 0);
    memset(n, 'd', 46);
    memcpy(n + 46, "\xf0\x9f\x8c\xb1", 5); /* 46 bytes + a 4-byte emoji: 50 */
    CHECK(notes_suffix_name(n, 2, out) == 0);
    CHECK(strlen(out) == 48 && strcmp(out + 46, " 2") == 0);
}

int main(void)
{
    test_id_valid();
    test_new_id();
    test_suffix_name();
    TEST_DONE();
}
