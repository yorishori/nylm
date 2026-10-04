/*
 * Picture files (see art.h): a folder per store, a file per picture and per
 * thumbnail, named by the SHA-256 of the picture. Every file is opened
 * relative to the folder with O_NOFOLLOW, and written whole under a
 * temporary name first, so a reader never sees half a picture.
 */
#define _POSIX_C_SOURCE 200809L

#include "art.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/evp.h>

#include "db.h"

#define TMP_PREFIX  ".tmp-"
#define THUMB_SUFFIX ".thumb"
#define NAME_LEN     (ART_HASH_LEN + sizeof THUMB_SUFFIX)  /* a file name, with its NUL */
#define TMP_LEN      (sizeof TMP_PREFIX - 1 + NAME_LEN)    /* its temporary name */

static char dir_paths[ART_STORES][DB_MAX_PATH];

int art_configure(const char *data_dir)
{
    static const char *const folders[ART_STORES] = { "music/art", "plants/photos" };
    for (int i = 0; i < ART_STORES; i++) {
        int n = snprintf(dir_paths[i], sizeof dir_paths[i], "%s/%s", data_dir, folders[i]);
        if (n < 0 || (size_t)n >= sizeof dir_paths[i]) {
            fprintf(stderr, "art: data folder path is too long\n");
            dir_paths[i][0] = '\0';
            return -1;
        }
    }
    return 0;
}

int art_hash_valid(const char *s)
{
    size_t n = strspn(s, "0123456789abcdef");
    return n == ART_HASH_LEN && s[n] == '\0';
}

int art_hash(const unsigned char *data, size_t size, char out[ART_HASH_LEN + 1])
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_Digest(data, size, md, &len, EVP_sha256(), NULL) != 1 || len * 2 != ART_HASH_LEN) {
        fprintf(stderr, "art: SHA-256 failed\n");
        return -1;
    }
    static const char hex[] = "0123456789abcdef";
    for (unsigned int i = 0; i < len; i++) {
        out[2 * i] = hex[md[i] >> 4];
        out[2 * i + 1] = hex[md[i] & 0xf];
    }
    out[ART_HASH_LEN] = '\0';
    return 0;
}

const char *art_mime(const unsigned char *data, size_t size)
{
    if (size >= 3 && memcmp(data, "\xff\xd8\xff", 3) == 0)
        return "image/jpeg";
    if (size >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0)
        return "image/png";
    if (size >= 6 && (memcmp(data, "GIF87a", 6) == 0 || memcmp(data, "GIF89a", 6) == 0))
        return "image/gif";
    if (size >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0)
        return "image/webp";
    return NULL;
}

/* The value of a base64 character, or -1. */
static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

long art_base64_decode(const char *s, size_t len, unsigned char *out, size_t max)
{
    if (len % 4 != 0)
        return -1;
    size_t pad = len >= 1 && s[len - 1] == '=' ? (len >= 2 && s[len - 2] == '=' ? 2 : 1) : 0;
    size_t size = len / 4 * 3 - pad;
    if (size > max)
        return -2;
    size_t at = 0;
    for (size_t i = 0; i < len; i += 4) {
        int last = i + 4 == len;
        int v[4];
        for (size_t k = 0; k < 4; k++) {
            /* '=' only as the padding of the last group */
            v[k] = last && k >= 4 - pad ? 0 : b64_value(s[i + k]);
            if (v[k] < 0)
                return -1;
        }
        unsigned long group = (unsigned long)v[0] << 18 | (unsigned long)v[1] << 12 |
                              (unsigned long)v[2] << 6 | (unsigned long)v[3];
        size_t bytes = last ? 3 - pad : 3;
        /* the bits after the last byte must be zero (one encoding only) */
        if (last && pad > 0 && (group & ((1ul << (8 * pad)) - 1)) != 0)
            return -1;
        for (size_t k = 0; k < bytes; k++)
            out[at++] = (unsigned char)(group >> (16 - 8 * k));
    }
    return (long)at;
}

