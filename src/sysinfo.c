#define _POSIX_C_SOURCE 200809L

#include "sysinfo.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
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

/* ---- configuration ------------------------------------------------------ */

static int is_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

int sysinfo_unit_valid(const char *s)
{
    if (s == NULL || !is_alnum(s[0]))
        return 0;
    size_t n = 0;
    for (; s[n] != '\0'; n++)
        if (n >= 128 || !(is_alnum(s[n]) || strchr("@._:-", s[n]) != NULL))
            return 0;
    return 1;
}

int sysinfo_entry_valid(const char *s)
{
    if (s == NULL || !is_alnum(s[0]))
        return 0;
    size_t n = 0;
    for (; s[n] != '\0'; n++)
        if (n >= 32 || !((s[n] >= 'a' && s[n] <= 'z') || (s[n] >= '0' && s[n] <= '9') ||
                         s[n] == '-'))
            return 0;
    return 1;
}

int sysinfo_path_valid(const char *s)
{
    if (s == NULL || s[0] != '/' || s[1] == '\0' || strlen(s) > SYSINFO_MAX_PATH)
        return 0;
    for (const char *p = s; *p != '\0'; p++)
        if (!(is_alnum(*p) || strchr("/._-", *p) != NULL))
            return 0;
    /* Each part after a '/': not empty, not "." or "..". */
    for (const char *part = s + 1;; ) {
        const char *end = strchr(part, '/');
        size_t len = end != NULL ? (size_t)(end - part) : strlen(part);
        if (len == 0 || (len == 1 && part[0] == '.') ||
            (len == 2 && part[0] == '.' && part[1] == '.'))
            return 0;
        if (end == NULL)
            return 1;
        part = end + 1;
    }
}

/* Splits s at spaces, tabs and newlines into words in the arena; at most
 * max (more: -1). The number of words, or -1. */
static int words(const char *s, char **out, int max)
{
    int n = 0;
    if (s == NULL)
        return 0;
    while (*s != '\0') {
        s += strspn(s, " \t\n");
        size_t len = strcspn(s, " \t\n");
        if (len == 0)
            break;
        if (n == max)
            return -1;
        if ((out[n++] = arena_strndup(s, len)) == NULL)
            return -1;
        s += len;
    }
    return n;
}

/* 1 if list (strings, or objects with "name") has name. */
static int listed(const cJSON *list, const char *name)
{
    const cJSON *item;
    cJSON_ArrayForEach(item, list) {
        const cJSON *v = cJSON_IsObject(item) ? cJSON_GetObjectItemCaseSensitive(item, "name")
                                              : item;
        if (cJSON_IsString(v) && strcmp(v->valuestring, name) == 0)
            return 1;
    }
    return 0;
}

char *sysinfo_unit_full(const char *name)
{
    static const char *const types[] = {
        ".service", ".socket", ".device", ".mount", ".automount", ".swap",
        ".target",  ".path",   ".timer",  ".slice", ".scope",
    };
    size_t len = strlen(name);
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++) {
        size_t t = strlen(types[i]);
        if (len > t && strcmp(name + len - t, types[i]) == 0)
            return arena_strndup(name, len);
    }
    char *full = arena_alloc(len + sizeof ".service");
    if (full != NULL)
        snprintf(full, len + sizeof ".service", "%s.service", name);
    return full;
}

cJSON *sysinfo_unit_list(const char *s, const char **why)
{
    char *w[SYSINFO_MAX_UNITS];
    int n = words(s, w, SYSINFO_MAX_UNITS);
    cJSON *list = cJSON_CreateArray();
    *why = "out of memory";
    if (n < 0 || list == NULL) {
        if (n < 0)
            *why = "NYLM_UNITS: more than 32 units";
        return NULL;
    }
    for (int i = 0; i < n; i++) {
        char *name = sysinfo_unit_full(w[i]);
        if (name == NULL || !sysinfo_unit_valid(name)) {
            *why = "NYLM_UNITS: a unit name may only have A-Z a-z 0-9 @ . _ : - (at most 128)";
            return NULL;
        }
        if (listed(list, name)) {
            *why = "NYLM_UNITS: a unit is listed twice";
            return NULL;
        }
        cJSON *v = cJSON_CreateString(name);
        if (v == NULL || !cJSON_AddItemToArray(list, v))
            return NULL;
    }
    return list;
}

