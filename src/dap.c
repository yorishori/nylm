/*
 * DAPs plugged into the server: finding them (src/dap.h).
 */
#define _POSIX_C_SOURCE 200809L

#include "dap.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "json.h"
#include "sysinfo.h"

#define MAX_UDEV      (64 * 1024)   /* bytes of one udev data file */
#define MAX_MOUNTINFO (1024 * 1024) /* bytes of the mount table */
#define MAX_DISKS     256           /* /sys/block entries looked at */

int dap_uuid_valid(const char *s)
{
    if (s == NULL || strlen(s) != DAP_UUID_LEN)
        return 0;
    for (int i = 0; i < DAP_UUID_LEN; i++) {
        char c = s[i];
        int hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
        if (i == 4 ? c != '-' : !hex)
            return 0;
    }
    return 1;
}

/* Copies the value of line (after "E:key=") into out if it is text of at
 * most outlen - 1 bytes; else leaves out empty. */
static void take(const char *value, size_t len, char *out, size_t outlen)
{
    out[0] = '\0';
    if (len >= outlen)
        return;
    memcpy(out, value, len);
    out[len] = '\0';
    if (!text_valid(out, 0))
        out[0] = '\0';
}

int dap_udev_parse(const char *text, struct dap_drive *d)
{
    static const struct {
        const char *key;
        size_t off, len;
    } keys[] = {
        { "E:ID_FS_TYPE=", offsetof(struct dap_drive, fstype), sizeof d->fstype },
        { "E:ID_FS_UUID=", offsetof(struct dap_drive, uuid), sizeof d->uuid },
        { "E:ID_FS_LABEL=", offsetof(struct dap_drive, label), sizeof d->label },
        { "E:ID_VENDOR=", offsetof(struct dap_drive, vendor), sizeof d->vendor },
        { "E:ID_MODEL=", offsetof(struct dap_drive, model), sizeof d->model },
    };
    for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++)
        ((char *)d + keys[k].off)[0] = '\0';
    for (const char *line = text; line != NULL && *line != '\0';) {
        const char *end = strchr(line, '\n');
        size_t n = end != NULL ? (size_t)(end - line) : strlen(line);
        for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++) {
            size_t kl = strlen(keys[k].key);
            if (n >= kl && strncmp(line, keys[k].key, kl) == 0)
                take(line + kl, n - kl, (char *)d + keys[k].off, keys[k].len);
        }
        line = end != NULL ? end + 1 : NULL;
    }
    return (strcmp(d->fstype, "vfat") == 0 || strcmp(d->fstype, "exfat") == 0) &&
           dap_uuid_valid(d->uuid);
}

/*
 * Finds the mountinfo line whose device (field 3) is majmin, or whose
 * mount point (field 5) is path: copies the other into out. 1 if found.
 */
static int find_mount(const char *mountinfo, const char *majmin, const char *path, char *out,
                      size_t outlen)
{
    for (const char *line = mountinfo; line != NULL && *line != '\0';) {
        const char *end = strchr(line, '\n');
        size_t n = end != NULL ? (size_t)(end - line) : strlen(line);
        char buf[4096], *save = NULL, *field[5];
        int nf = 0;
        if (n < sizeof buf) {
            memcpy(buf, line, n);
            buf[n] = '\0';
            for (char *f = strtok_r(buf, " ", &save); f != NULL && nf < 5;
                 f = strtok_r(NULL, " ", &save))
                field[nf++] = f;
        }
        if (nf == 5 && sysinfo_unescape(field[4]) == 0) {
            const char *got = NULL;
            if (majmin != NULL && strcmp(field[2], majmin) == 0)
                got = field[4];
            else if (path != NULL && strcmp(field[4], path) == 0)
                got = field[2];
            if (got != NULL) {
                int w = snprintf(out, outlen, "%s", got);
                return w >= 0 && (size_t)w < outlen;
            }
        }
        line = end != NULL ? end + 1 : NULL;
    }
    return 0;
}

int dap_mount_of(const char *mountinfo, const char *majmin, char *out, size_t outlen)
{
    return find_mount(mountinfo, majmin, NULL, out, outlen);
}

int dap_device_at(const char *mountinfo, const char *path, char *out, size_t outlen)
{
    return find_mount(mountinfo, NULL, path, out, outlen);
}

