#define _POSIX_C_SOURCE 200809L

#include "sysinfo.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "arena.h"

#define MAX_HWMON      64 /* hwmon0..hwmon63 */
#define MAX_TEMP_INDEX 32 /* temp1..temp32 of each */
#define LINE_MAX_LEN   4096

/* sysinfo_read_file; quiet: log nothing (a sensor that is not ready fails
 * its reads). */
static char *read_file(const char *path, size_t max, int quiet)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno != ENOENT && !quiet)
            fprintf(stderr, "sysinfo: %s: %s\n", path, strerror(errno));
        return NULL;
    }
    char *buf = arena_alloc(max + 1);
    size_t used = 0;
    /* /proc and /sys files say size 0: read until the end. */
    while (buf != NULL) {
        ssize_t n = read(fd, buf + used, max + 1 - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            if (!quiet)
                fprintf(stderr, "sysinfo: %s: %s\n", path, strerror(errno));
            buf = NULL;
        } else if (n == 0) {
            buf[used] = '\0';
            break;
        } else if ((used += (size_t)n) > max) {
            if (!quiet)
                fprintf(stderr, "sysinfo: %s: larger than %zu bytes\n", path, max);
            buf = NULL;
        }
    }
    close(fd);
    return buf;
}

char *sysinfo_read_file(const char *path, size_t max)
{
    return read_file(path, max, 0);
}

/* The number after "name:" at the start of a line of text (meminfo), in
 * kB; -1 if there is no such line or it is not a number. */
static long long meminfo_kb(const char *text, const char *name)
{
    size_t n = strlen(name);
    for (const char *line = text; line != NULL && *line != '\0';) {
        if (strncmp(line, name, n) == 0 && line[n] == ':') {
            char *end;
            errno = 0;
            long long v = strtoll(line + n + 1, &end, 10);
            if (errno != 0 || end == line + n + 1 || v < 0 || strncmp(end, " kB", 3) != 0)
                return -1;
            return v;
        }
        line = strchr(line, '\n');
        if (line != NULL)
            line++;
    }
    return -1;
}

cJSON *sysinfo_memory(const char *meminfo)
{
    long long total = meminfo_kb(meminfo, "MemTotal");
    long long avail = meminfo_kb(meminfo, "MemAvailable");
    long long swap = meminfo_kb(meminfo, "SwapTotal");
    long long swap_free = meminfo_kb(meminfo, "SwapFree");
    if (total < 0 || avail < 0 || avail > total)
        return NULL;
    if (swap < 0 || swap_free < 0 || swap_free > swap)
        swap = swap_free = 0;
    cJSON *o = cJSON_CreateObject();
    if (o == NULL || cJSON_AddNumberToObject(o, "total", (double)total * 1024) == NULL ||
        cJSON_AddNumberToObject(o, "available", (double)avail * 1024) == NULL ||
        cJSON_AddNumberToObject(o, "swap_total", (double)swap * 1024) == NULL ||
        cJSON_AddNumberToObject(o, "swap_free", (double)swap_free * 1024) == NULL)
        return NULL;
    return o;
}

long long sysinfo_uptime(const char *text)
{
    char *end;
    errno = 0;
    double s = strtod(text, &end);
    if (errno != 0 || end == text || *end != ' ' || !(s >= 0 && s < 1e12))
        return -1;
    return (long long)s;
}

cJSON *sysinfo_load(const char *text)
{
    double load[3];
    const char *p = text;
    for (int i = 0; i < 3; i++) {
        char *end;
        errno = 0;
        load[i] = strtod(p, &end);
        if (errno != 0 || end == p || *end != ' ' || !(load[i] >= 0 && load[i] < 1e9))
            return NULL;
        p = end + 1;
    }
    return cJSON_CreateDoubleArray(load, 3);
}

int sysinfo_unescape(char *s)
{
    char *out = s;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p != '\\') {
            *out++ = *p;
            continue;
        }
        int v = 0;
        for (int i = 1; i <= 3; i++) {
            if (p[i] < '0' || p[i] > '7')
                return -1;
            v = v * 8 + (p[i] - '0');
        }
        if (v == 0 || v > 0177)
            return -1;
        *out++ = (char)v;
        p += 3;
    }
    *out = '\0';
    return 0;
}

