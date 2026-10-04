#ifndef SYSINFO_H
#define SYSINFO_H

#include <stddef.h>

#include <cjson/cJSON.h>

/*
 * The server app's facts (src/api_server.c): parsers of what /proc, /sys
 * and the root actions print. They take text, so tests can give them
 * samples, and build cJSON (in the arena). NULL means the text is not what
 * was expected, or out of memory.
 */

/* The whole file (at most max bytes) NUL-terminated in the arena; NULL if it
 * can not be read or is larger (logged unless it does not exist). */
char *sysinfo_read_file(const char *path, size_t max);

/* /proc/meminfo -> {total, available, swap_total, swap_free} in bytes. */
cJSON *sysinfo_memory(const char *meminfo);

/* /proc/uptime -> whole seconds since boot, or -1. */
long long sysinfo_uptime(const char *text);

/* /proc/loadavg -> [1, 5, 15 minute load]. */
cJSON *sysinfo_load(const char *text);

/*
 * /proc/self/mounts -> [{path, device, type}]: the filesystems on disks
 * (ext4, btrfs, xfs, vfat, ...), each device once (its first mount), at
 * most SYSINFO_MAX_MOUNTS. Pseudo and network filesystems are left out:
 * statvfs() on a network mount that is gone would hang the server.
 */
#define SYSINFO_MAX_MOUNTS 64
cJSON *sysinfo_mounts(const char *text);

/* Undoes /proc/mounts escapes (\040 space, \011 tab, \012 newline, \134
 * backslash) in place. 0, or -1 for a bad escape. */
int sysinfo_unescape(char *s);

/*
 * Temperatures from a hwmon folder (/sys/class/hwmon): [{sensor, label,
 * celsius}] for each temp*_input of each hwmon*, at most
 * SYSINFO_MAX_TEMPS. A folder that can not be read gives [].
 */
#define SYSINFO_MAX_TEMPS 64
cJSON *sysinfo_temperatures(const char *hwmon_dir);

#endif
