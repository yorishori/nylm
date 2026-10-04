/*
 * Qobuz without the network (src/qobuz.h): links, the pasted login
 * redirect, what the web player's bundle holds, request signatures, and
 * the tags of a downloaded track.
 */
#define _GNU_SOURCE /* strcasestr */

#include "qobuz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <openssl/evp.h>

#include "arena.h"

/* ---- small helpers ------------------------------------------------------ */

static int is_lower(char c) { return c >= 'a' && c <= 'z'; }
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_alnum(char c)
{
    return is_lower(c) || is_digit(c) || (c >= 'A' && c <= 'Z');
}
/* A "word" character of the bundle's patterns: [A-Za-z0-9_]. */
static int is_word(char c) { return is_alnum(c) || c == '_'; }

/* The length of the run of characters at s for which ok() holds. */
static size_t span(const char *s, int (*ok)(char))
{
    size_t n = 0;
    while (s[n] != '\0' && ok(s[n]))
        n++;
    return n;
}

static int is_seed(char c) { return is_word(c) || c == '='; }

/* ---- links -------------------------------------------------------------- */

int qobuz_album_id(const char *url, char id[QOBUZ_MAX_ID + 1])
{
    static const char *const hosts[] = { "www.qobuz.com", "open.qobuz.com", "play.qobuz.com" };
    const char *p = url;
    if (p == NULL || strlen(p) > QOBUZ_MAX_REDIRECT)
        return -1;
    if (strncmp(p, "https://", 8) == 0)
        p += 8;
    else if (strncmp(p, "http://", 7) == 0)
        p += 7;
    else
        return -1;
    size_t i;
    for (i = 0; i < sizeof hosts / sizeof hosts[0]; i++) {
        size_t n = strlen(hosts[i]);
        if (strncmp(p, hosts[i], n) == 0 && p[n] == '/') {
            p += n;
            break;
        }
    }
    if (i == sizeof hosts / sizeof hosts[0])
        return -1;
    /* An optional language: /us-en */
    if (is_lower(p[1]) && is_lower(p[2]) && p[3] == '-' && is_lower(p[4]) && is_lower(p[5]) &&
        p[6] == '/')
        p += 6;
    if (strncmp(p, "/album/", 7) != 0)
        return -1;
    p += 7;
    /* ID or name/ID, then maybe a slash, a query or a fragment. */
    size_t len = strcspn(p, "?#");
    if (len > 0 && p[len - 1] == '/')
        len--;
    const char *slash = memchr(p, '/', len);
    const char *start = p;
    if (slash != NULL) {
        for (const char *c = p; c < slash; c++)
            if (!is_word(*c) && *c != '-')
                return -1;
        if (slash == p)
            return -1;
        start = slash + 1;
    }
    size_t n = (size_t)(p + len - start);
    if (n == 0 || n > QOBUZ_MAX_ID)
        return -1;
    for (size_t k = 0; k < n; k++)
        if (!is_alnum(start[k]))
            return -1;
    memcpy(id, start, n);
    id[n] = '\0';
    return 0;
}

/* ---- the login redirect -------------------------------------------------- */

int qobuz_redirect_valid(const char *s)
{
    size_t n = s != NULL ? strlen(s) : 0;
    if (n == 0 || n > QOBUZ_MAX_REDIRECT)
        return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] <= ' ' || (unsigned char)s[i] >= 0x7f)
            return 0;
    return 1;
}

