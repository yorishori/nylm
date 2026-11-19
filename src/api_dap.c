/*
 * DAPs in the music app (src/dap.h): the USB drives with a FAT or exFAT
 * file system plugged into the server, the one mounted at DAP_MOUNT,
 * mounting and unmounting it (root actions dap-mount, dap-umount; both
 * recorded in the audit table), and the names given to them (daps).
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>

#include "action.h"
#include "api.h"
#include "audit.h"
#include "dap.h"
#include "db.h"
#include "json.h"
#include "sysinfo.h"

#define SYS_DIR    "/sys"
#define UDEV_DIR   "/run/udev/data"
#define MOUNTINFO  "/proc/self/mountinfo"

/* What is plugged in and mounted now. */
struct state {
    struct dap_drive drives[DAP_MAX];
    int n;
    int mounted;  /* index of the drive at DAP_MOUNT, or -1 */
    int other;    /* 1 if something nylm does not list is mounted there */
};

/* Reads the state; 0, or -1 after replying 500. */
static int look(struct state *s, struct response *res)
{
    s->n = dap_list(SYS_DIR, UDEV_DIR, MOUNTINFO, s->drives, DAP_MAX);
    s->mounted = -1;
    s->other = 0;
    const char *mounts = sysinfo_read_file(MOUNTINFO, 1024 * 1024);
    if (s->n < 0 || mounts == NULL) {
        json_error(res, 500, "can not read the drives (/sys, /proc); see the server log");
        return -1;
    }
    char dev[24];
    if (dap_device_at(mounts, DAP_MOUNT, dev, sizeof dev)) {
        for (int i = 0; i < s->n; i++)
            if (strcmp(s->drives[i].majmin, dev) == 0)
                s->mounted = i;
        s->other = s->mounted < 0;
    }
    return 0;
}

/* The drive with uuid, or NULL. */
static const struct dap_drive *find(const struct state *s, const char *uuid)
{
    for (int i = 0; i < s->n; i++)
        if (strcmp(s->drives[i].uuid, uuid) == 0)
            return &s->drives[i];
    return NULL;
}

/* Adds key: s, or null when it is empty. NULL when out of memory. */
static cJSON *add_text(cJSON *obj, const char *key, const char *s)
{
    return s[0] != '\0' ? cJSON_AddStringToObject(obj, key, s) : cJSON_AddNullToObject(obj, key);
}

/* A drive as JSON: dev, uuid, fstype, label, vendor, model, size, mounted
 * (where, or null), name (given in nylm, or null). NULL on error. */
static cJSON *drive_json(const struct dap_drive *d, sqlite3_stmt *name)
{
    cJSON *obj = cJSON_CreateObject();
    int rc = sqlite3_bind_text(name, 1, d->uuid, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK)
        rc = sqlite3_step(name);
    const char *given = rc == SQLITE_ROW ? (const char *)sqlite3_column_text(name, 0) : NULL;
    int ok = obj != NULL && (rc == SQLITE_ROW || rc == SQLITE_DONE) &&
             cJSON_AddStringToObject(obj, "dev", d->dev) != NULL &&
             cJSON_AddStringToObject(obj, "uuid", d->uuid) != NULL &&
             cJSON_AddStringToObject(obj, "fstype", d->fstype) != NULL &&
             add_text(obj, "label", d->label) != NULL && add_text(obj, "vendor", d->vendor) != NULL &&
             add_text(obj, "model", d->model) != NULL &&
             cJSON_AddNumberToObject(obj, "size", (double)d->size) != NULL &&
             add_text(obj, "mounted", d->mounted) != NULL &&
             (given != NULL ? cJSON_AddStringToObject(obj, "name", given)
                            : cJSON_AddNullToObject(obj, "name")) != NULL;
    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
        db_log_error(music_db, "dap: name");
    sqlite3_reset(name);
    return ok ? obj : NULL;
}

/*
 * GET /api/music/dap: the mount point, the USB drives with a FAT or exFAT
 * file system (drives, as drive_json() says), the one mounted at the mount
 * point (mounted: its drive, with free and total bytes; null if none),
 * and whether something else is mounted there (other).
 */
void music_dap(struct request *req, struct response *res)
{
    (void)req;
    struct state s;
    if (look(&s, res) != 0)
        return;
    sqlite3_stmt *name = db_prepare(music_db, "SELECT name FROM daps WHERE uuid = ?");
    cJSON *obj = cJSON_CreateObject();
    cJSON *list = obj != NULL && name != NULL ? cJSON_AddArrayToObject(obj, "drives") : NULL;
    int ok = list != NULL && cJSON_AddStringToObject(obj, "mount", DAP_MOUNT) != NULL &&
             cJSON_AddBoolToObject(obj, "other", s.other) != NULL;
    for (int i = 0; ok && i < s.n; i++) {
        cJSON *d = drive_json(&s.drives[i], name);
        ok = d != NULL && cJSON_AddItemToArray(list, d);
    }
    if (ok && s.mounted >= 0) {
        cJSON *m = drive_json(&s.drives[s.mounted], name);
        struct statvfs fs;
        int got = statvfs(DAP_MOUNT, &fs) == 0;
        if (!got)
            perror("dap: statvfs " DAP_MOUNT);
        ok = m != NULL && cJSON_AddItemToObject(obj, "mounted", m) &&
             (got ? cJSON_AddNumberToObject(m, "free", (double)fs.f_bavail * (double)fs.f_frsize)
                  : cJSON_AddNullToObject(m, "free")) != NULL &&
             (got ? cJSON_AddNumberToObject(m, "total", (double)fs.f_blocks * (double)fs.f_frsize)
                  : cJSON_AddNullToObject(m, "total")) != NULL;
    } else if (ok) {
        ok = cJSON_AddNullToObject(obj, "mounted") != NULL;
    }
    sqlite3_finalize(name);
    if (!ok) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, obj);
}