/* Filesystems that live on a disk of this machine. */
static int disk_fs(const char *type)
{
    static const char *const types[] = {
        "ext2", "ext3", "ext4", "xfs", "btrfs", "bcachefs", "f2fs", "jfs", "zfs",
        "vfat", "exfat", "ntfs", "ntfs3", "fuseblk",
    };
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++)
        if (strcmp(type, types[i]) == 0)
            return 1;
    return 0;
}

/* 1 if list already has a mount of device. */
static int has_device(const cJSON *list, const char *device)
{
    const cJSON *m;
    cJSON_ArrayForEach(m, list) {
        const cJSON *d = cJSON_GetObjectItemCaseSensitive(m, "device");
        if (cJSON_IsString(d) && strcmp(d->valuestring, device) == 0)
            return 1;
    }
    return 0;
}

cJSON *sysinfo_mounts(const char *text)
{
    cJSON *list = cJSON_CreateArray();
    if (list == NULL)
        return NULL;
    int count = 0;
    for (const char *line = text; *line != '\0';) {
        const char *nl = strchr(line, '\n');
        size_t len = nl != NULL ? (size_t)(nl - line) : strlen(line);
        if (len > LINE_MAX_LEN)
            return NULL;
        char *copy = arena_strndup(line, len);
        if (copy == NULL)
            return NULL;
        line += len + (nl != NULL);

        char *save = NULL;
        char *device = strtok_r(copy, " ", &save);
        char *path = device != NULL ? strtok_r(NULL, " ", &save) : NULL;
        char *type = path != NULL ? strtok_r(NULL, " ", &save) : NULL;
        if (device == NULL)
            continue; /* an empty line */
        if (type == NULL || sysinfo_unescape(device) != 0 || sysinfo_unescape(path) != 0 ||
            path[0] != '/')
            return NULL;
        if (!disk_fs(type) || has_device(list, device))
            continue;
        if (count == SYSINFO_MAX_MOUNTS)
            break;
        cJSON *m = cJSON_CreateObject();
        if (m == NULL || cJSON_AddStringToObject(m, "path", path) == NULL ||
            cJSON_AddStringToObject(m, "device", device) == NULL ||
            cJSON_AddStringToObject(m, "type", type) == NULL ||
            !cJSON_AddItemToArray(list, m))
            return NULL;
        count++;
    }
    return list;
}

/* The first line of a small file in dir, without its newline; NULL if none. */
static char *read_line(const char *dir, const char *name)
{
    char path[1024];
    int n = snprintf(path, sizeof path, "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= sizeof path)
        return NULL;
    char *s = read_file(path, 128, 1);
    if (s != NULL)
        s[strcspn(s, "\n")] = '\0';
    return s;
}

cJSON *sysinfo_temperatures(const char *hwmon_dir)
{
    cJSON *list = cJSON_CreateArray();
    if (list == NULL)
        return NULL;
    int count = 0;
    /* hwmon devices are numbered from 0 without gaps; temp inputs from 1,
     * sometimes with gaps. */
    for (int h = 0; h < MAX_HWMON && count < SYSINFO_MAX_TEMPS; h++) {
        char dir[1024];
        int n = snprintf(dir, sizeof dir, "%s/hwmon%d", hwmon_dir, h);
        if (n < 0 || (size_t)n >= sizeof dir || access(dir, F_OK) != 0)
            break;
        const char *sensor = read_line(dir, "name");
        for (int t = 1; t <= MAX_TEMP_INDEX && count < SYSINFO_MAX_TEMPS; t++) {
            char input[32], label_file[32];
            snprintf(input, sizeof input, "temp%d_input", t);
            snprintf(label_file, sizeof label_file, "temp%d_label", t);
            const char *v = read_line(dir, input);
            if (v == NULL)
                continue;
            char *end;
            errno = 0;
            long milli = strtol(v, &end, 10);
            if (errno != 0 || end == v || *end != '\0' || milli < -100000 || milli > 300000)
                continue; /* a sensor that is not ready */
            const char *label = read_line(dir, label_file);
            cJSON *o = cJSON_CreateObject();
            if (o == NULL ||
                cJSON_AddStringToObject(o, "sensor", sensor != NULL ? sensor : "?") == NULL ||
                (label != NULL ? cJSON_AddStringToObject(o, "label", label)
                               : cJSON_AddNullToObject(o, "label")) == NULL ||
                cJSON_AddNumberToObject(o, "celsius", (double)milli / 1000.0) == NULL ||
                !cJSON_AddItemToArray(list, o))
                return NULL;
            count++;
        }
    }
    return list;
}