static int hexval(char c)
{
    return is_digit(c) ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* The value of name in the query q (up to '#'), percent-decoded, into out.
 * 1 if found, 0 if not, -1 if it is too long or badly encoded. */
static int query_value(const char *q, const char *name, char out[QOBUZ_MAX_TOKEN + 1])
{
    size_t len = strlen(name);
    while (*q != '\0' && *q != '#') {
        size_t pair = strcspn(q, "&#");
        if (pair > len && strncmp(q, name, len) == 0 && q[len] == '=') {
            size_t o = 0;
            for (size_t i = len + 1; i < pair; i++) {
                char c = q[i];
                if (c == '%') {
                    if (i + 2 >= pair + 1 || hexval(q[i + 1]) < 0 || hexval(q[i + 2]) < 0)
                        return -1;
                    c = (char)(hexval(q[i + 1]) * 16 + hexval(q[i + 2]));
                    i += 2;
                } else if (c == '+') {
                    c = ' ';
                }
                if (o == QOBUZ_MAX_TOKEN || c == '\0')
                    return -1;
                out[o++] = c;
            }
            out[o] = '\0';
            return o > 0;
        }
        q += pair;
        if (*q == '&')
            q++;
    }
    return 0;
}

int qobuz_redirect_parse(const char *s, char code[QOBUZ_MAX_TOKEN + 1],
                         char token[QOBUZ_MAX_TOKEN + 1], char user_id[QOBUZ_MAX_TOKEN + 1])
{
    code[0] = token[0] = user_id[0] = '\0';
    if (!qobuz_redirect_valid(s))
        return -1;
    const char *q = strchr(s, '?');
    if (q == NULL) {
        if (strncmp(s, "http", 4) == 0 || strlen(s) > QOBUZ_MAX_TOKEN)
            return -1;
        snprintf(code, QOBUZ_MAX_TOKEN + 1, "%s", s);
        return 0;
    }
    q++;
    if (query_value(q, "code_autorisation", code) < 0 ||
        (code[0] == '\0' && query_value(q, "code", code) < 0) ||
        query_value(q, "user_auth_token", token) < 0 ||
        (token[0] == '\0' && query_value(q, "token", token) < 0) ||
        query_value(q, "user_id", user_id) < 0)
        return -1;
    return code[0] != '\0' || token[0] != '\0' ? 0 : -1;
}

/* ---- the bundle --------------------------------------------------------- */

int qobuz_bundle_path(const char *html, char *out, size_t size)
{
    static const char start[] = "<script src=\"/resources/";
    for (const char *p = strstr(html, start); p != NULL; p = strstr(p + 1, start)) {
        const char *v = p + sizeof start - 1;
        /* N.N.N-xNNN/bundle.js" */
        size_t a = span(v, is_digit);
        if (a == 0 || v[a] != '.')
            continue;
        size_t b = span(v + a + 1, is_digit);
        if (b == 0 || v[a + 1 + b] != '.')
            continue;
        const char *w = v + a + 1 + b + 1;
        size_t c = span(w, is_digit);
        if (c == 0 || w[c] != '-' || !is_lower(w[c + 1]) || !is_digit(w[c + 2]) ||
            !is_digit(w[c + 3]) || !is_digit(w[c + 4]) || strncmp(w + c + 5, "/bundle.js\"", 11) != 0)
            continue;
        size_t len = (size_t)(w + c + 5 + 10 - (p + 13));
        if (len + 1 > size)
            return -1;
        memcpy(out, p + 13, len); /* from "/resources/" */
        out[len] = '\0';
        return 0;
    }
    return -1;
}

int qobuz_app_id(const char *bundle, char out[QOBUZ_APP_ID_LEN + 1])
{
    static const char start[] = "production:{api:{appId:\"";
    for (const char *p = strstr(bundle, start); p != NULL; p = strstr(p + 1, start)) {
        const char *v = p + sizeof start - 1;
        if (span(v, is_digit) != QOBUZ_APP_ID_LEN ||
            strncmp(v + QOBUZ_APP_ID_LEN, "\",appSecret:\"", 13) != 0)
            continue;
        const char *secret = v + QOBUZ_APP_ID_LEN + 13;
        size_t n = span(secret, is_word);
        if (n != 32 || secret[n] != '"')
            continue;
        memcpy(out, v, QOBUZ_APP_ID_LEN);
        out[QOBUZ_APP_ID_LEN] = '\0';
        return 0;
    }
    return -1;
}

static int is_key(char c) { return is_alnum(c) || strchr("+/=_-", c) != NULL; }

void qobuz_private_key(const char *bundle, char out[QOBUZ_MAX_TOKEN + 1])
{
    /* Qobuz has used each of these names; the first found wins. */
    static const char *const names[] = { "privateKey:", "private_key:", "oauthKey:",
                                         "clientSecret:" };
    out[0] = '\0';
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        for (const char *p = strstr(bundle, names[i]); p != NULL; p = strstr(p + 1, names[i])) {
            const char *v = p + strlen(names[i]);
            while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')
                v++;
            if (*v != '"')
                continue;
            size_t n = span(v + 1, is_key);
            if (n < 6 || n > 128 || v[1 + n] != '"')
                continue;
            memcpy(out, v + 1, n);
            out[n] = '\0';
            return;
        }
    }
}