/* One entry "name=path[,path...]" -> {name, paths}; NULL and why if invalid. */
static cJSON *backup_entry(char *word, const char **why)
{
    char *eq = strchr(word, '=');
    *why = "NYLM_BACKUP: each entry is name=/path[,/path...]";
    if (eq == NULL)
        return NULL;
    *eq = '\0';
    if (!sysinfo_entry_valid(word)) {
        *why = "NYLM_BACKUP: a name may only have a-z 0-9 - (at most 32)";
        return NULL;
    }
    if (strcmp(word, "nylm") == 0) {
        *why = "NYLM_BACKUP: the name nylm is nylm's own data, which is always there";
        return NULL;
    }
    cJSON *entry = cJSON_CreateObject();
    cJSON *paths = entry != NULL ? cJSON_AddArrayToObject(entry, "paths") : NULL;
    if (paths == NULL || cJSON_AddStringToObject(entry, "name", word) == NULL) {
        *why = "out of memory";
        return NULL;
    }
    int count = 0;
    for (char *p = eq + 1;;) {
        char *comma = strchr(p, ',');
        if (comma != NULL)
            *comma = '\0';
        if (!sysinfo_path_valid(p)) {
            *why = "NYLM_BACKUP: a path must be absolute, of A-Z a-z 0-9 / . _ - only, "
                   "without . or .. parts or a trailing /";
            return NULL;
        }
        if (listed(paths, p)) {
            *why = "NYLM_BACKUP: a path is listed twice in an entry";
            return NULL;
        }
        if (++count > SYSINFO_MAX_ENTRY_PATHS) {
            *why = "NYLM_BACKUP: more than 16 paths in an entry";
            return NULL;
        }
        cJSON *v = cJSON_CreateString(p);
        if (v == NULL || !cJSON_AddItemToArray(paths, v)) {
            *why = "out of memory";
            return NULL;
        }
        if (comma == NULL)
            return entry;
        p = comma + 1;
    }
}

cJSON *sysinfo_backup_entries(const char *s, const char **why)
{
    char *w[SYSINFO_MAX_ENTRIES];
    int n = words(s, w, SYSINFO_MAX_ENTRIES);
    cJSON *list = cJSON_CreateArray();
    *why = "out of memory";
    if (n < 0 || list == NULL) {
        if (n < 0)
            *why = "NYLM_BACKUP: more than 32 entries";
        return NULL;
    }
    for (int i = 0; i < n; i++) {
        cJSON *entry = backup_entry(w[i], why);
        if (entry == NULL)
            return NULL;
        if (listed(list, cJSON_GetObjectItemCaseSensitive(entry, "name")->valuestring)) {
            *why = "NYLM_BACKUP: a name is used twice";
            return NULL;
        }
        if (!cJSON_AddItemToArray(list, entry)) {
            *why = "out of memory";
            return NULL;
        }
    }
    return list;
}

/* ---- systemd and jobs ---------------------------------------------------- */

/* "@1696412345" -> that number; "" or anything else -> null. */
static cJSON *timestamp(const char *v)
{
    if (v[0] != '@')
        return cJSON_CreateNull();
    char *end;
    errno = 0;
    long long t = strtoll(v + 1, &end, 10);
    if (errno != 0 || end == v + 1 || *end != '\0' || t <= 0)
        return cJSON_CreateNull();
    return cJSON_CreateNumber((double)t);
}

/* Adds one KEY=VALUE line of systemctl show to unit. 0, or -1 if out of
 * memory. Keys nylm does not use are skipped. */
static int unit_property(cJSON *unit, const char *key, const char *value)
{
    static const struct {
        const char *key, *name;
        char kind; /* s: text, i: number, t: timestamp */
    } props[] = {
        { "Id", "unit", 's' },          { "Description", "description", 's' },
        { "LoadState", "load", 's' },   { "ActiveState", "active", 's' },
        { "SubState", "sub", 's' },     { "Result", "result", 's' },
        { "Type", "type", 's' },
        { "ExecMainStatus", "status", 'i' },
        { "ExecMainStartTimestamp", "started", 't' },
        { "ExecMainExitTimestamp", "ended", 't' },
        { "ActiveEnterTimestamp", "since", 't' },
    };
    for (size_t i = 0; i < sizeof props / sizeof props[0]; i++) {
        if (strcmp(key, props[i].key) != 0)
            continue;
        cJSON *v;
        if (props[i].kind == 's') {
            v = cJSON_CreateString(value);
        } else if (props[i].kind == 't') {
            v = timestamp(value);
        } else {
            char *end;
            errno = 0;
            long n = strtol(value, &end, 10);
            v = errno == 0 && end != value && *end == '\0' ? cJSON_CreateNumber((double)n)
                                                          : cJSON_CreateNull();
        }
        cJSON_DeleteItemFromObjectCaseSensitive(unit, props[i].name);
        return v != NULL && cJSON_AddItemToObject(unit, props[i].name, v) ? 0 : -1;
    }
    return 0;
}

cJSON *sysinfo_units(const char *text)
{
    cJSON *list = cJSON_CreateArray();
    cJSON *unit = NULL;
    if (list == NULL)
        return NULL;
    for (const char *line = text; *line != '\0';) {
        const char *nl = strchr(line, '\n');
        size_t len = nl != NULL ? (size_t)(nl - line) : strlen(line);
        if (len > LINE_MAX_LEN)
            return NULL;
        char *copy = arena_strndup(line, len);
        if (copy == NULL)
            return NULL;
        line += len + (nl != NULL);
        if (len == 0) { /* between units */
            unit = NULL;
            continue;
        }
        char *eq = strchr(copy, '=');
        if (eq == NULL)
            return NULL;
        *eq = '\0';
        if (unit == NULL &&
            ((unit = cJSON_CreateObject()) == NULL || !cJSON_AddItemToArray(list, unit)))
            return NULL;
        if (unit_property(unit, copy, eq + 1) != 0)
            return NULL;
    }
    /* Every unit must say which it is. */
    cJSON_ArrayForEach(unit, list)
        if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(unit, "unit")))
            return NULL;
    return list;
}

