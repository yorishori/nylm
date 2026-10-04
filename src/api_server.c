/*
 * Server maintenance: the machine's health (host, disks, services,
 * containers, network), its package updates, and backups.
 *
 * Status that needs no root is read here from /proc, /sys and statvfs()
 * (parsed by src/sysinfo.c). Anything that needs root is a root action
 * (src/action.c); anything that changes the machine asks for the password
 * again and is recorded in the server app's audit table.
 *
 * Long work (disk usage, updates, backups) is a job: a root script in
 * /usr/local/lib/nylm/jobs/ run as a systemd unit, started by an action.
 * Every root job holds an exclusive flock on that folder while it runs, so
 * only one runs at a time; its state comes from `systemctl show` and its
 * output from the journal (action unit-log).
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "action.h"
#include "api.h"
#include "arena.h"
#include "audit.h"
#include "db.h"
#include "json.h"
#include "sysinfo.h"

#define MAX_AUDIT  100 /* actions listed */
#define JOBS_LOCK  "/usr/local/lib/nylm/jobs" /* see above */
#define LOG_MAX    (2 * 1024 * 1024) /* a unit's log as unit-log prints it */
#define SHOW_MAX   (256 * 1024)      /* systemctl show of the units */
#define SHOW_TIMEOUT 10              /* seconds */

#define BUSY_MESSAGE \
    "another job is running (disk usage, update or backup); try again when it is done"

/* nylm's own units: always shown, their logs always readable. */
static const char *const nylm_units[] = {
    "nylm.service", "nylm-music-scan.service", "nylm-music-write.service",
    "nylm-music-move.service", "nylm-qobuz.service", "nylm-disk-usage.service",
};
#define NNYLM_UNITS (sizeof nylm_units / sizeof nylm_units[0])

/* ---- configuration ------------------------------------------------------ */

/* NYLM_UNITS: the other units to watch; NULL (logged) if invalid. */
static cJSON *watched_units(void)
{
    const char *why;
    cJSON *list = sysinfo_unit_list(getenv("NYLM_UNITS"), &why);
    if (list == NULL)
        fprintf(stderr, "server: %s\n", why);
    return list;
}

/* NYLM_BACKUP: [{name, paths}]; NULL (logged) if invalid. */
static cJSON *backup_entries(void)
{
    const char *why;
    cJSON *list = sysinfo_backup_entries(getenv("NYLM_BACKUP"), &why);
    if (list == NULL)
        fprintf(stderr, "server: %s\n", why);
    return list;
}

int server_configure(void)
{
    const char *dir = getenv("NYLM_BACKUP_DIR");
    if (watched_units() == NULL || backup_entries() == NULL)
        return -1;
    if (dir != NULL && *dir != '\0' && !sysinfo_path_valid(dir)) {
        fprintf(stderr, "server: NYLM_BACKUP_DIR must be an absolute path of A-Z a-z 0-9 "
                        "/ . _ - only\n");
        return -1;
    }
    return 0;
}

/* 1 if nylm shows unit's log: nylm's own, NYLM_UNITS, a backup entry's job. */
static int log_allowed(const char *unit)
{
    for (size_t i = 0; i < NNYLM_UNITS; i++)
        if (strcmp(unit, nylm_units[i]) == 0)
            return 1;
    const cJSON *item;
    cJSON *units = watched_units();
    cJSON_ArrayForEach(item, units)
        if (strcmp(item->valuestring, unit) == 0)
            return 1;
    cJSON *entries = backup_entries();
    cJSON_ArrayForEach(item, entries) {
        char name[64];
        snprintf(name, sizeof name, "nylm-backup@%s.service",
                 cJSON_GetObjectItemCaseSensitive(item, "name")->valuestring);
        if (strcmp(name, unit) == 0)
            return 1;
    }
    return 0;
}

/* ---- helpers ------------------------------------------------------------ */

/* The state of n units (valid names), from systemctl show; NULL (logged)
 * if it fails. */
static cJSON *units_state(const char *const *units, size_t n)
{
    char *argv[16 + SYSINFO_MAX_UNITS + NNYLM_UNITS + SYSINFO_MAX_ENTRIES];
    char systemctl[] = "/usr/bin/systemctl", show[] = "show", ts[] = "--timestamp=unix",
         p[] = "-p", props[] = "Id,Description,LoadState,ActiveState,SubState,Result,"
                               "ExecMainStatus,ExecMainStartTimestamp,ExecMainExitTimestamp,"
                               "ActiveEnterTimestamp",
         dashes[] = "--";
    size_t argc = 0;
    if (n > sizeof argv / sizeof argv[0] - 8)
        return NULL;
    argv[argc++] = systemctl;
    argv[argc++] = show;
    argv[argc++] = ts;
    argv[argc++] = p;
    argv[argc++] = props;
    argv[argc++] = dashes;
    for (size_t i = 0; i < n; i++) {
        if ((argv[argc++] = arena_strndup(units[i], strlen(units[i]))) == NULL)
            return NULL;
    }
    argv[argc] = NULL;
    char *out;
    if (command_output(argv, SHOW_TIMEOUT, SHOW_MAX, &out, NULL, NULL) != 0)
        return NULL;
    cJSON *list = sysinfo_units(out);
    if (list == NULL || cJSON_GetArraySize(list) != (int)n) {
        fprintf(stderr, "server: unexpected output of systemctl show\n");
        return NULL;
    }
    return list;
}