/* Base64 as Python's lenient decoder reads it: characters outside the
 * alphabet are skipped, a partial last byte dropped. The length, or -1. */
static long decode64(const char *s, size_t len, char *out, size_t max)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned long acc = 0;
    int bits = 0;
    size_t n = 0;
    for (size_t i = 0; i < len && s[i] != '='; i++) {
        const char *at = strchr(alphabet, s[i]);
        if (s[i] == '\0' || at == NULL)
            continue;
        acc = (acc << 6 | (unsigned long)(at - alphabet)) & 0xffffff;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n == max)
                return -1;
            out[n++] = (char)((acc >> bits) & 0xff);
        }
    }
    return (long)n;
}

#define MAX_ZONES     16
#define MAX_ZONE_TEXT 4096

struct zone {
    char name[32];
    char text[MAX_ZONE_TEXT]; /* the seeds, infos and extras, joined */
    size_t len;
    int full;
};

/* Appends n bytes of s to z's text (marks it full if they do not fit). */
static void zone_add(struct zone *z, const char *s, size_t n)
{
    if (z->len + n >= sizeof z->text) {
        z->full = 1;
        return;
    }
    memcpy(z->text + z->len, s, n);
    z->len += n;
    z->text[z->len] = '\0';
}

int qobuz_secrets(const char *bundle, char out[][QOBUZ_MAX_SECRET + 1], int max)
{
    static const char seed[] = ".initialSeed(\"";
    static const char zone_mark[] = "\",window.utimezone.";
    struct zone *zones = calloc(MAX_ZONES, sizeof *zones);
    if (zones == NULL)
        return 0;
    int nzones = 0;
    /* [a-z].initialSeed("SEED",window.utimezone.ZONE) */
    for (const char *p = strstr(bundle, seed); p != NULL; p = strstr(p + 1, seed)) {
        if (p == bundle || !is_lower(p[-1]))
            continue;
        const char *s = p + sizeof seed - 1;
        size_t n = span(s, is_seed);
        if (n == 0 || strncmp(s + n, zone_mark, sizeof zone_mark - 1) != 0)
            continue;
        const char *z = s + n + sizeof zone_mark - 1;
        size_t zl = span(z, is_lower);
        if (zl == 0 || zl >= sizeof zones[0].name || z[zl] != ')')
            continue;
        int i;
        for (i = 0; i < nzones; i++)
            if (strlen(zones[i].name) == zl && strncmp(zones[i].name, z, zl) == 0)
                break;
        if (i == nzones) {
            if (nzones == MAX_ZONES)
                continue;
            memcpy(zones[i].name, z, zl);
            zones[i].name[zl] = '\0';
            nzones++;
        }
        zone_add(&zones[i], s, n);
    }
    /* name:"Word/Zone",info:"INFO",extras:"EXTRAS" (Zone capitalised) */
    for (int i = 0; i < nzones; i++) {
        char mark[64];
        snprintf(mark, sizeof mark, "/%c%s\",info:\"", zones[i].name[0] - 'a' + 'A',
                 zones[i].name + 1);
        for (const char *p = strstr(bundle, mark); p != NULL; p = strstr(p + 1, mark)) {
            const char *w = p;
            while (w > bundle && is_word(w[-1]))
                w--;
            if (w == p || w - bundle < 6 || strncmp(w - 6, "name:\"", 6) != 0)
                continue;
            const char *info = p + strlen(mark);
            size_t a = span(info, is_seed);
            if (a == 0 || strncmp(info + a, "\",extras:\"", 10) != 0)
                continue;
            const char *extras = info + a + 10;
            size_t b = span(extras, is_seed);
            if (b == 0 || extras[b] != '"')
                continue;
            zone_add(&zones[i], info, a);
            zone_add(&zones[i], extras, b);
        }
    }
    int count = 0;
    for (int i = 0; i < nzones && count < max; i++) {
        if (zones[i].full || zones[i].len <= 44)
            continue;
        long n = decode64(zones[i].text, zones[i].len - 44, out[count], QOBUZ_MAX_SECRET);
        if (n <= 0)
            continue;
        out[count][n] = '\0';
        int printable = 1;
        for (long k = 0; k < n; k++)
            printable &= (unsigned char)out[count][k] > ' ' && (unsigned char)out[count][k] < 0x7f;
        count += printable;
    }
    free(zones);
    return count;
}

