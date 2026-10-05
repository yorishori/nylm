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

#include <dirent.h>
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
#define SMART_MAX  (1024 * 1024)     /* smartctl's reports of every disk */
#define DOCKER_MAX (2 * 1024 * 1024) /* docker-list: every container */
#define WG_MAX     (256 * 1024)      /* wg-show: interfaces and peers */
#define PROC_NET_MAX (2 * 1024 * 1024) /* one of /proc/net/{tcp,udp}{,6} */
#define PEER_NAME_MAX 100            /* bytes */
#define MAX_BACKUPS_LISTED 200       /* of one entry */

#define BUSY_MESSAGE \
    "another job is running (disk usage, update or backup); try again when it is done"

/* nylm's own units: always shown, their logs always readable. */
static const char *const nylm_units[] = {
    "nylm.service", "nylm-music-scan.service", "nylm-music-write.service",
    "nylm-music-move.service", "nylm-qobuz.service", "nylm-disk-usage.service",
    "nylm-updates-check.service", "nylm-update.service", "nylm-backup@nylm.service",
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

/* NYLM_BACKUP_DIR, or NULL when it is not set. */
static const char *backup_dir(void)
{
    const char *dir = getenv("NYLM_BACKUP_DIR");
    return dir != NULL && *dir != '\0' ? dir : NULL;
}

/* NYLM_BACKUP_KEEP: how many backups of an entry are kept (2 if unset). */
static int backup_keep(void)
{
    const char *keep = getenv("NYLM_BACKUP_KEEP");
    return keep != NULL && *keep != '\0' ? sysinfo_backup_keep(keep) : 2;
}

int server_configure(void)
{
    const char *dir = backup_dir();
    const char *group = getenv("NYLM_BACKUP_GROUP");
    const char *data = getenv("NYLM_DATA");
    cJSON *entries = backup_entries();
    if (watched_units() == NULL || entries == NULL)
        return -1;
    if (backup_keep() < 0) {
        fprintf(stderr, "server: NYLM_BACKUP_KEEP must be a number from 1 to 100\n");
        return -1;
    }
    if (group != NULL && *group != '\0' && !sysinfo_group_valid(group)) {
        fprintf(stderr, "server: NYLM_BACKUP_GROUP must be a group name (a-z 0-9 _ -)\n");
        return -1;
    }
    if (dir == NULL)
        return 0;
    if (!sysinfo_path_valid(dir)) {
        fprintf(stderr, "server: NYLM_BACKUP_DIR must be an absolute path of A-Z a-z 0-9 "
                        "/ . _ - only\n");
        return -1;
    }
    /* A backup must never hold the backups (nor be inside them). */
    if (data != NULL && (sysinfo_path_within(dir, data) || sysinfo_path_within(data, dir))) {
        fprintf(stderr, "server: NYLM_BACKUP_DIR and NYLM_DATA must not be inside each other\n");
        return -1;
    }
    const cJSON *entry;
    cJSON_ArrayForEach(entry, entries) {
        const cJSON *path;
        cJSON_ArrayForEach(path, cJSON_GetObjectItemCaseSensitive(entry, "paths")) {
            if (sysinfo_path_within(dir, path->valuestring) ||
                sysinfo_path_within(path->valuestring, dir)) {
                fprintf(stderr, "server: NYLM_BACKUP: %s and NYLM_BACKUP_DIR must not be "
                                "inside each other\n", path->valuestring);
                return -1;
            }
        }
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
                               "ActiveEnterTimestamp,Type,Job",
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

/* 1 if the unit (from units_state) is queued to start, starting or running. */
static int unit_running(const cJSON *state)
{
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(state, "active");
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(state, "queued")))
        return 1;
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
 * Starts a job with the password: unless unit (the job's own) is still
 * running or, for a root job (exclusive), another job runs (409), records
 * it in the audit log and runs the action (arg: NULL or checked by the
 * caller); body is the request's, with the password. -> 202
 */
static void start_job(struct request *req, struct response *res, const cJSON *body,
                      const char *action, const char *arg, const char *unit, int exclusive)
{
    if (!audit_password_ok(body, res))
        return;
    cJSON *state = unit_state(unit);
    int busy = exclusive ? sysinfo_locked(JOBS_LOCK) : 0;
    if (state == NULL || busy < 0) {
        json_error(res, 500, "can not tell whether a job is running; see the server log");
        return;
    }
    if (unit_running(state)) {
        json_error(res, 409, "it is already running");
        return;
    }
    if (busy) {
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
    cJSON *body = json_body(req, res);
    if (body != NULL)
        start_job(req, res, body, "disk-usage", NULL, "nylm-disk-usage.service", 1);
}

/* ---- updates ------------------------------------------------------------- */

/*
 * GET /api/server/updates: the update check (its job, and the packages it
 * found: null unless it ran and succeeded), the update job, the last full
 * upgrade (pacman.log), whether a reboot is needed and a job runs.
 */
void server_updates(struct request *req, struct response *res)
{
    (void)req;
    static const char *const units[] = { "nylm-updates-check.service", "nylm-update.service" };
    cJSON *jobs = units_state(units, 2);
    int busy = sysinfo_locked(JOBS_LOCK);
    struct utsname u;
    if (jobs == NULL || busy < 0 || uname(&u) != 0) {
        json_error(res, 500, "can not read the jobs' state; see the server log");
        return;
    }
    cJSON *check = cJSON_DetachItemFromArray(jobs, 0);
    cJSON *update = cJSON_DetachItemFromArray(jobs, 0);
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(check, "result");
    cJSON *packages = NULL;
    if (unit_ran(check) && cJSON_IsString(result) && strcmp(result->valuestring, "success") == 0) {
        char *log = unit_log(units[0]);
        packages = log != NULL ? sysinfo_updates(log) : NULL;
    }
    /* The end of the log has the latest upgrade (one is many lines). */
    const char *pacman_log = sysinfo_read_tail("/var/log/pacman.log", 1024 * 1024);
    long long last = pacman_log != NULL ? sysinfo_last_upgrade(pacman_log) : -1;

    cJSON *out = cJSON_CreateObject();
    if (out == NULL || !cJSON_AddItemToObject(out, "check", check) ||
        !cJSON_AddItemToObject(out, "packages", packages != NULL ? packages : cJSON_CreateNull()) ||
        !cJSON_AddItemToObject(out, "update", update) ||
        (last >= 0 ? cJSON_AddNumberToObject(out, "last_upgrade", (double)last)
                   : cJSON_AddNullToObject(out, "last_upgrade")) == NULL ||
        cJSON_AddBoolToObject(out, "reboot_needed", reboot_needed(u.release)) == NULL ||
        cJSON_AddBoolToObject(out, "busy", busy) == NULL) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, out);
}

/* POST /api/server/updates/check {password}: looks for updates. -> 202 */
void server_updates_check(struct request *req, struct response *res)
{
    cJSON *body = json_body(req, res);
    if (body != NULL)
        start_job(req, res, body, "updates-check", NULL, "nylm-updates-check.service", 0);
}

/* POST /api/server/update {password}: updates every package. -> 202 */
void server_update(struct request *req, struct response *res)
{
    cJSON *body = json_body(req, res);
    if (body != NULL)
        start_job(req, res, body, "update", NULL, "nylm-update.service", 1);
}

/* POST /api/server/reboot {password}: reboots, unless a job runs. -> 202 */
void server_reboot(struct request *req, struct response *res)
{
    cJSON *body = json_body(req, res);
    if (body == NULL || !audit_password_ok(body, res))
        return;
    int busy = sysinfo_locked(JOBS_LOCK);
    if (busy != 0) {
        json_error(res, busy < 0 ? 500 : 409,
                   busy < 0 ? "can not tell whether a job is running; see the server log"
                            : BUSY_MESSAGE);
        return;
    }
    long long audit = audit_begin(server_db, req, "reboot", "");
    if (audit < 0) {
        json_error(res, 500, "can not write the audit log; nothing was done");
        return;
    }
    int ok = action_run("reboot", NULL) == 0;
    audit_end(server_db, audit, ok ? "ok: rebooting" : "failed: see the server log");
    if (!ok) {
        json_error(res, 502, "could not reboot; see the server log");
        return;
    }
    json_reply(res, 202, cJSON_CreateObject());
}

/* ---- backups ------------------------------------------------------------- */

/* The backups of entry name in NYLM_BACKUP_DIR/name, newest first:
 * [{file, size, time}], at most MAX_BACKUPS_LISTED. NULL if out of memory
 * or the folder can not be read (logged). */
static cJSON *backups_of(const char *dir, const char *name)
{
    char path[SYSINFO_MAX_PATH + 64];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    cJSON *list = cJSON_CreateArray();
    DIR *d = opendir(path);
    if (list == NULL || d == NULL) {
        if (d == NULL && errno == ENOENT)
            return list; /* no backup yet */
        if (d == NULL)
            fprintf(stderr, "server: %s: %s\n", path, strerror(errno));
        else
            closedir(d);
        return NULL;
    }
    struct dirent *e;
    int ok = 1;
    while (ok && (e = readdir(d)) != NULL && cJSON_GetArraySize(list) < MAX_BACKUPS_LISTED) {
        long long t = sysinfo_backup_time(name, e->d_name);
        char file[SYSINFO_MAX_PATH + 400];
        struct stat sb;
        if (t < 0)
            continue;
        snprintf(file, sizeof file, "%s/%s", path, e->d_name);
        if (stat(file, &sb) != 0 || !S_ISREG(sb.st_mode))
            continue;
        cJSON *b = cJSON_CreateObject();
        ok = b != NULL && cJSON_AddStringToObject(b, "file", e->d_name) != NULL &&
             cJSON_AddNumberToObject(b, "size", (double)sb.st_size) != NULL &&
             cJSON_AddNumberToObject(b, "time", (double)t) != NULL;
        /* Newest first: before the first one that is older. */
        cJSON *at;
        int i = 0;
        cJSON_ArrayForEach(at, list) {
            if (cJSON_GetObjectItemCaseSensitive(at, "time")->valuedouble < (double)t)
                break;
            i++;
        }
        ok = ok && cJSON_InsertItemInArray(list, i, b);
    }
    closedir(d);
    return ok ? list : NULL;
}

/*
 * GET /api/server/backups: where backups go and how many are kept, and
 * each entry (nylm's data first, then NYLM_BACKUP): its paths, its job
 * and its backups. available: the folder is there (the drive mounted).
 */
void server_backups(struct request *req, struct response *res)
{
    (void)req;
    const char *dir = backup_dir();
    const char *data = getenv("NYLM_DATA");
    cJSON *entries = backup_entries();
    cJSON *nylm = cJSON_CreateObject();
    cJSON *nylm_paths = nylm != NULL ? cJSON_AddArrayToObject(nylm, "paths") : NULL;
    cJSON *data_path = cJSON_CreateString(data != NULL ? data : "");
    if (entries == NULL || nylm_paths == NULL || data_path == NULL ||
        cJSON_AddStringToObject(nylm, "name", "nylm") == NULL ||
        !cJSON_AddItemToArray(nylm_paths, data_path) ||
        !cJSON_InsertItemInArray(entries, 0, nylm)) {
        json_error(res, 500, "internal error");
        return;
    }
    const char *units[SYSINFO_MAX_ENTRIES + 1];
    size_t n = 0;
    cJSON *e;
    cJSON_ArrayForEach(e, entries) {
        char *unit = arena_alloc(64);
        if (unit == NULL) {
            json_error(res, 500, "internal error");
            return;
        }
        snprintf(unit, 64, "nylm-backup@%s.service",
                 cJSON_GetObjectItemCaseSensitive(e, "name")->valuestring);
        units[n++] = unit;
    }
    cJSON *jobs = units_state(units, n);
    int busy = sysinfo_locked(JOBS_LOCK);
    struct stat sb;
    int available = dir != NULL && stat(dir, &sb) == 0 && S_ISDIR(sb.st_mode);
    if (jobs == NULL || busy < 0) {
        json_error(res, 500, "can not read the jobs' state; see the server log");
        return;
    }
    cJSON_ArrayForEach(e, entries) {
        const char *name = cJSON_GetObjectItemCaseSensitive(e, "name")->valuestring;
        cJSON *list = available ? backups_of(dir, name) : cJSON_CreateArray();
        if (list == NULL || !cJSON_AddItemToObject(e, "job", cJSON_DetachItemFromArray(jobs, 0)) ||
            !cJSON_AddItemToObject(e, "backups", list)) {
            json_error(res, 500, "can not list the backups; see the server log");
            return;
        }
    }
    const char *group = getenv("NYLM_BACKUP_GROUP");
    cJSON *out = cJSON_CreateObject();
    if (out == NULL ||
        (dir != NULL ? cJSON_AddStringToObject(out, "dir", dir)
                     : cJSON_AddNullToObject(out, "dir")) == NULL ||
        cJSON_AddBoolToObject(out, "available", available) == NULL ||
        cJSON_AddNumberToObject(out, "keep", backup_keep()) == NULL ||
        (group != NULL && *group != '\0' ? cJSON_AddStringToObject(out, "group", group)
                                         : cJSON_AddNullToObject(out, "group")) == NULL ||
        cJSON_AddBoolToObject(out, "busy", busy) == NULL ||
        !cJSON_AddItemToObject(out, "entries", entries)) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, out);
}

/* POST /api/server/backups/start {name, password}: backs up one entry
 * (nylm or a name in NYLM_BACKUP). -> 202 */
void server_backup_start(struct request *req, struct response *res)
{
    cJSON *body = json_body(req, res);
    const char *name;
    if (body == NULL)
        return;
    if (json_get_string(body, "name", 1, 32, &name) != NULL || !sysinfo_entry_valid(name)) {
        json_error(res, 400, "'name' must be a backup entry's name");
        return;
    }
    int known = strcmp(name, "nylm") == 0;
    cJSON *entries = backup_entries();
    const cJSON *e;
    cJSON_ArrayForEach(e, entries)
        known |= strcmp(cJSON_GetObjectItemCaseSensitive(e, "name")->valuestring, name) == 0;
    if (!known) {
        json_error(res, 404, "no such backup entry (NYLM_BACKUP in /etc/nylm.conf)");
        return;
    }
    if (backup_dir() == NULL) {
        json_error(res, 409, "set NYLM_BACKUP_DIR in /etc/nylm.conf first");
        return;
    }
    char unit[64];
    snprintf(unit, sizeof unit, "nylm-backup@%s.service", name);
    start_job(req, res, body, "backup", name, unit, 1);
}

/* ---- services ------------------------------------------------------------ */

/* GET /api/server/units: the units in NYLM_UNITS, then nylm's own. */
void server_units(struct request *req, struct response *res)
{
    (void)req;
    const char *names[SYSINFO_MAX_UNITS + NNYLM_UNITS];
    size_t n = 0;
    const cJSON *item;
    cJSON *watched = watched_units();
    cJSON_ArrayForEach(item, watched)
        names[n++] = item->valuestring;
    for (size_t i = 0; i < NNYLM_UNITS; i++)
        names[n++] = nylm_units[i];
    cJSON *list = watched != NULL ? units_state(names, n) : NULL;
    cJSON *out = list != NULL ? cJSON_CreateObject() : NULL;
    if (out == NULL || !cJSON_AddItemToObject(out, "units", list)) {
        json_error(res, 500, "can not read the services' state; see the server log");
        return;
    }
    json_reply(res, 200, out);
}

/* ---- containers ---------------------------------------------------------- */

/* GET /api/server/containers: every container, its state and use. */
void server_containers(struct request *req, struct response *res)
{
    (void)req;
    char *text;
    cJSON *list = action_output("docker-list", NULL, DOCKER_MAX, &text, NULL) == 0
                      ? sysinfo_containers(text)
                      : NULL;
    if (list == NULL) {
        fprintf(stderr, "server: can not list the containers (action docker-list)\n");
        json_error(res, 502, "could not list the containers; see the server log");
        return;
    }
    cJSON *out = cJSON_CreateObject();
    if (out == NULL || !cJSON_AddItemToObject(out, "containers", list)) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, out);
}

/* GET /api/server/containers/log?name=N: its last 500 lines. -> {name, log} */
void server_container_log(struct request *req, struct response *res)
{
    const char *name;
    int rc = http_query(req, "name", &name);
    if (rc != 0 || !sysinfo_container_valid(name)) {
        json_error(res, 400, rc < 0 ? "invalid query string" : "'name' must be a container name");
        return;
    }
    char *log;
    if (action_output("docker-logs", name, LOG_MAX, &log, NULL) != 0) {
        json_error(res, 502, "could not read its log (is there such a container?); "
                             "see the server log");
        return;
    }
    cJSON *out = cJSON_CreateObject();
    if (out == NULL || cJSON_AddStringToObject(out, "name", name) == NULL ||
        cJSON_AddStringToObject(out, "log", log) == NULL) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, out);
}