cJSON *sysinfo_du(const char *log)
{
    static const char marker[] = "nylm-du\t";
    cJSON *list = cJSON_CreateArray();
    if (list == NULL)
        return NULL;
    for (const char *p = strstr(log, marker); p != NULL; p = strstr(p, marker)) {
        p += sizeof marker - 1;
        size_t len = strcspn(p, "\n");
        char *copy = arena_strndup(p, len);
        if (copy == NULL)
            return NULL;
        char *save = NULL;
        char *bytes = strtok_r(copy, "\t", &save);
        char *label = bytes != NULL ? strtok_r(NULL, "\t", &save) : NULL;
        char *path = label != NULL ? strtok_r(NULL, "\t", &save) : NULL;
        if (path == NULL || strtok_r(NULL, "\t", &save) != NULL)
            continue; /* not a line of ours */
        char *end;
        errno = 0;
        long long n = strtoll(bytes, &end, 10);
        if (errno != 0 || end == bytes || *end != '\0' || n < 0)
            continue;
        cJSON *o = cJSON_CreateObject();
        if (o == NULL || cJSON_AddStringToObject(o, "label", label) == NULL ||
            cJSON_AddStringToObject(o, "path", path) == NULL ||
            cJSON_AddNumberToObject(o, "bytes", (double)n) == NULL ||
            !cJSON_AddItemToArray(list, o))
            return NULL;
    }
    return list;
}

int sysinfo_locked(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT)
            return 0;
        fprintf(stderr, "sysinfo: %s: %s\n", path, strerror(errno));
        return -1;
    }
    int locked = 0;
    if (flock(fd, LOCK_SH | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK) {
            locked = 1;
        } else {
            fprintf(stderr, "sysinfo: flock %s: %s\n", path, strerror(errno));
            locked = -1;
        }
    }
    close(fd); /* also drops our shared lock */
    return locked;
}

/* ---- disks --------------------------------------------------------------- */

/* obj's item at a path of keys (NULL-terminated), or NULL. */
static const cJSON *dig(const cJSON *obj, const char *const *keys)
{
    for (; *keys != NULL && obj != NULL; keys++)
        obj = cJSON_GetObjectItemCaseSensitive(obj, *keys);
    return obj;
}

/* Adds key: the number at keys in report (null if absent). 0 or -1. */
static int copy_number(cJSON *out, const char *key, const cJSON *report,
                       const char *const *keys)
{
    const cJSON *v = dig(report, keys);
    return (cJSON_IsNumber(v) ? cJSON_AddNumberToObject(out, key, v->valuedouble)
                              : cJSON_AddNullToObject(out, key)) != NULL ? 0 : -1;
}

/* Adds key: the raw value of ATA attribute id (null if the disk has none). */
static int ata_attribute(cJSON *out, const char *key, const cJSON *report, int id)
{
    static const char *const table_keys[] = { "ata_smart_attributes", "table", NULL };
    static const char *const raw_keys[] = { "raw", "value", NULL };
    const cJSON *row;
    cJSON_ArrayForEach(row, dig(report, table_keys)) {
        const cJSON *rid = cJSON_GetObjectItemCaseSensitive(row, "id");
        if (cJSON_IsNumber(rid) && rid->valueint == id)
            return copy_number(out, key, row, raw_keys);
    }
    return cJSON_AddNullToObject(out, key) != NULL ? 0 : -1;
}

/* Adds key: the string at keys in report (null if absent). 0 or -1. */
static int copy_string(cJSON *out, const char *key, const cJSON *report,
                       const char *const *keys)
{
    const cJSON *v = dig(report, keys);
    return (cJSON_IsString(v) ? cJSON_AddStringToObject(out, key, v->valuestring)
                              : cJSON_AddNullToObject(out, key)) != NULL ? 0 : -1;
}