/* The state of one unit, or NULL (logged). */
static cJSON *unit_state(const char *unit)
{
    cJSON *list = units_state(&unit, 1);
    return list != NULL ? cJSON_DetachItemFromArray(list, 0) : NULL;
}

/* 1 if the unit (from units_state) is starting or running. */
static int unit_running(const cJSON *state)
{
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(state, "active");
    return cJSON_IsString(a) && (strcmp(a->valuestring, "active") == 0 ||
                                 strcmp(a->valuestring, "activating") == 0 ||
                                 strcmp(a->valuestring, "deactivating") == 0 ||
                                 strcmp(a->valuestring, "reloading") == 0);
}

/* 1 if the unit (from units_state) has run since boot and is done. */
static int unit_ran(const cJSON *state)
{
    return cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(state, "started")) &&
           !unit_running(state);
}

/* The unit's last run's log through the root action, or NULL (logged). */
static char *unit_log(const char *unit)
{
    char *out;
    return action_output("unit-log", unit, LOG_MAX, &out, NULL) == 0 ? out : NULL;
}

/*
 * Starts a root job with the password: unless another job runs (409), or
 * unit (the job's own) is still running, records it in the audit log and
 * runs the action (arg: NULL or checked by the caller). -> 202
 */
static void start_job(struct request *req, struct response *res, const char *action,
                      const char *arg, const char *unit)
{
    cJSON *body = json_body(req, res);
    if (body == NULL || !audit_password_ok(body, res))
        return;
    cJSON *state = unit_state(unit);
    int busy = sysinfo_locked(JOBS_LOCK);
    if (state == NULL || busy < 0) {
        json_error(res, 500, "can not tell whether a job is running; see the server log");
        return;
    }
    if (busy || unit_running(state)) {
        json_error(res, 409, BUSY_MESSAGE);
        return;
    }
    long long audit = audit_begin(server_db, req, action, arg != NULL ? arg : "");
    if (audit < 0) {
        json_error(res, 500, "can not write the audit log; nothing was started");
        return;
    }
    int started = action_run(action, arg) == 0;
    audit_end(server_db, audit,
              started ? "ok: started" : "failed: the action did not start the job");
    if (!started) {
        json_error(res, 502, "could not start it; see the server log");
        return;
    }
    json_reply(res, 202, cJSON_CreateObject());
}

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

/* ---- logs and jobs ------------------------------------------------------ */

/*
 * GET /api/server/log?unit=U: the last run of the unit (at most 500 lines),
 * for nylm's own units, NYLM_UNITS and the backup jobs. -> {unit, log}
 */
void server_log(struct request *req, struct response *res)
{
    const char *unit;
    int rc = http_query(req, "unit", &unit);
    if (rc != 0 || !sysinfo_unit_valid(unit)) {
        json_error(res, 400, rc < 0 ? "invalid query string" : "'unit' must be a unit name");
        return;
    }
    if (!log_allowed(unit)) {
        json_error(res, 404, "nylm does not show this unit (add it to NYLM_UNITS)");
        return;
    }
    char *log = unit_log(unit);
    if (log == NULL) {
        json_error(res, 502, "could not read the log; see the server log");
        return;
    }
    cJSON *out = cJSON_CreateObject();
    if (out == NULL || cJSON_AddStringToObject(out, "unit", unit) == NULL ||
        cJSON_AddStringToObject(out, "log", log) == NULL) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, out);
}

/*
 * GET /api/server/disk-usage: the disk usage job's state, whether any job
 * runs, and what its last run measured ([{label, path, bytes}]; null
 * while it runs or before it ran, or if its log can not be read).
 */
void server_disk_usage(struct request *req, struct response *res)
{
    (void)req;
    cJSON *job = unit_state("nylm-disk-usage.service");
    int busy = sysinfo_locked(JOBS_LOCK);
    if (job == NULL || busy < 0) {
        json_error(res, 500, "can not read the job's state; see the server log");
        return;
    }
    cJSON *sizes = NULL;
    if (unit_ran(job)) {
        char *log = unit_log("nylm-disk-usage.service");
        sizes = log != NULL ? sysinfo_du(log) : NULL;
    }
    cJSON *out = cJSON_CreateObject();
    if (out == NULL || !cJSON_AddItemToObject(out, "job", job) ||
        cJSON_AddBoolToObject(out, "busy", busy) == NULL ||
        !cJSON_AddItemToObject(out, "sizes", sizes != NULL ? sizes : cJSON_CreateNull())) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, out);
}

/* POST /api/server/disk-usage {password}: measures the folders. -> 202 */
void server_disk_usage_start(struct request *req, struct response *res)
{
    start_job(req, res, "disk-usage", NULL, "nylm-disk-usage.service");
}