/*
 * POST /api/server/containers/restart {name, password}: restarts it, not
 * while a job runs (a backup stops and starts containers). -> 200 when it
 * is restarted.
 */
void server_container_restart(struct request *req, struct response *res)
{
    cJSON *body = json_body(req, res);
    const char *name;
    if (body == NULL)
        return;
    if (json_get_string(body, "name", 1, 128, &name) != NULL || !sysinfo_container_valid(name)) {
        json_error(res, 400, "'name' must be a container name");
        return;
    }
    if (!audit_password_ok(body, res))
        return;
    int busy = sysinfo_locked(JOBS_LOCK);
    if (busy != 0) {
        json_error(res, busy < 0 ? 500 : 409,
                   busy < 0 ? "can not tell whether a job is running; see the server log"
                            : BUSY_MESSAGE);
        return;
    }
    long long audit = audit_begin(server_db, req, "docker-restart", name);
    if (audit < 0) {
        json_error(res, 500, "can not write the audit log; nothing was restarted");
        return;
    }
    int ok = action_run("docker-restart", name) == 0;
    audit_end(server_db, audit, ok ? "ok: restarted" : "failed: see the server log");
    if (!ok) {
        json_error(res, 502, "could not restart it; see the server log");
        return;
    }
    json_reply(res, 200, cJSON_CreateObject());
}