/* One smartctl report -> what the page shows; NULL if out of memory. */
static cJSON *smart_disk(const cJSON *report)
{
    static const char *const device[] = { "device", "name", NULL };
    static const char *const model[] = { "model_name", NULL };
    static const char *const protocol[] = { "device", "protocol", NULL };
    static const char *const size[] = { "user_capacity", "bytes", NULL };
    static const char *const temp[] = { "temperature", "current", NULL };
    static const char *const hours[] = { "power_on_time", "hours", NULL };
    static const char *const used[] = { "nvme_smart_health_information_log",
                                        "percentage_used", NULL };
    static const char *const spare[] = { "nvme_smart_health_information_log",
                                         "available_spare", NULL };
    static const char *const media[] = { "nvme_smart_health_information_log",
                                         "media_errors", NULL };
    static const char *const warning[] = { "nvme_smart_health_information_log",
                                           "critical_warning", NULL };
    static const char *const passed_keys[] = { "smart_status", "passed", NULL };
    static const char *const messages[] = { "smartctl", "messages", NULL };

    cJSON *out = cJSON_CreateObject();
    if (out == NULL || copy_string(out, "device", report, device) != 0 ||
        copy_string(out, "model", report, model) != 0 ||
        copy_string(out, "protocol", report, protocol) != 0 ||
        copy_number(out, "size", report, size) != 0)
        return NULL;
    const cJSON *passed = dig(report, passed_keys);
    if ((cJSON_IsBool(passed) ? cJSON_AddBoolToObject(out, "passed", cJSON_IsTrue(passed))
                              : cJSON_AddNullToObject(out, "passed")) == NULL ||
        copy_number(out, "temperature", report, temp) != 0 ||
        copy_number(out, "hours", report, hours) != 0 ||
        ata_attribute(out, "reallocated", report, 5) != 0 ||
        ata_attribute(out, "pending", report, 197) != 0 ||
        ata_attribute(out, "uncorrectable", report, 198) != 0 ||
        copy_number(out, "percent_used", report, used) != 0 ||
        copy_number(out, "spare", report, spare) != 0 ||
        copy_number(out, "media_errors", report, media) != 0 ||
        copy_number(out, "critical_warning", report, warning) != 0)
        return NULL;
    /* smartctl's first error message, e.g. a disk asleep or without SMART. */
    const char *error = NULL;
    const cJSON *m;
    cJSON_ArrayForEach(m, dig(report, messages)) {
        const cJSON *sev = cJSON_GetObjectItemCaseSensitive(m, "severity");
        const cJSON *s = cJSON_GetObjectItemCaseSensitive(m, "string");
        if (error == NULL && cJSON_IsString(s) && cJSON_IsString(sev) &&
            strcmp(sev->valuestring, "error") == 0)
            error = s->valuestring;
    }
    if ((error != NULL ? cJSON_AddStringToObject(out, "error", error)
                       : cJSON_AddNullToObject(out, "error")) == NULL)
        return NULL;
    return out;
}

cJSON *sysinfo_smart(const char *text)
{
    cJSON *reports = cJSON_Parse(text);
    cJSON *list = cJSON_CreateArray();
    if (!cJSON_IsArray(reports) || list == NULL)
        return NULL;
    const cJSON *report;
    int count = 0;
    cJSON_ArrayForEach(report, reports) {
        if (!cJSON_IsObject(report))
            return NULL;
        if (count++ == SYSINFO_MAX_DISKS)
            break;
        cJSON *disk = smart_disk(report);
        if (disk == NULL || !cJSON_AddItemToArray(list, disk))
            return NULL;
    }
    return list;
}

/* ---- containers ---------------------------------------------------------- */

int sysinfo_container_valid(const char *s)
{
    if (s == NULL || !is_alnum(s[0]))
        return 0;
    size_t n = 0;
    for (; s[n] != '\0'; n++)
        if (n >= 128 || !(is_alnum(s[n]) || strchr("_.-", s[n]) != NULL))
            return 0;
    return 1;
}

/* Days from 1970-01-01 to y-m-d (proleptic Gregorian; Howard Hinnant's
 * days_from_civil). */
static long long days_from_civil(long long y, long long m, long long d)
{
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* n digits at s as a number, or -1. */
static long long digits(const char *s, int n)
{
    long long v = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

long long sysinfo_docker_time(const char *s)
{
    /* YYYY-MM-DDTHH:MM:SS[.fraction]Z */
    if (s == NULL || strlen(s) < 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' ||
        s[13] != ':' || s[16] != ':')
        return -1;
    long long y = digits(s, 4), mo = digits(s + 5, 2), d = digits(s + 8, 2),
              h = digits(s + 11, 2), mi = digits(s + 14, 2), se = digits(s + 17, 2);
    const char *p = s + 19;
    if (*p == '.')
        for (p++; *p >= '0' && *p <= '9'; p++)
            ;
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 ||
        mi > 59 || se < 0 || se > 60 || strcmp(p, "Z") != 0)
        return -1;
    return days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
}

double sysinfo_docker_size(const char *s)
{
    static const struct {
        const char *unit;
        double factor;
    } units[] = {
        { "B", 1 },          { "kB", 1e3 },           { "KB", 1e3 },
        { "MB", 1e6 },       { "GB", 1e9 },           { "TB", 1e12 },
        { "KiB", 1024.0 },   { "MiB", 1048576.0 },    { "GiB", 1073741824.0 },
        { "TiB", 1099511627776.0 },
    };
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (errno != 0 || end == s || !(v >= 0 && v < 1e15))
        return -1;
    for (size_t i = 0; i < sizeof units / sizeof units[0]; i++)
        if (strcmp(end, units[i].unit) == 0)
            return v * units[i].factor;
    return -1;
}

/* "12.34%" -> 12.34; -1 if not a percentage (e.g. "--"). */
static double docker_percent(const char *s)
{
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (errno != 0 || end == s || strcmp(end, "%") != 0 || !(v >= 0 && v < 1e6))
        return -1;
    return v;
}

/* Adds key: n, or null when n < 0. 0, or -1 if out of memory. */
static int number_or_null(cJSON *o, const char *key, double n)
{
    return (n >= 0 ? cJSON_AddNumberToObject(o, key, n) : cJSON_AddNullToObject(o, key)) != NULL
               ? 0
               : -1;
}

/* Adds key: the string item of c (or null; "" counts as null). */
static int string_or_null(cJSON *o, const char *key, const cJSON *c)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(c, key);
    return (cJSON_IsString(v) && v->valuestring[0] != '\0'
                ? cJSON_AddStringToObject(o, key, v->valuestring)
                : cJSON_AddNullToObject(o, key)) != NULL
               ? 0
               : -1;
}

