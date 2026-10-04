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

/* ---- configuration ------------------------------------------------------
 * From /etc/nylm.conf. The root scripts (deploy/lib.sh) check the same
 * rules: what nylm accepts, root accepts. */

#define SYSINFO_MAX_UNITS        32
#define SYSINFO_MAX_ENTRIES      32
#define SYSINFO_MAX_ENTRY_PATHS  16
#define SYSINFO_MAX_PATH         1024

/* A systemd unit name: 1..128 of A-Z a-z 0-9 @ . _ : -, starting with a
 * letter or digit. */
int sysinfo_unit_valid(const char *s);

/* A backup entry's name: 1..32 of a-z 0-9 -, starting with a letter or digit. */
int sysinfo_entry_valid(const char *s);

/* An absolute path of A-Z a-z 0-9 / . _ -, at most SYSINFO_MAX_PATH bytes,
 * not "/", without empty, "." or ".." parts and no trailing slash. */
int sysinfo_path_valid(const char *s);

/* The unit's full name as systemd reads it: ".service" added when it has
 * no unit type ("docker" -> "docker.service"). In the arena; NULL if out
 * of memory. */
char *sysinfo_unit_full(const char *name);

/* NYLM_UNITS (unit names separated by spaces) -> [full names]. NULL (and
 * why) if a name is invalid or repeated, or there are too many. */
cJSON *sysinfo_unit_list(const char *s, const char **why);

/* NYLM_BACKUP (entries "name=path[,path...]" separated by spaces) ->
 * [{name, paths: [...]}]. NULL (and why) if anything is invalid, a name
 * is repeated or is "nylm" (nylm's own data), or there are too many. */
cJSON *sysinfo_backup_entries(const char *s, const char **why);

/* ---- systemd and jobs ---------------------------------------------------- */

/*
 * `systemctl show --timestamp=unix -p ...` of one or more units -> [{unit,
 * description, load, active, sub, result, type, status, started, ended,
 * since}],
 * timestamps in unix seconds or null.
 */
cJSON *sysinfo_units(const char *text);

/* The disk usage job's log -> [{label, path, bytes}] from its lines
 * "nylm-du<TAB>bytes<TAB>label<TAB>path" (anything before the marker, such
 * as the journal's prefix, is skipped). */
cJSON *sysinfo_du(const char *log);

/* 1 if someone holds an exclusive flock on path, 0 if not (or path does
 * not exist), -1 on error (logged). */
int sysinfo_locked(const char *path);

/* ---- disks --------------------------------------------------------------- */

/*
 * The action smart's output (a JSON array of `smartctl -j` reports) ->
 * [{device, model, protocol, size, passed, temperature, hours,
 * reallocated, pending, uncorrectable (ATA), percent_used, spare,
 * media_errors, critical_warning (NVMe), error}]: what is unknown is null;
 * error is smartctl's message when it could not read the disk (asleep,
 * no SMART, ...). At most SYSINFO_MAX_DISKS disks.
 */
#define SYSINFO_MAX_DISKS 32
cJSON *sysinfo_smart(const char *text);

/* ---- containers ---------------------------------------------------------- */

/* A container name as Docker allows it: 1..128 of A-Z a-z 0-9 _ . -,
 * starting with a letter or digit (deploy/lib.sh: valid_container). */
int sysinfo_container_valid(const char *s);

/* Docker's time ("2026-10-04T12:00:00.123456789Z", UTC) -> unix seconds;
 * -1 for its "never" (year 1) or anything else. */
long long sysinfo_docker_time(const char *s);

/* Docker's size ("12.5MiB", "1.2GB", "0B") -> bytes; -1 if not one. */
double sysinfo_docker_size(const char *s);

/*
 * The action docker-list's output -> [{name, image, state, health,
 * started, finished, exit_code, restarts, ports: [{container, host}],
 * project, service, cpu, memory, memory_percent}] sorted by name. The
 * times in unix seconds, the use only for running containers (else null).
 * At most SYSINFO_MAX_CONTAINERS.
 */
#define SYSINFO_MAX_CONTAINERS 256
cJSON *sysinfo_containers(const char *text);

/* ---- network ------------------------------------------------------------- */

/*
 * Adds the listening sockets of a /proc/net file (tcp, tcp6, udp, udp6) to
 * list as {proto, address, port}: TCP sockets in LISTEN, UDP sockets that
 * are not connected. v6: the file is tcp6 or udp6. Addresses in /proc are
 * in the host's byte order (little-endian here). 0, or -1 if the text is
 * not what was expected or there are more than SYSINFO_MAX_SOCKETS.
 */
#define SYSINFO_MAX_SOCKETS 1024
int sysinfo_listening(const char *text, const char *proto, int v6, cJSON *list);

/*
 * The action wg-show's output -> {interfaces: [{name, public_key, port}],
 * peers: [{interface, public_key, endpoint, allowed_ips: [...],
 * handshake, rx, tx, keepalive}]}; endpoint, handshake (unix seconds) and
 * keepalive (seconds) are null when there is none. At most
 * SYSINFO_MAX_PEERS peers.
 */
#define SYSINFO_MAX_PEERS 256
cJSON *sysinfo_wireguard(const char *text);

/* A WireGuard public key: 44 characters of base64 ending in '='. */
int sysinfo_wg_key_valid(const char *s);

/* ---- updates ------------------------------------------------------------- */

/* The end of a file (its last max bytes at most), NUL-terminated in the
 * arena; NULL if it can not be read (logged). */
char *sysinfo_read_tail(const char *path, size_t max);

/* The update check's log (checkupdates: "name old -> new" lines, after the
 * journal's prefix) -> [{name, old, new}], at most SYSINFO_MAX_UPDATES. */
#define SYSINFO_MAX_UPDATES 5000
cJSON *sysinfo_updates(const char *log);

/* The time of the last "starting full system upgrade" in pacman.log text
 * ("[2026-10-04T12:00:00+0200] [PACMAN] ..."), unix seconds; -1 if none. */
long long sysinfo_last_upgrade(const char *text);

#endif