int qobuz_signature(const char *track_id, int format, long long ts, const char *secret,
                    char out[33])
{
    char text[QOBUZ_MAX_ID + QOBUZ_MAX_SECRET + 128];
    int n = snprintf(text, sizeof text, "trackgetFileUrlformat_id%dintentstreamtrack_id%s%lld%s",
                     format, track_id, ts, secret);
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned len = 0;
    if (n < 0 || (size_t)n >= sizeof text ||
        EVP_Digest(text, (size_t)n, md, &len, EVP_md5(), NULL) != 1 || len != 16) {
        fprintf(stderr, "qobuz: can not sign a request\n");
        return -1;
    }
    for (unsigned i = 0; i < 16; i++)
        snprintf(out + 2 * i, 3, "%02x", md[i]);
    return 0;
}

int qobuz_urlencode(const char *s, char *out, size_t size)
{
    size_t o = 0;
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (is_alnum((char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            if (o + 1 >= size)
                return -1;
            out[o++] = (char)c;
        } else {
            if (o + 3 >= size)
                return -1;
            snprintf(out + o, 4, "%%%02X", c);
            o += 3;
        }
    }
    if (o >= size)
        return -1;
    out[o] = '\0';
    return 0;
}

/* ---- tags ---------------------------------------------------------------- */

/* obj's string member name (one level, or "a.b"), or NULL. */
static const char *text_of(const cJSON *obj, const char *name)
{
    char first[32];
    const char *dot = strchr(name, '.');
    if (dot != NULL) {
        snprintf(first, sizeof first, "%.*s", (int)(dot - name), name);
        obj = cJSON_GetObjectItemCaseSensitive(obj, first);
        name = dot + 1;
    }
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, name);
    return cJSON_IsString(v) && v->valuestring[0] != '\0' ? v->valuestring : NULL;
}

/* obj's number member name as a whole number >= 1, or 0. */
static int number_of(const cJSON *obj, const char *name)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, name);
    return cJSON_IsNumber(v) && v->valuedouble >= 1 && v->valuedouble <= 100000
               ? (int)v->valuedouble : 0;
}

/* "Title (Version)", unless the title already has the version; in the
 * arena. NULL if there is no title (or out of memory). */
static const char *with_version(const cJSON *item)
{
    const char *title = text_of(item, "title");
    const char *version = text_of(item, "version");
    if (title == NULL || version == NULL || strcasestr(title, version) != NULL)
        return title;
    size_t n = strlen(title) + strlen(version) + 4;
    char *s = arena_alloc(n);
    if (s != NULL)
        snprintf(s, n, "%s (%s)", title, version);
    return s;
}