/* ---- network ------------------------------------------------------------- */

/*
 * GET /api/server/ports: the listening TCP and unconnected UDP sockets
 * (/proc/net, no root), and nylm's own port. -> {ports, nylm_port}
 */
void server_ports(struct request *req, struct response *res)
{
    (void)req;
    static const struct {
        const char *path, *proto;
        int v6;
    } files[] = {
        { "/proc/net/tcp", "tcp", 0 }, { "/proc/net/tcp6", "tcp", 1 },
        { "/proc/net/udp", "udp", 0 }, { "/proc/net/udp6", "udp", 1 },
    };
    cJSON *out = cJSON_CreateObject();
    cJSON *list = out != NULL ? cJSON_AddArrayToObject(out, "ports") : NULL;
    if (list == NULL) {
        json_error(res, 500, "internal error");
        return;
    }
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        /* ~14000 sockets each; four of them fit the request's memory. */
        char *text = sysinfo_read_file(files[i].path, PROC_NET_MAX);
        if (text == NULL && access(files[i].path, F_OK) != 0)
            continue; /* no IPv6 on this machine */
        if (text == NULL || sysinfo_listening(text, files[i].proto, files[i].v6, list) != 0) {
            fprintf(stderr, "server: can not read %s\n", files[i].path);
            json_error(res, 500, "can not read the open ports; see the server log");
            return;
        }
    }
    /* main() refused to start with an invalid NYLM_PORT; 8080 when unset. */
    const char *port = getenv("NYLM_PORT");
    long nylm_port = 8080;
    if (port != NULL && *port != '\0') {
        char *end;
        errno = 0;
        nylm_port = strtol(port, &end, 10);
        if (errno != 0 || *end != '\0' || nylm_port < 1 || nylm_port > 65535)
            nylm_port = 0;
    }
    if (cJSON_AddNumberToObject(out, "nylm_port", (double)nylm_port) == NULL) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, out);
}