/* Docker's NetworkSettings.Ports ({"80/tcp": [{"HostIp", "HostPort"}] or
 * null}) -> [{container: "80/tcp", host: "0.0.0.0:9000" or null}]. */
static cJSON *ports(const cJSON *map)
{
    cJSON *list = cJSON_CreateArray();
    const cJSON *port;
    if (list == NULL)
        return NULL;
    cJSON_ArrayForEach(port, map) {
        const cJSON *binding;
        int bound = 0;
        cJSON_ArrayForEach(binding, port) {
            const cJSON *ip = cJSON_GetObjectItemCaseSensitive(binding, "HostIp");
            const cJSON *hp = cJSON_GetObjectItemCaseSensitive(binding, "HostPort");
            if (!cJSON_IsString(ip) || !cJSON_IsString(hp))
                continue;
            char host[128];
            snprintf(host, sizeof host, strchr(ip->valuestring, ':') ? "[%s]:%s" : "%s:%s",
                     ip->valuestring[0] != '\0' ? ip->valuestring : "0.0.0.0", hp->valuestring);
            cJSON *p = cJSON_CreateObject();
            if (p == NULL || cJSON_AddStringToObject(p, "container", port->string) == NULL ||
                cJSON_AddStringToObject(p, "host", host) == NULL || !cJSON_AddItemToArray(list, p))
                return NULL;
            bound = 1;
        }
        if (!bound) { /* exposed, not published */
            cJSON *p = cJSON_CreateObject();
            if (p == NULL || cJSON_AddStringToObject(p, "container", port->string) == NULL ||
                cJSON_AddNullToObject(p, "host") == NULL || !cJSON_AddItemToArray(list, p))
                return NULL;
        }
    }
    return list;
}

/* The stats of the container named name ("/name" in inspect), or NULL. */
static const cJSON *stats_of(const cJSON *stats, const char *name)
{
    const cJSON *s;
    if (name[0] == '/')
        name++;
    cJSON_ArrayForEach(s, stats) {
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(s, "name");
        if (cJSON_IsString(n) && strcmp(n->valuestring, name) == 0)
            return s;
    }
    return NULL;
}

/* One container of docker-list -> what the page shows; NULL if invalid. */
static cJSON *container(const cJSON *c, const cJSON *stats)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(c, "name");
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(c, "state");
    const cJSON *started = cJSON_GetObjectItemCaseSensitive(c, "started");
    const cJSON *finished = cJSON_GetObjectItemCaseSensitive(c, "finished");
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(c, "exit_code");
    const cJSON *restarts = cJSON_GetObjectItemCaseSensitive(c, "restarts");
    if (!cJSON_IsString(name) || !cJSON_IsString(state) || !cJSON_IsString(started) ||
        !cJSON_IsString(finished) || !cJSON_IsNumber(code) || !cJSON_IsNumber(restarts))
        return NULL;
    const char *shown = name->valuestring[0] == '/' ? name->valuestring + 1 : name->valuestring;
    const cJSON *s = strcmp(state->valuestring, "running") == 0 ? stats_of(stats, shown) : NULL;
    const cJSON *cpu = cJSON_GetObjectItemCaseSensitive(s, "cpu");
    const cJSON *mem = cJSON_GetObjectItemCaseSensitive(s, "memory");
    const cJSON *mem_pct = cJSON_GetObjectItemCaseSensitive(s, "memory_percent");
    double mem_bytes = -1;
    if (cJSON_IsString(mem)) {
        /* "12.5MiB / 31.3GiB": what it uses, then its limit. */
        char *used = arena_strndup(mem->valuestring, strcspn(mem->valuestring, " "));
        if (used == NULL)
            return NULL;
        mem_bytes = sysinfo_docker_size(used);
    }
    cJSON *list = ports(cJSON_GetObjectItemCaseSensitive(c, "ports"));
    cJSON *o = cJSON_CreateObject();
    if (list == NULL || o == NULL || cJSON_AddStringToObject(o, "name", shown) == NULL ||
        string_or_null(o, "image", c) != 0 ||
        cJSON_AddStringToObject(o, "state", state->valuestring) == NULL ||
        string_or_null(o, "health", c) != 0 ||
        number_or_null(o, "started", (double)sysinfo_docker_time(started->valuestring)) != 0 ||
        number_or_null(o, "finished", (double)sysinfo_docker_time(finished->valuestring)) != 0 ||
        cJSON_AddNumberToObject(o, "exit_code", code->valuedouble) == NULL ||
        cJSON_AddNumberToObject(o, "restarts", restarts->valuedouble) == NULL ||
        !cJSON_AddItemToObject(o, "ports", list) || string_or_null(o, "project", c) != 0 ||
        string_or_null(o, "service", c) != 0 ||
        number_or_null(o, "cpu", cJSON_IsString(cpu) ? docker_percent(cpu->valuestring) : -1) !=
            0 ||
        number_or_null(o, "memory", mem_bytes) != 0 ||
        number_or_null(o, "memory_percent",
                       cJSON_IsString(mem_pct) ? docker_percent(mem_pct->valuestring) : -1) != 0)
        return NULL;
    return o;
}

