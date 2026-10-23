#include "notes.h"

#include <stdio.h>
#include <string.h>

#include <openssl/rand.h>

int notes_id_valid(const char *s)
{
    size_t i = 0;
    for (; s[i] != '\0' && i <= NOTES_ID_LEN; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return 0;
    return i == NOTES_ID_LEN;
}

int notes_new_id(char out[NOTES_ID_LEN + 1])
{
    static const char hex[] = "0123456789abcdef";
    unsigned char bytes[NOTES_ID_LEN / 2];
    if (RAND_bytes(bytes, sizeof bytes) != 1) {
        fprintf(stderr, "notes: RAND_bytes failed\n");
        return -1;
    }
    for (size_t i = 0; i < sizeof bytes; i++) {
        out[2 * i] = hex[bytes[i] >> 4];
        out[2 * i + 1] = hex[bytes[i] & 0xf];
    }
    out[NOTES_ID_LEN] = '\0';
    return 0;
}

int notes_suffix_name(const char *name, int n, char out[NOTES_TAG_MAX + 1])
{
    if (n < 2 || n > 99)
        return -1;
    char suffix[4];
    int slen = snprintf(suffix, sizeof suffix, " %d", n);
    size_t keep = strlen(name);
    if (keep > NOTES_TAG_MAX - (size_t)slen) {
        keep = NOTES_TAG_MAX - (size_t)slen;
        /* Never cut a character in two: back off its continuation bytes. */
        while (keep > 0 && ((unsigned char)name[keep] & 0xc0) == 0x80)
            keep--;
    }
    memcpy(out, name, keep);
    memcpy(out + keep, suffix, (size_t)slen + 1);
    return 0;
}