/* Adds each peer's name (null if it has none) from wg_peers. 0 or -1. */
static int add_peer_names(cJSON *peers)
{
    sqlite3_stmt *st = db_prepare(server_db, "SELECT name FROM wg_peers WHERE public_key = ?");
    if (st == NULL)
        return -1;
    cJSON *p;
    int rc = 0;
    cJSON_ArrayForEach(p, peers) {
        const char *key = cJSON_GetObjectItemCaseSensitive(p, "public_key")->valuestring;
        sqlite3_reset(st);
        int step = sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC) == SQLITE_OK
                       ? sqlite3_step(st)
                       : SQLITE_ERROR;
        if (step != SQLITE_ROW && step != SQLITE_DONE) {
            db_log_error(server_db, "peer name");
            rc = -1;
            break;
        }
        if ((step == SQLITE_ROW
                 ? cJSON_AddStringToObject(p, "name", (const char *)sqlite3_column_text(st, 0))
                 : cJSON_AddNullToObject(p, "name")) == NULL) {
            rc = -1;
            break;
        }
    }
    sqlite3_finalize(st);
    return rc;
}

/* GET /api/server/wireguard: interfaces and peers (action wg-show). */
void server_wireguard(struct request *req, struct response *res)
{
    (void)req;
    char *text;
    cJSON *wg = action_output("wg-show", NULL, WG_MAX, &text, NULL) == 0
                    ? sysinfo_wireguard(text)
                    : NULL;
    if (wg == NULL) {
        fprintf(stderr, "server: can not read WireGuard (action wg-show)\n");
        json_error(res, 502, "could not read WireGuard; see the server log");
        return;
    }
    if (add_peer_names(cJSON_GetObjectItemCaseSensitive(wg, "peers")) != 0) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, wg);
}