/* Opens store's folder, making it first if needed: an fd, or -1 (logged). */
static int open_dir(enum art_store store)
{
    const char *dir_path = dir_paths[store];
    if (dir_path[0] == '\0') {
        fprintf(stderr, "art: not configured\n");
        return -1;
    }
    if (mkdir(dir_path, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "art: mkdir %s: %s\n", dir_path, strerror(errno));
        return -1;
    }
    int fd = open(dir_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        fprintf(stderr, "art: open %s: %s\n", dir_path, strerror(errno));
    return fd;
}

/* The file name of hash (thumb: its thumbnail) into name (NAME_LEN). */
static void file_name(const char *hash, int thumb, char *name)
{
    snprintf(name, NAME_LEN, "%.64s%s", hash, thumb ? THUMB_SUFFIX : "");
}

/* Writes all of data to fd. 0 or -1. */
static int write_all(int fd, const unsigned char *data, size_t size)
{
    while (size > 0) {
        ssize_t n = write(fd, data, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        data += n;
        size -= (size_t)n;
    }
    return 0;
}

int art_save(enum art_store store, const char *hash, int thumb, const unsigned char *data,
             size_t size)
{
    const char *dir_path = dir_paths[store];
    if (!art_hash_valid(hash)) {
        fprintf(stderr, "art: invalid hash\n");
        return -1;
    }
    int dir = open_dir(store);
    if (dir < 0)
        return -1;
    char name[NAME_LEN], tmp[TMP_LEN];
    file_name(hash, thumb, name);
    snprintf(tmp, sizeof tmp, TMP_PREFIX "%s", name);
    struct stat sb;
    if (fstatat(dir, name, &sb, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(sb.st_mode)) {
        close(dir);
        return 0; /* the same bytes are there already */
    }
    int rc = -1;
    int fd = openat(dir, tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) {
        int ok = write_all(fd, data, size) == 0 && fsync(fd) == 0;
        if (close(fd) == 0 && ok && renameat(dir, tmp, dir, name) == 0)
            rc = 0;
    }
    if (rc != 0) {
        fprintf(stderr, "art: can not save %s/%s: %s\n", dir_path, name, strerror(errno));
        unlinkat(dir, tmp, 0);
    }
    close(dir);
    return rc;
}

int art_open(enum art_store store, const char *hash, int thumb)
{
    const char *dir_path = dir_paths[store];
    if (!art_hash_valid(hash)) {
        errno = ENOENT;
        return -1;
    }
    int dir = open_dir(store);
    if (dir < 0)
        return -1;
    char name[NAME_LEN];
    file_name(hash, thumb, name);
    int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    int err = errno;
    struct stat sb;
    if (fd >= 0 && (fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode))) {
        close(fd);
        fd = -1;
        err = EINVAL;
    }
    if (fd < 0 && err != ENOENT)
        fprintf(stderr, "art: open %s/%s: %s\n", dir_path, name, strerror(err));
    close(dir);
    errno = err;
    return fd;
}

long art_load(enum art_store store, const char *hash, unsigned char *out, size_t max)
{
    int fd = art_open(store, hash, 0);
    if (fd < 0) {
        if (errno == ENOENT)
            fprintf(stderr, "art: %s is not stored\n", hash);
        return -1;
    }
    size_t at = 0;
    long rc = 0;
    for (;;) {
        if (at == max) {
            /* full: one more byte means too big */
            unsigned char extra;
            ssize_t n = read(fd, &extra, 1);
            rc = n == 0 ? (long)at : n > 0 ? -2 : -1;
            break;
        }
        ssize_t n = read(fd, out + at, max - at);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            rc = n == 0 ? (long)at : -1;
            break;
        }
        at += (size_t)n;
    }
    if (rc == -1)
        fprintf(stderr, "art: read %s: %s\n", hash, strerror(errno));
    close(fd);
    return rc;
}

long art_sweep(enum art_store store, int (*keep)(const char *hash, void *ctx), void *ctx)
{
    const char *dir_path = dir_paths[store];
    int dir = open_dir(store);
    if (dir < 0)
        return -1;
    int fd = dup(dir);
    DIR *d = fd >= 0 ? fdopendir(fd) : NULL;
    if (d == NULL) {
        fprintf(stderr, "art: can not list %s: %s\n", dir_path, strerror(errno));
        if (fd >= 0)
            close(fd);
        close(dir);
        return -1;
    }
    long removed = 0;
    for (;;) {
        errno = 0;
        struct dirent *de = readdir(d);
        if (de == NULL) {
            if (errno != 0) {
                fprintf(stderr, "art: can not list %s: %s\n", dir_path, strerror(errno));
                removed = -1;
            }
            break;
        }
        const char *name = de->d_name;
        size_t len = strlen(name);
        int drop;
        if (strncmp(name, TMP_PREFIX, sizeof TMP_PREFIX - 1) == 0) {
            drop = 1;
        } else {
            char hash[ART_HASH_LEN + 1];
            int ours = (len == ART_HASH_LEN ||
                        (len == ART_HASH_LEN + sizeof THUMB_SUFFIX - 1 &&
                         strcmp(name + ART_HASH_LEN, THUMB_SUFFIX) == 0));
            if (ours) {
                memcpy(hash, name, ART_HASH_LEN);
                hash[ART_HASH_LEN] = '\0';
                ours = art_hash_valid(hash);
            }
            int k = ours ? keep(hash, ctx) : 1;
            if (k < 0) {
                removed = -1;
                break;
            }
            drop = k == 0;
        }
        if (!drop)
            continue;
        if (unlinkat(dir, name, 0) == 0)
            removed++;
        else
            fprintf(stderr, "art: can not remove %s/%s: %s\n", dir_path, name, strerror(errno));
    }
    closedir(d);
    close(dir);
    return removed;
}
