/* Keys for possible duplicates in the music library (src/dupes.h). */
#include <string.h>

#include "dupes.h"

/* Latin letters U+00C0..U+017F as ASCII, in runs: an entry covers the code
 * points after the previous entry's up to last. "" drops the sign (× ÷). */
static const struct {
    unsigned last;
    const char *ascii;
} latin[] = {
    { 0xc5, "a" },  { 0xc6, "ae" }, { 0xc7, "c" },   { 0xcb, "e" },  { 0xcf, "i" },
    { 0xd0, "d" },  { 0xd1, "n" },  { 0xd6, "o" },   { 0xd7, "" },   { 0xd8, "o" },
    { 0xdc, "u" },  { 0xdd, "y" },  { 0xde, "th" },  { 0xdf, "ss" }, { 0xe5, "a" },
    { 0xe6, "ae" }, { 0xe7, "c" },  { 0xeb, "e" },   { 0xef, "i" },  { 0xf0, "d" },
    { 0xf1, "n" },  { 0xf6, "o" },  { 0xf7, "" },    { 0xf8, "o" },  { 0xfc, "u" },
    { 0xfd, "y" },  { 0xfe, "th" }, { 0xff, "y" },   { 0x105, "a" }, { 0x10d, "c" },
    { 0x111, "d" }, { 0x11b, "e" }, { 0x123, "g" },  { 0x127, "h" }, { 0x131, "i" },
    { 0x133, "ij" }, { 0x135, "j" }, { 0x138, "k" }, { 0x142, "l" }, { 0x14b, "n" },
    { 0x151, "o" }, { 0x153, "oe" }, { 0x159, "r" }, { 0x161, "s" }, { 0x167, "t" },
    { 0x173, "u" }, { 0x175, "w" }, { 0x178, "y" },  { 0x17e, "z" }, { 0x17f, "s" },
};

/* Code points dropped like punctuation. */
static const struct {
    unsigned first, last;
} dropped[] = {
    { 0x0080, 0x00bf }, /* Latin-1 controls, punctuation and signs */
    { 0x0300, 0x036f }, /* combining accents */
    { 0x2000, 0x206f }, /* spaces, dashes, quotes, ... */
    { 0x3000, 0x303f }, /* CJK punctuation */
    { 0xfe00, 0xfe0f }, /* variation selectors */
    { 0xfeff, 0xfeff }, /* byte order mark */
};

/* Decodes the UTF-8 sequence at s (n >= 1 bytes left) into *cp. Its
 * length, or 0 if it is not valid UTF-8. */
static size_t decode(const unsigned char *s, size_t n, unsigned *cp)
{
    unsigned c = s[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    /* Overlong forms, surrogates and code points past U+10FFFF are
     * rejected through the allowed range of the second byte. */
    size_t more;
    unsigned lo = 0x80, hi = 0xbf;
    if (c >= 0xc2 && c <= 0xdf) {
        more = 1;
    } else if (c >= 0xe0 && c <= 0xef) {
        more = 2;
        lo = c == 0xe0 ? 0xa0 : lo;
        hi = c == 0xed ? 0x9f : hi;
    } else if (c >= 0xf0 && c <= 0xf4) {
        more = 3;
        lo = c == 0xf0 ? 0x90 : lo;
        hi = c == 0xf4 ? 0x8f : hi;
    } else {
        return 0;
    }
    if (n <= more || s[1] < lo || s[1] > hi)
        return 0;
    unsigned v = c & (0x3fu >> more);
    for (size_t i = 1; i <= more; i++) {
        if (s[i] < 0x80 || s[i] > 0xbf)
            return 0;
        v = v << 6 | (s[i] & 0x3fu);
    }
    *cp = v;
    return more + 1;
}

static int is_dropped(unsigned cp)
{
    for (size_t i = 0; i < sizeof dropped / sizeof dropped[0]; i++)
        if (cp >= dropped[i].first && cp <= dropped[i].last)
            return 1;
    return 0;
}

/* The key of the n bytes at s, without the "The " rule (dupes_key()). */
static size_t fold(const unsigned char *s, size_t n, char *out, size_t cap)
{
    size_t len = 0, i = 0;
    while (i < n) {
        unsigned cp;
        size_t step = decode(s + i, n - i, &cp);
        if (step == 0) {
            i++; /* not UTF-8: skipped */
            continue;
        }
        char ascii;
        const char *piece = NULL;
        size_t plen = 0;
        if (cp >= 'A' && cp <= 'Z') {
            ascii = (char)(cp - 'A' + 'a');
            piece = &ascii;
            plen = 1;
        } else if ((cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9')) {
            ascii = (char)cp;
            piece = &ascii;
            plen = 1;
        } else if (cp == '&') {
            piece = "and";
            plen = 3;
        } else if (cp >= 0xc0 && cp <= 0x17f) {
            size_t k = 0;
            while (latin[k].last < cp)
                k++;
            piece = latin[k].ascii;
            plen = strlen(piece);
        } else if (cp >= 0x80 && !is_dropped(cp)) {
            piece = (const char *)s + i; /* another script: as it is */
            plen = step;
        }
        if (len + plen >= cap)
            break;
        if (plen > 0)
            memcpy(out + len, piece, plen);
        len += plen;
        i += step;
    }
    out[len] = '\0';
    return len;
}

size_t dupes_key(const char *s, size_t n, char *out, size_t cap)
{
    if (cap == 0)
        return 0;
    const unsigned char *p = (const unsigned char *)s;
    size_t at = 0;
    while (at < n && p[at] == ' ')
        at++;
    if (n - at > 4 && (p[at] | 0x20) == 't' && (p[at + 1] | 0x20) == 'h' &&
        (p[at + 2] | 0x20) == 'e' && p[at + 3] == ' ') {
        size_t len = fold(p + at + 4, n - at - 4, out, cap);
        if (len > 0)
            return len;
    }
    return fold(p, n, out, cap);
}

/* dupe_key(text) in SQL. */
static void key_function(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
    (void)argc;
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }
    const unsigned char *s = sqlite3_value_text(argv[0]);
    if (s == NULL) {
        sqlite3_result_error_nomem(ctx);
        return;
    }
    size_t n = (size_t)sqlite3_value_bytes(argv[0]);
    char key[DUPES_KEY_MAX + 1];
    size_t len = dupes_key((const char *)s, n, key, sizeof key);
    if (len == 0)
        sqlite3_result_null(ctx);
    else
        sqlite3_result_text(ctx, key, (int)len, SQLITE_TRANSIENT);
}

int dupes_register(sqlite3 *db)
{
    return sqlite3_create_function_v2(db, "dupe_key", 1,
                                      SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS,
                                      NULL, key_function, NULL, NULL, NULL);
}