/* qsort: containers by name. */
static int by_name(const void *a, const void *b)
{
    const cJSON *x = *(const cJSON *const *)a, *y = *(const cJSON *const *)b;
    return strcmp(cJSON_GetObjectItemCaseSensitive(x, "name")->valuestring,
                  cJSON_GetObjectItemCaseSensitive(y, "name")->valuestring);
}

cJSON *sysinfo_containers(const char *text)
{
    cJSON *all = cJSON_Parse(text);
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(all, "containers");
    const cJSON *stats = cJSON_GetObjectItemCaseSensitive(all, "stats");
    if (!cJSON_IsArray(list) || !cJSON_IsArray(stats))
        return NULL;
    cJSON *items[SYSINFO_MAX_CONTAINERS];
    size_t n = 0;
    const cJSON *c;
    cJSON_ArrayForEach(c, list) {
        if (n == SYSINFO_MAX_CONTAINERS)
            break;
        if ((items[n++] = container(c, stats)) == NULL)
            return NULL;
    }
    qsort(items, n, sizeof items[0], by_name);
    cJSON *out = cJSON_CreateArray();
    for (size_t i = 0; out != NULL && i < n; i++)
        if (!cJSON_AddItemToArray(out, items[i]))
            return NULL;
    return out;
}

/* ---- network ------------------------------------------------------------- */

/* n hex digits at s as a number in *v. 0, or -1. */
static int hex(const char *s, int n, unsigned long *v)
{
    *v = 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10
                : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0)
            return -1;
        *v = *v * 16 + (unsigned long)d;
    }
    return 0;
}

/* /proc's "ADDR:PORT" (ADDR 8 or 32 hex digits, 32-bit words in host
 * order) -> text address and port. 0, or -1. */
static int proc_address(const char *s, int v6, char *out, size_t size, unsigned long *port)
{
    int words = v6 ? 4 : 1;
    unsigned char bytes[16];
    for (int w = 0; w < words; w++) {
        unsigned long v;
        if (hex(s + w * 8, 8, &v) != 0)
            return -1;
        for (int b = 0; b < 4; b++)
            bytes[w * 4 + b] = (unsigned char)(v >> (8 * b));
    }
    const char *colon = s + words * 8;
    if (*colon != ':' || hex(colon + 1, 4, port) != 0 || (colon[5] != ' ' && colon[5] != '\0'))
        return -1;
    return inet_ntop(v6 ? AF_INET6 : AF_INET, bytes, out, (socklen_t)size) != NULL ? 0 : -1;
}

int sysinfo_listening(const char *text, const char *proto, int v6, cJSON *list)
{
    int udp = proto[0] == 'u';
    const char *line = strchr(text, '\n'); /* the first line names the columns */
    if (line == NULL)
        return -1;
    for (line++; *line != '\0';) {
        const char *nl = strchr(line, '\n');
        size_t len = nl != NULL ? (size_t)(nl - line) : strlen(line);
        if (len > LINE_MAX_LEN)
            return -1;
        char *copy = arena_strndup(line, len);
        if (copy == NULL)
            return -1;
        line += len + (nl != NULL);

        char *save = NULL;
        char *sl = strtok_r(copy, " ", &save);
        char *local = sl != NULL ? strtok_r(NULL, " ", &save) : NULL;
        char *remote = local != NULL ? strtok_r(NULL, " ", &save) : NULL;
        char *state = remote != NULL ? strtok_r(NULL, " ", &save) : NULL;
        if (sl == NULL)
            continue;
        if (state == NULL || strlen(local) != (v6 ? 37u : 13u) || strlen(state) != 2)
            return -1;
        /* TCP_LISTEN is 0A; an unconnected UDP socket is TCP_CLOSE, 07. */
        if (strcmp(state, udp ? "07" : "0A") != 0)
            continue;
        char address[64];
        unsigned long port;
        if (proc_address(local, v6, address, sizeof address, &port) != 0)
            return -1;
        if (cJSON_GetArraySize(list) >= SYSINFO_MAX_SOCKETS)
            return -1;
        cJSON *o = cJSON_CreateObject();
        if (o == NULL || cJSON_AddStringToObject(o, "proto", udp ? "udp" : "tcp") == NULL ||
            cJSON_AddStringToObject(o, "address", address) == NULL ||
            cJSON_AddNumberToObject(o, "port", (double)port) == NULL ||
            !cJSON_AddItemToArray(list, o))
            return -1;
    }
    return 0;
}

int sysinfo_wg_key_valid(const char *s)
{
    if (s == NULL || strlen(s) != 44 || s[43] != '=')
        return 0;
    for (int i = 0; i < 43; i++)
        if (!(is_alnum(s[i]) || s[i] == '+' || s[i] == '/'))
            return 0;
    return 1;
}

/* A whole number field of wg's dump; -1 if it is not one. */
static double dump_number(const char *s)
{
    char *end;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    return errno != 0 || end == s || *end != '\0' || v < 0 ? -1 : (double)v;
}