/* Reads body's "uuid" (a FAT or exFAT UUID); replies 400 and returns NULL
 * if it is not one. */
static const char *read_uuid(const cJSON *body, struct response *res)
{
    const char *uuid;
    if (json_get_string(body, "uuid", DAP_UUID_LEN, DAP_UUID_LEN, &uuid) != NULL ||
        !dap_uuid_valid(uuid)) {
        json_error(res, 400, "'uuid' must be a FAT or exFAT file system's UUID, like ABCD-1234");
        return NULL;
    }
    return uuid;
}

/* Runs a root action (dap-mount with uuid, or dap-umount) and records it
 * in the audit table. -> 200, or 500/502. */
static void run_action(struct request *req, struct response *res, const char *action,
                       const char *uuid)
{
    char detail[64] = "{}";
    if (uuid != NULL)
        snprintf(detail, sizeof detail, "{\"uuid\":\"%s\"}", uuid);
    long long audit = audit_begin(music_db, req, action, detail);
    if (audit < 0) {
        json_error(res, 500, "can not write the audit log; nothing was done");
        return;
    }
    int ok = action_run(action, uuid) == 0;
    audit_end(music_db, audit, ok ? "ok" : "failed: see the server log");
    if (!ok) {
        json_error(res, 502, uuid != NULL ? "could not mount it; see the server log"
                                          : "could not unmount it; see the server log");
        return;
    }
    json_reply(res, 200, cJSON_CreateObject());
}

/*
 * POST /api/music/dap/mount {uuid}: mounts the USB drive with that file
 * system at the mount point (root action dap-mount, which checks it all
 * again): it must be listed, not mounted, and the mount point free (409).
 */
void music_dap_mount(struct request *req, struct response *res)
{
    cJSON *body = json_body(req, res);
    const char *uuid = body != NULL ? read_uuid(body, res) : NULL;
    struct state s;
    if (uuid == NULL || look(&s, res) != 0)
        return;
    const struct dap_drive *d = find(&s, uuid);
    if (d == NULL) {
        json_error(res, 404, "no USB drive plugged in has a FAT or exFAT file system with this "
                             "UUID");
        return;
    }
    if (s.mounted >= 0 || s.other) {
        json_error(res, 409, s.mounted >= 0 && d == &s.drives[s.mounted]
                                 ? "it is mounted already"
                                 : "a drive is mounted already: unmount it first");
        return;
    }
    if (d->mounted[0] != '\0') {
        json_error(res, 409, "it is mounted somewhere else on the server: unmount it there first");
        return;
    }
    run_action(req, res, "dap-mount", d->uuid);
}

/* POST /api/music/dap/unmount {}: unmounts the drive at the mount point
 * (root action dap-umount; 409 if none is). */
void music_dap_unmount(struct request *req, struct response *res)
{
    struct state s;
    if (json_body(req, res) == NULL || look(&s, res) != 0)
        return;
    if (s.mounted < 0 && !s.other) {
        json_error(res, 409, "no drive is mounted at " DAP_MOUNT);
        return;
    }
    run_action(req, res, "dap-umount", NULL);
}

/* POST /api/music/dap/name {uuid, name}: the name nylm shows for a DAP (1
 * to 64 bytes of text), kept whether it is plugged in or not. */
void music_dap_name(struct request *req, struct response *res)
{
    cJSON *body = json_body(req, res);
    const char *uuid = body != NULL ? read_uuid(body, res) : NULL;
    const char *name;
    if (uuid == NULL)
        return;
    if (json_get_text(body, "name", 1, DAP_MAX_TEXT, 0, &name) != NULL) {
        json_error(res, 400, "'name' must be 1 to 64 bytes of text");
        return;
    }
    sqlite3_stmt *st = db_prepare(music_db,
        "INSERT INTO daps (uuid, name) VALUES (?, ?)"
        " ON CONFLICT (uuid) DO UPDATE SET name = excluded.name");
    int ok = st != NULL && sqlite3_bind_text(st, 1, uuid, -1, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_bind_text(st, 2, name, -1, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_step(st) == SQLITE_DONE;
    if (st != NULL && !ok)
        db_log_error(music_db, "dap: keep a name");
    sqlite3_finalize(st);
    if (!ok) {
        json_error(res, 500, "internal error");
        return;
    }
    json_reply(res, 200, cJSON_CreateObject());
}