/* a/b into out (outlen bytes). 0, or -1 if it does not fit. */
static int join(char *out, size_t outlen, const char *a, const char *b)
{
    int w = snprintf(out, outlen, "%s/%s", a, b);
    return w >= 0 && (size_t)w < outlen ? 0 : -1;
}

/* The first line of the file at path into out, without its newline. 0, or
 * -1 if it can not be read or is empty. */
static int first_line(const char *path, char *out, size_t outlen)
{
    const char *text = sysinfo_read_file(path, 256);
    size_t n = text != NULL ? strcspn(text, "\n") : 0;
    if (n == 0 || n >= outlen)
        return -1;
    memcpy(out, text, n);
    out[n] = '\0';
    return 0;
}

/* Adds the file system at sys/block/disk[/part] (dir) to out if it is a
 * FAT or exFAT one udev knows. 1 if added, 0 if not. */
static int add_drive(const char *dir, const char *name, const char *udev, const char *mountinfo,
                     struct dap_drive *d)
{
    char path[1024], size[32], file[40];
    memset(d, 0, sizeof *d);
    if (strlen(name) >= sizeof d->dev)
        return 0;
    snprintf(d->dev, sizeof d->dev, "%s", name);
    if (join(path, sizeof path, dir, "dev") != 0 ||
        first_line(path, d->majmin, sizeof d->majmin) != 0 ||
        strspn(d->majmin, "0123456789:") != strlen(d->majmin))
        return 0;
    snprintf(file, sizeof file, "b%s", d->majmin);
    const char *data = join(path, sizeof path, udev, file) == 0
                           ? sysinfo_read_file(path, MAX_UDEV) : NULL;
    if (data == NULL || !dap_udev_parse(data, d))
        return 0;
    if (join(path, sizeof path, dir, "size") == 0 && first_line(path, size, sizeof size) == 0) {
        char *end;
        errno = 0;
        unsigned long long sectors = strtoull(size, &end, 10);
        if (errno == 0 && *end == '\0' && sectors < (1ULL << 50))
            d->size = sectors * 512;
    }
    if (mountinfo != NULL)
        dap_mount_of(mountinfo, d->majmin, d->mounted, sizeof d->mounted);
    return 1;
}

static int by_dev(const void *a, const void *b)
{
    return strcmp(((const struct dap_drive *)a)->dev, ((const struct dap_drive *)b)->dev);
}

/* 1 if name is a /sys/block entry nylm looks at: a-z 0-9 only. */
static int plain_name(const char *name)
{
    return name[0] != '\0' && strspn(name, "abcdefghijklmnopqrstuvwxyz0123456789") == strlen(name);
}

int dap_list(const char *sys, const char *udev, const char *mountinfo, struct dap_drive *out,
             int max)
{
    char block[1024];
    DIR *d = join(block, sizeof block, sys, "block") == 0 ? opendir(block) : NULL;
    if (d == NULL) {
        fprintf(stderr, "dap: %s: %s\n", block, strerror(errno));
        return -1;
    }
    const char *mounts = sysinfo_read_file(mountinfo, MAX_MOUNTINFO);
    int n = 0, seen = 0;
    struct dirent *e;
    while (n < max && seen++ < MAX_DISKS && (e = readdir(d)) != NULL) {
        char disk[1024], target[1024];
        if (!plain_name(e->d_name) || join(disk, sizeof disk, block, e->d_name) != 0)
            continue;
        /* On USB: the device the disk belongs to is below a USB port. */
        ssize_t len = readlink(disk, target, sizeof target - 1);
        if (len <= 0)
            continue;
        target[len] = '\0';
        if (strstr(target, "/usb") == NULL)
            continue;
        DIR *parts = opendir(disk);
        int had_part = 0;
        struct dirent *p;
        while (parts != NULL && n < max && (p = readdir(parts)) != NULL) {
            char dir[1024], marker[1024];
            if (!plain_name(p->d_name) || strncmp(p->d_name, e->d_name, strlen(e->d_name)) != 0 ||
                join(dir, sizeof dir, disk, p->d_name) != 0 ||
                join(marker, sizeof marker, dir, "partition") != 0 || access(marker, F_OK) != 0)
                continue;
            had_part = 1;
            n += add_drive(dir, p->d_name, udev, mounts, &out[n]);
        }
        if (parts != NULL)
            closedir(parts);
        if (!had_part && n < max)
            n += add_drive(disk, e->d_name, udev, mounts, &out[n]);
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof *out, by_dev);
    return n;
}