/* One "peer" line's fields (after "peer") -> a peer; NULL if invalid. */
static cJSON *wg_peer(char **f)
{
    /* f: interface, key, endpoint, allowed ips, handshake, rx, tx, keepalive */
    double handshake = dump_number(f[4]), rx = dump_number(f[5]), tx = dump_number(f[6]);
    double keepalive = strcmp(f[7], "off") == 0 ? 0 : dump_number(f[7]);
    if (!sysinfo_unit_valid(f[0]) || !sysinfo_wg_key_valid(f[1]) || handshake < 0 || rx < 0 ||
        tx < 0 || keepalive < 0)
        return NULL;
    cJSON *ips = cJSON_CreateArray();
    if (ips == NULL)
        return NULL;
    if (strcmp(f[3], "(none)") != 0) {
        char *save = NULL;
        for (char *ip = strtok_r(f[3], ",", &save); ip != NULL; ip = strtok_r(NULL, ",", &save)) {
            cJSON *v = cJSON_CreateString(ip);
            if (v == NULL || !cJSON_AddItemToArray(ips, v))
                return NULL;
        }
    }
    cJSON *p = cJSON_CreateObject();
    int none = strcmp(f[2], "(none)") == 0;
    if (p == NULL || cJSON_AddStringToObject(p, "interface", f[0]) == NULL ||
        cJSON_AddStringToObject(p, "public_key", f[1]) == NULL ||
        (none ? cJSON_AddNullToObject(p, "endpoint")
              : cJSON_AddStringToObject(p, "endpoint", f[2])) == NULL ||
        !cJSON_AddItemToObject(p, "allowed_ips", ips) ||
        number_or_null(p, "handshake", handshake > 0 ? handshake : -1) != 0 ||
        cJSON_AddNumberToObject(p, "rx", rx) == NULL ||
        cJSON_AddNumberToObject(p, "tx", tx) == NULL ||
        number_or_null(p, "keepalive", keepalive > 0 ? keepalive : -1) != 0)
        return NULL;
    return p;
}

cJSON *sysinfo_wireguard(const char *text)
{
    cJSON *out = cJSON_CreateObject();
    cJSON *ifaces = out != NULL ? cJSON_AddArrayToObject(out, "interfaces") : NULL;
    cJSON *peers = ifaces != NULL ? cJSON_AddArrayToObject(out, "peers") : NULL;
    if (peers == NULL)
        return NULL;
    for (const char *line = text; *line != '\0';) {
        const char *nl = strchr(line, '\n');
        size_t len = nl != NULL ? (size_t)(nl - line) : strlen(line);
        if (len > LINE_MAX_LEN)
            return NULL;
        char *copy = arena_strndup(line, len);
        if (copy == NULL)
            return NULL;
        line += len + (nl != NULL);
        if (len == 0)
            continue;

        /* Tab-separated; no field is empty. */
        char *f[10];
        int n = 0;
        for (char *p = copy; n < 10;) {
            f[n++] = p;
            char *tab = strchr(p, '\t');
            if (tab == NULL)
                break;
            *tab = '\0';
            p = tab + 1;
        }
        if (n == 4 && strcmp(f[0], "interface") == 0) {
            double port = dump_number(f[3]);
            cJSON *i = cJSON_CreateObject();
            if (!sysinfo_unit_valid(f[1]) || !sysinfo_wg_key_valid(f[2]) || port < 0 ||
                port > 65535 || i == NULL || cJSON_AddStringToObject(i, "name", f[1]) == NULL ||
                cJSON_AddStringToObject(i, "public_key", f[2]) == NULL ||
                cJSON_AddNumberToObject(i, "port", port) == NULL ||
                !cJSON_AddItemToArray(ifaces, i))
                return NULL;
        } else if (n == 9 && strcmp(f[0], "peer") == 0) {
            if (cJSON_GetArraySize(peers) == SYSINFO_MAX_PEERS)
                continue;
            cJSON *p = wg_peer(f + 1);
            if (p == NULL || !cJSON_AddItemToArray(peers, p))
                return NULL;
        } else {
            return NULL;
        }
    }
    return out;
}

/* ---- updates ------------------------------------------------------------- */

char *sysinfo_read_tail(const char *path, size_t max)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "sysinfo: %s: %s\n", path, strerror(errno));
        return NULL;
    }
    struct stat sb;
    char *buf = NULL;
    if (fstat(fd, &sb) != 0 || sb.st_size < 0) {
        fprintf(stderr, "sysinfo: %s: %s\n", path, strerror(errno));
    } else {
        off_t start = (size_t)sb.st_size > max ? sb.st_size - (off_t)max : 0;
        if (lseek(fd, start, SEEK_SET) < 0) {
            fprintf(stderr, "sysinfo: %s: %s\n", path, strerror(errno));
        } else if ((buf = arena_alloc(max + 1)) != NULL) {
            size_t used = 0;
            for (;;) {
                ssize_t n = read(fd, buf + used, max - used);
                if (n < 0 && errno == EINTR)
                    continue;
                if (n < 0) {
                    fprintf(stderr, "sysinfo: %s: %s\n", path, strerror(errno));
                    buf = NULL;
                    break;
                }
                used += (size_t)n;
                if (n == 0 || used == max)
                    break;
            }
            if (buf != NULL)
                buf[used] = '\0';
        }
    }
    close(fd);
    return buf;
}