/*
 * POST /api/server/wireguard/name {public_key, name}: names a peer ("" to
 * forget its name). A label in nylm only: no password. -> 200
 */
void server_wireguard_name(struct request *req, struct response *res)
{
    cJSON *body = json_body(req, res);
    const char *key, *name, *err;
    if (body == NULL)
        return;
    if (json_get_string(body, "public_key", 44, 44, &key) != NULL || !sysinfo_wg_key_valid(key)) {
        json_error(res, 400, "'public_key' must be a WireGuard public key");
        return;
    }
    if ((err = json_get_text(body, "name", 0, PEER_NAME_MAX, 0, &name)) != NULL) {
        json_error(res, 400, err);
        return;
    }
    sqlite3_stmt *st = db_prepare(server_db, name[0] != '\0'
        ? "INSERT INTO wg_peers (public_key, name) VALUES (?1, ?2)"
          " ON CONFLICT (public_key) DO UPDATE SET name = ?2"
        : "DELETE FROM wg_peers WHERE public_key = ?1");
    int ok = st != NULL && sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC) == SQLITE_OK &&
             (name[0] == '\0' || sqlite3_bind_text(st, 2, name, -1, SQLITE_STATIC) == SQLITE_OK) &&
             sqlite3_step(st) == SQLITE_DONE;
    if (!ok)
        db_log_error(server_db, "peer name");
    sqlite3_finalize(st);
    if (!ok) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, cJSON_CreateObject());
}

/* ---- disks' health ------------------------------------------------------- */

/* GET /api/server/smart: each disk's SMART health (action smart). -> {disks} */
void server_smart(struct request *req, struct response *res)
{
    (void)req;
    char *text;
    if (action_output("smart", NULL, SMART_MAX, &text, NULL) != 0) {
        json_error(res, 502, "could not read the disks' SMART data; see the server log");
        return;
    }
    cJSON *disks = sysinfo_smart(text);
    if (disks == NULL) {
        fprintf(stderr, "server: the action smart printed something unexpected\n");
        json_error(res, 502, "could not read the disks' SMART data; see the server log");
        return;
    }
    cJSON *out = cJSON_CreateObject();
    if (out == NULL || !cJSON_AddItemToObject(out, "disks", disks)) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, out);
}