/* (P) and (C) as the symbols, in the arena. */
static const char *copyright(const char *s)
{
    if (s == NULL)
        return NULL;
    char *out = arena_alloc(strlen(s) + 1);
    if (out == NULL)
        return NULL;
    size_t o = 0;
    for (const char *p = s; *p != '\0';) {
        if (strncmp(p, "(P)", 3) == 0 || strncmp(p, "(C)", 3) == 0) {
            /* ℗ and © take 3 and 2 bytes: never longer than "(P)". */
            const char *sym = p[1] == 'P' ? "\xe2\x84\x97" : "\xc2\xa9";
            memcpy(out + o, sym, strlen(sym));
            o += strlen(sym);
            p += 3;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
    return out;
}

const char *qobuz_track_tags(const cJSON *album, const cJSON *track, struct tags *t)
{
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(album, "tracks"), "items");
    const char *title = with_version(track);
    const char *album_title = with_version(album);
    const char *album_artist = text_of(album, "artist.name");
    const char *artist = text_of(track, "performer.name");
    int number = number_of(track, "track_number");
    int disc = number_of(track, "media_number");
    if (disc == 0)
        disc = 1;
    if (title == NULL)
        return "the track has no title";
    if (album_title == NULL || album_artist == NULL)
        return "the album has no title or artist";
    if (number == 0)
        return "the track has no number";
    /* The tracks on its disc, and the discs. */
    int on_disc = 0, discs = number_of(album, "media_count");
    const cJSON *it;
    cJSON_ArrayForEach(it, items) {
        int d = number_of(it, "media_number");
        if (d == 0)
            d = 1;
        on_disc += d == disc;
        if (d > discs)
            discs = d;
    }
    if (on_disc < number)
        on_disc = number;
    if (discs < disc)
        discs = disc;
    char tracknumber[32], discnumber[32], year[8] = "";
    snprintf(tracknumber, sizeof tracknumber, "%d/%d", number, on_disc);
    snprintf(discnumber, sizeof discnumber, "%d/%d", disc, discs);
    const char *date = text_of(album, "release_date_original");
    if (date != NULL && is_digit(date[0]) && is_digit(date[1]) && is_digit(date[2]) &&
        is_digit(date[3]) && date[0] != '0')
        snprintf(year, sizeof year, "%.4s", date);

    /* tags_set_one() keeps the pointer: the numbers go into the arena. */
    const char *tn = arena_strndup(tracknumber, strlen(tracknumber));
    const char *dn = arena_strndup(discnumber, strlen(discnumber));
    const char *yn = arena_strndup(year, strlen(year));
    if (tn == NULL || dn == NULL || yn == NULL)
        return "out of memory";
    memset(t, 0, sizeof *t);
    const struct {
        enum tag_field f;
        const char *v;
    } set[] = {
        { TAG_TITLE, title },
        { TAG_ALBUM, album_title },
        { TAG_ARTIST, artist != NULL ? artist : album_artist },
        { TAG_ALBUMARTIST, album_artist },
        { TAG_TRACKNUMBER, tn },
        { TAG_DISCNUMBER, dn },
        { TAG_DATE, yn },
        { TAG_COMPILATION, "0" },
        { TAG_COMPOSER, text_of(track, "composer.name") },
        { TAG_COPYRIGHT, copyright(text_of(album, "copyright")) },
        { TAG_LABEL, text_of(album, "label.name") },
        { TAG_ISRC, text_of(track, "isrc") },
        { TAG_BARCODE, text_of(album, "upc") },
    };
    for (size_t i = 0; i < sizeof set / sizeof set[0]; i++)
        if (tags_set_one(t, set[i].f, set[i].v) != 0)
            return "out of memory";
    return NULL;
}