/* A package name or version: 1..256 of the characters pacman uses. */
static int package_word(const char *s, size_t len)
{
    if (len == 0 || len > 256)
        return 0;
    for (size_t i = 0; i < len; i++)
        if (!(is_alnum(s[i]) || strchr("@._+-:~", s[i]) != NULL))
            return 0;
    return 1;
}

cJSON *sysinfo_updates(const char *log)
{
    cJSON *list = cJSON_CreateArray();
    if (list == NULL)
        return NULL;
    int count = 0;
    for (const char *line = log; *line != '\0';) {
        const char *nl = strchr(line, '\n');
        size_t len = nl != NULL ? (size_t)(nl - line) : strlen(line);
        const char *end = line + len;
        const char *arrow = NULL;
        for (const char *p = line; p + 4 <= end; p++)
            if (memcmp(p, " -> ", 4) == 0)
                arrow = p;
        const char *start = line;
        line = nl != NULL ? nl + 1 : end;
        if (arrow == NULL)
            continue;
        /* "... name old -> new": new after the arrow, old and name before. */
        const char *new_v = arrow + 4;
        const char *old_end = arrow, *old_v = old_end;
        while (old_v > start && old_v[-1] != ' ')
            old_v--;
        if (old_v == start)
            continue;
        const char *name_end = old_v - 1, *name = name_end;
        while (name > start && name[-1] != ' ')
            name--;
        if (!package_word(name, (size_t)(name_end - name)) ||
            !package_word(old_v, (size_t)(old_end - old_v)) ||
            !package_word(new_v, (size_t)(end - new_v)))
            continue;
        if (count++ == SYSINFO_MAX_UPDATES)
            break;
        char *n = arena_strndup(name, (size_t)(name_end - name));
        char *o = arena_strndup(old_v, (size_t)(old_end - old_v));
        char *w = arena_strndup(new_v, (size_t)(end - new_v));
        cJSON *u = cJSON_CreateObject();
        if (n == NULL || o == NULL || w == NULL || u == NULL ||
            cJSON_AddStringToObject(u, "name", n) == NULL ||
            cJSON_AddStringToObject(u, "old", o) == NULL ||
            cJSON_AddStringToObject(u, "new", w) == NULL || !cJSON_AddItemToArray(list, u))
            return NULL;
    }
    return list;
}

long long sysinfo_last_upgrade(const char *text)
{
    static const char marker[] = "] [PACMAN] starting full system upgrade";
    const char *found = NULL;
    for (const char *p = strstr(text, marker); p != NULL; p = strstr(p + 1, marker))
        found = p;
    if (found == NULL)
        return -1;
    /* "[YYYY-MM-DDTHH:MM:SS+HHMM" before the marker. */
    const char *t = found - 25;
    if (found - text < 25 || t[0] != '[')
        return -1;
    t++;
    if (t[4] != '-' || t[7] != '-' || t[10] != 'T' || t[13] != ':' || t[16] != ':' ||
        (t[19] != '+' && t[19] != '-'))
        return -1;
    long long y = digits(t, 4), mo = digits(t + 5, 2), d = digits(t + 8, 2),
              h = digits(t + 11, 2), mi = digits(t + 14, 2), se = digits(t + 17, 2),
              oh = digits(t + 20, 2), om = digits(t + 22, 2);
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 ||
        mi > 59 || se < 0 || se > 60 || oh < 0 || oh > 23 || om < 0 || om > 59)
        return -1;
    long long offset = (oh * 3600 + om * 60) * (t[19] == '+' ? 1 : -1);
    return days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se - offset;
}

/* ---- backups ------------------------------------------------------------- */

int sysinfo_backup_keep(const char *s)
{
    if (s == NULL || s[0] < '1' || s[0] > '9' || strlen(s) > 3)
        return -1;
    long v = 0;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p < '0' || *p > '9')
            return -1;
        v = v * 10 + (*p - '0');
    }
    return v <= 100 ? (int)v : -1;
}

int sysinfo_group_valid(const char *s)
{
    if (s == NULL || !((s[0] >= 'a' && s[0] <= 'z') || s[0] == '_'))
        return 0;
    size_t n = 0;
    for (; s[n] != '\0'; n++)
        if (n >= 32 || !((s[n] >= 'a' && s[n] <= 'z') || (s[n] >= '0' && s[n] <= '9') ||
                         s[n] == '_' || s[n] == '-'))
            return 0;
    return 1;
}

int sysinfo_path_within(const char *a, const char *b)
{
    size_t n = strlen(b);
    return strncmp(a, b, n) == 0 && (a[n] == '\0' || a[n] == '/');
}

long long sysinfo_backup_time(const char *name, const char *file)
{
    size_t n = strlen(name);
    /* name, "-", 16 for the time, ".tar.zst" */
    if (strncmp(file, name, n) != 0 || file[n] != '-' || strlen(file) != n + 1 + 16 + 8 ||
        strcmp(file + n + 17, ".tar.zst") != 0)
        return -1;
    const char *t = file + n + 1;
    if (t[8] != 'T' || t[15] != 'Z')
        return -1;
    long long y = digits(t, 4), mo = digits(t + 4, 2), d = digits(t + 6, 2),
              h = digits(t + 9, 2), mi = digits(t + 11, 2), se = digits(t + 13, 2);
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 ||
        mi > 59 || se < 0 || se > 60)
        return -1;
    return days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
}
