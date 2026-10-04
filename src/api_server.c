/*
 * Server maintenance: the machine's health (host, disks, services,
 * containers, network), its package updates, and backups.
 *
 * Status that needs no root is read here from /proc, /sys and statvfs()
 * (parsed by src/sysinfo.c). Anything that needs root is a root action
 * (src/action.c); anything that changes the machine asks for the password
 * again and is recorded in the server app's audit table.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "api.h"
#include "db.h"
#include "json.h"
#include "sysinfo.h"

#define MAX_AUDIT 100 /* actions listed */

/* ---- helpers ------------------------------------------------------------ */

/* 1 if the running kernel's modules are gone: a newer kernel was
 * installed, and only a reboot loads it (and its modules). */
static int reboot_needed(const char *release)
{
    char path[512];
    struct stat sb;
    int n = snprintf(path, sizeof path, "/usr/lib/modules/%s", release);
    if (n < 0 || (size_t)n >= sizeof path)
        return 0;
    if (stat(path, &sb) == 0)
        return 0;
    if (errno != ENOENT) {
        fprintf(stderr, "server: %s: %s\n", path, strerror(errno));
        return 0;
    }
    return 1;
}

/* Adds size, used and free (bytes free for users) to each mount; null if
 * statvfs fails (logged). 0, or -1 when out of memory. */
static int add_usage(cJSON *mounts)
{
    cJSON *m;
    cJSON_ArrayForEach(m, mounts) {
        const char *path = cJSON_GetObjectItemCaseSensitive(m, "path")->valuestring;
        struct statvfs sv;
        if (statvfs(path, &sv) != 0) {
            fprintf(stderr, "server: statvfs %s: %s\n", path, strerror(errno));
            if (cJSON_AddNullToObject(m, "size") == NULL ||
                cJSON_AddNullToObject(m, "used") == NULL ||
                cJSON_AddNullToObject(m, "free") == NULL)
                return -1;
            continue;
        }
        double unit = (double)sv.f_frsize;
        if (cJSON_AddNumberToObject(m, "size", (double)sv.f_blocks * unit) == NULL ||
            cJSON_AddNumberToObject(m, "used", (double)(sv.f_blocks - sv.f_bfree) * unit) ==
                NULL ||
            cJSON_AddNumberToObject(m, "free", (double)sv.f_bavail * unit) == NULL)
            return -1;
    }
    return 0;
}

/* ---- host --------------------------------------------------------------- */

/*
 * GET /api/server: the host: name, kernel, uptime, load, CPUs, memory,
 * temperatures, whether a reboot is needed, and the disks' filesystems.
 */
void server_overview(struct request *req, struct response *res)
{
    (void)req;
    struct utsname u;
    if (uname(&u) != 0) {
        fprintf(stderr, "server: uname: %s\n", strerror(errno));
        json_error(res, 500, "internal error");
        return;
    }
    const char *uptime_text = sysinfo_read_file("/proc/uptime", 256);
    const char *load_text = sysinfo_read_file("/proc/loadavg", 256);
    const char *mem_text = sysinfo_read_file("/proc/meminfo", 64 * 1024);
    const char *mounts_text = sysinfo_read_file("/proc/self/mounts", 1024 * 1024);
    long long uptime = uptime_text != NULL ? sysinfo_uptime(uptime_text) : -1;
    cJSON *load = load_text != NULL ? sysinfo_load(load_text) : NULL;
    cJSON *memory = mem_text != NULL ? sysinfo_memory(mem_text) : NULL;
    cJSON *mounts = mounts_text != NULL ? sysinfo_mounts(mounts_text) : NULL;
    cJSON *temps = sysinfo_temperatures("/sys/class/hwmon");
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (uptime < 0 || load == NULL || memory == NULL || mounts == NULL || temps == NULL) {
        fprintf(stderr, "server: can not read /proc (uptime, loadavg, meminfo or mounts)\n");
        json_error(res, 500, "can not read the host's status; see the server log");
        return;
    }

    cJSON *out = cJSON_CreateObject();
    if (out == NULL || add_usage(mounts) != 0 ||
        cJSON_AddStringToObject(out, "hostname", u.nodename) == NULL ||
        cJSON_AddStringToObject(out, "kernel", u.release) == NULL ||
        cJSON_AddNumberToObject(out, "uptime", (double)uptime) == NULL ||
        !cJSON_AddItemToObject(out, "load", load) ||
        cJSON_AddNumberToObject(out, "cpus", cpus > 0 ? (double)cpus : 1) == NULL ||
        !cJSON_AddItemToObject(out, "memory", memory) ||
        !cJSON_AddItemToObject(out, "temperatures", temps) ||
        cJSON_AddBoolToObject(out, "reboot_needed", reboot_needed(u.release)) == NULL ||
        !cJSON_AddItemToObject(out, "mounts", mounts)) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, out);
}

/* GET /api/server/audit: the latest actions started from this app. */
void server_audit(struct request *req, struct response *res)
{
    (void)req;
    sqlite3_stmt *st = db_prepare(server_db, "SELECT at, client, action, detail, result"
                                             " FROM audit ORDER BY id DESC LIMIT ?");
    cJSON *out = cJSON_CreateObject();
    cJSON *list = out != NULL ? cJSON_AddArrayToObject(out, "actions") : NULL;
    int rc = SQLITE_ERROR;
    if (st != NULL && list != NULL && sqlite3_bind_int(st, 1, MAX_AUDIT) == SQLITE_OK) {
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            cJSON *row = json_row(st, 5);
            if (row == NULL || !cJSON_AddItemToArray(list, row)) {
                rc = SQLITE_NOMEM;
                break;
            }
        }
    }
    if (rc != SQLITE_DONE)
        db_log_error(server_db, "audit list");
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, out);
}
