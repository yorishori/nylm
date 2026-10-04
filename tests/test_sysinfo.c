/* The server app's parsers of /proc, /sys and root action output. */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/arena.h"
#include "../src/json.h"
#include "../src/sysinfo.h"
#include "test.h"

static char tmp[] = "/tmp/nylm-sysinfo-XXXXXX";

/* The value of obj[key] as a number; -1 if it is not one. */
static double num(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : -1;
}

static const char *str(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void write_file(const char *dir, const char *name, const char *text)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        perror(path);
        exit(1);
    }
    fputs(text, f);
    fclose(f);
}

static void test_memory(void)
{
    cJSON *m = sysinfo_memory("MemTotal:       16000 kB\nMemFree:  100 kB\n"
                              "MemAvailable:    8000 kB\nSwapTotal:  2000 kB\n"
                              "SwapFree:  500 kB\n");
    CHECK(m != NULL);
    CHECK(num(m, "total") == 16000.0 * 1024);
    CHECK(num(m, "available") == 8000.0 * 1024);
    CHECK(num(m, "swap_total") == 2000.0 * 1024);
    CHECK(num(m, "swap_free") == 500.0 * 1024);

    /* No swap lines: no swap. */
    m = sysinfo_memory("MemTotal: 100 kB\nMemAvailable: 100 kB\n");
    CHECK(m != NULL && num(m, "swap_total") == 0 && num(m, "available") == 100.0 * 1024);

    CHECK(sysinfo_memory("") == NULL);
    CHECK(sysinfo_memory("MemTotal: 100 kB\n") == NULL);              /* no available */
    CHECK(sysinfo_memory("MemTotal: x kB\nMemAvailable: 1 kB\n") == NULL);
    CHECK(sysinfo_memory("MemTotal: 100\nMemAvailable: 1 kB\n") == NULL); /* no unit */
    CHECK(sysinfo_memory("MemTotal: 100 kB\nMemAvailable: 200 kB\n") == NULL);
    CHECK(sysinfo_memory("MemTotal: -1 kB\nMemAvailable: 1 kB\n") == NULL);
    /* A name that only starts the same is not the line. */
    CHECK(sysinfo_memory("MemTotalX: 100 kB\nMemAvailable: 1 kB\n") == NULL);
}

static void test_uptime_load(void)
{
    CHECK(sysinfo_uptime("12345.67 99.00\n") == 12345);
    CHECK(sysinfo_uptime("0.00 0.00\n") == 0);
    CHECK(sysinfo_uptime("") == -1);
    CHECK(sysinfo_uptime("x 1\n") == -1);
    CHECK(sysinfo_uptime("-5 1\n") == -1);
    CHECK(sysinfo_uptime("12345.67") == -1); /* nothing after */

    cJSON *l = sysinfo_load("0.52 1.50 2.00 1/123 4567\n");
    CHECK(cJSON_GetArraySize(l) == 3);
    CHECK(cJSON_GetArrayItem(l, 1)->valuedouble == 1.5);
    CHECK(sysinfo_load("0.52 1.50\n") == NULL);
    CHECK(sysinfo_load("") == NULL);
    CHECK(sysinfo_load("a b c d\n") == NULL);
}

static void test_unescape(void)
{
    char a[] = "/mnt/my\\040disk", b[] = "x\\134y", c[] = "bad\\04", d[] = "bad\\999",
         e[] = "nul\\000", f[] = "plain";
    CHECK(sysinfo_unescape(a) == 0);
    CHECK_STR(a, "/mnt/my disk");
    CHECK(sysinfo_unescape(b) == 0);
    CHECK_STR(b, "x\\y");
    CHECK(sysinfo_unescape(c) == -1);
    CHECK(sysinfo_unescape(d) == -1);
    CHECK(sysinfo_unescape(e) == -1);
    CHECK(sysinfo_unescape(f) == 0);
    CHECK_STR(f, "plain");
}

static void test_mounts(void)
{
    cJSON *m = sysinfo_mounts(
        "proc /proc proc rw 0 0\n"
        "/dev/nvme0n1p2 / ext4 rw,relatime 0 0\n"
        "tmpfs /tmp tmpfs rw 0 0\n"
        "/dev/nvme0n1p1 /boot vfat rw 0 0\n"
        "/dev/sda1 /mnt/data\\040drive btrfs rw 0 0\n"
        "/dev/sda1 /mnt/data/sub btrfs rw,subvol=/sub 0 0\n"
        "overlay /var/lib/docker/overlay2/x/merged overlay rw 0 0\n"
        "server:/export /mnt/pc nfs4 rw 0 0\n"
        "\n");
    CHECK(cJSON_GetArraySize(m) == 3);
    CHECK_STR(str(cJSON_GetArrayItem(m, 0), "path"), "/");
    CHECK_STR(str(cJSON_GetArrayItem(m, 0), "type"), "ext4");
    CHECK_STR(str(cJSON_GetArrayItem(m, 1), "device"), "/dev/nvme0n1p1");
    CHECK_STR(str(cJSON_GetArrayItem(m, 2), "path"), "/mnt/data drive");

    m = sysinfo_mounts("");
    CHECK(m != NULL && cJSON_GetArraySize(m) == 0);
    CHECK(sysinfo_mounts("/dev/sda1 /x\n") == NULL);          /* no type */
    CHECK(sysinfo_mounts("/dev/sda1 x ext4 rw 0 0\n") == NULL); /* not a path */
    CHECK(sysinfo_mounts("/dev/sda1 /x\\9 ext4 rw 0 0\n") == NULL);

    /* At most SYSINFO_MAX_MOUNTS. */
    char *many = arena_alloc(80 * 40);
    size_t used = 0;
    for (int i = 0; i < 70; i++)
        used += (size_t)snprintf(many + used, 40, "/dev/d%d /m%d ext4 rw 0 0\n", i, i);
    m = sysinfo_mounts(many);
    CHECK(cJSON_GetArraySize(m) == SYSINFO_MAX_MOUNTS);
}

static void test_temperatures(void)
{
    char hw0[512], hw1[512];
    snprintf(hw0, sizeof hw0, "%s/hwmon0", tmp);
    snprintf(hw1, sizeof hw1, "%s/hwmon1", tmp);
    CHECK(mkdir(hw0, 0700) == 0 && mkdir(hw1, 0700) == 0);
    write_file(hw0, "name", "coretemp\n");
    write_file(hw0, "temp1_input", "45000\n");
    write_file(hw0, "temp1_label", "Package id 0\n");
    write_file(hw0, "temp3_input", "51500\n"); /* a gap, no label */
    write_file(hw0, "temp4_input", "x\n");     /* not ready: skipped */
    write_file(hw1, "temp1_input", "38850\n"); /* no name */

    cJSON *t = sysinfo_temperatures(tmp);
    CHECK(cJSON_GetArraySize(t) == 3);
    cJSON *a = cJSON_GetArrayItem(t, 0), *b = cJSON_GetArrayItem(t, 1),
          *c = cJSON_GetArrayItem(t, 2);
    CHECK_STR(str(a, "sensor"), "coretemp");
    CHECK_STR(str(a, "label"), "Package id 0");
    CHECK(num(a, "celsius") == 45.0);
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(b, "label")));
    CHECK(num(b, "celsius") == 51.5);
    CHECK_STR(str(c, "sensor"), "?");

    t = sysinfo_temperatures("/nonexistent");
    CHECK(t != NULL && cJSON_GetArraySize(t) == 0);
}

static void test_read_file(void)
{
    char dir[512];
    snprintf(dir, sizeof dir, "%s", tmp);
    write_file(dir, "four", "abcd");
    char path[512];
    snprintf(path, sizeof path, "%s/four", tmp);
    CHECK_STR(sysinfo_read_file(path, 4), "abcd");
    CHECK_STR(sysinfo_read_file(path, 100), "abcd");
    CHECK(sysinfo_read_file(path, 3) == NULL);
    write_file(dir, "empty", "");
    snprintf(path, sizeof path, "%s/empty", tmp);
    CHECK_STR(sysinfo_read_file(path, 0), "");
    CHECK(sysinfo_read_file("/nonexistent/x", 10) == NULL);
}

static void test_names(void)
{
    CHECK(sysinfo_unit_valid("docker.service"));
    CHECK(sysinfo_unit_valid("wg-quick@wg0"));
    CHECK(sysinfo_unit_valid("a"));
    CHECK(!sysinfo_unit_valid(""));
    CHECK(!sysinfo_unit_valid(NULL));
    CHECK(!sysinfo_unit_valid("-x"));
    CHECK(!sysinfo_unit_valid("@x"));
    CHECK(!sysinfo_unit_valid("a b"));
    CHECK(!sysinfo_unit_valid("a/b"));
    CHECK(!sysinfo_unit_valid("a;b"));
    char s[131];
    memset(s, 'u', sizeof s);
    s[128] = '\0';
    CHECK(sysinfo_unit_valid(s)); /* 128 */
    s[128] = 'u';
    s[129] = '\0';
    CHECK(!sysinfo_unit_valid(s));

    CHECK(sysinfo_entry_valid("davis"));
    CHECK(sysinfo_entry_valid("immich-db2"));
    CHECK(sysinfo_entry_valid("0"));
    CHECK(!sysinfo_entry_valid(""));
    CHECK(!sysinfo_entry_valid("-a"));
    CHECK(!sysinfo_entry_valid("Davis"));
    CHECK(!sysinfo_entry_valid("a_b"));
    CHECK(!sysinfo_entry_valid("a.b"));
    memset(s, 'e', sizeof s);
    s[32] = '\0';
    CHECK(sysinfo_entry_valid(s)); /* 32 */
    s[32] = 'e';
    s[33] = '\0';
    CHECK(!sysinfo_entry_valid(s));

    CHECK(sysinfo_path_valid("/mnt/data/docker/davis"));
    CHECK(sysinfo_path_valid("/var/lib/docker/volumes/davis_data/_data"));
    CHECK(sysinfo_path_valid("/a/.hidden/b-c"));
    CHECK(!sysinfo_path_valid("/"));
    CHECK(!sysinfo_path_valid(""));
    CHECK(!sysinfo_path_valid("relative/path"));
    CHECK(!sysinfo_path_valid("/a/"));
    CHECK(!sysinfo_path_valid("/a//b"));
    CHECK(!sysinfo_path_valid("/a/./b"));
    CHECK(!sysinfo_path_valid("/a/../b"));
    CHECK(!sysinfo_path_valid("/a/.."));
    CHECK(!sysinfo_path_valid("/a b"));
    CHECK(!sysinfo_path_valid("/a,b"));
    CHECK(!sysinfo_path_valid("/a=b"));
    CHECK(!sysinfo_path_valid("/a$b"));
    char p[1030];
    memset(p, 'p', sizeof p);
    p[0] = '/';
    p[1024] = '\0';
    CHECK(sysinfo_path_valid(p)); /* 1024 */
    p[1024] = 'p';
    p[1025] = '\0';
    CHECK(!sysinfo_path_valid(p));
}

static void test_config(void)
{
    const char *why = NULL;
    cJSON *u = sysinfo_unit_list(" docker  wg-quick@wg0\tsshd.service\n", &why);
    CHECK(cJSON_GetArraySize(u) == 3);
    CHECK_STR(cJSON_GetArrayItem(u, 0)->valuestring, "docker.service");
    CHECK_STR(cJSON_GetArrayItem(u, 1)->valuestring, "wg-quick@wg0.service");
    CHECK_STR(cJSON_GetArrayItem(u, 2)->valuestring, "sshd.service");
    CHECK(sysinfo_unit_list("docker docker.service", &why) == NULL); /* the same unit */
    CHECK_STR(sysinfo_unit_full("docker"), "docker.service");
    CHECK_STR(sysinfo_unit_full("docker.socket"), "docker.socket");
    CHECK_STR(sysinfo_unit_full("backup.timer"), "backup.timer");
    CHECK_STR(sysinfo_unit_full("mnt-data.mount"), "mnt-data.mount");
    CHECK_STR(sysinfo_unit_full("a.b"), "a.b.service");
    CHECK_STR(sysinfo_unit_full(".service"), ".service.service");
    /* A name that is too long once ".service" is added. */
    char long_name[130];
    memset(long_name, 'u', sizeof long_name);
    long_name[121] = '\0';
    CHECK(sysinfo_unit_list(long_name, &why) == NULL);
    long_name[120] = '\0';
    CHECK(sysinfo_unit_list(long_name, &why) != NULL);
    u = sysinfo_unit_list(NULL, &why);
    CHECK(u != NULL && cJSON_GetArraySize(u) == 0);
    u = sysinfo_unit_list("", &why);
    CHECK(u != NULL && cJSON_GetArraySize(u) == 0);
    CHECK(sysinfo_unit_list("docker docker", &why) == NULL);
    CHECK(why != NULL && strstr(why, "twice") != NULL);
    CHECK(sysinfo_unit_list("docker $(x)", &why) == NULL);
    char many[33 * 4 + 1];
    size_t used = 0;
    for (int i = 0; i < 33; i++)
        used += (size_t)snprintf(many + used, sizeof many - used, "u%02d ", i);
    CHECK(sysinfo_unit_list(many, &why) == NULL); /* 33 */
    many[32 * 4] = '\0';
    CHECK(cJSON_GetArraySize(sysinfo_unit_list(many, &why)) == 32);

    cJSON *b = sysinfo_backup_entries(
        "davis=/var/lib/docker/volumes/davis_data/_data immich=/srv/immich,/srv/immich-db\n"
        "compose=/home/yori/docker", &why);
    CHECK(cJSON_GetArraySize(b) == 3);
    cJSON *e = cJSON_GetArrayItem(b, 1);
    CHECK_STR(str(e, "name"), "immich");
    cJSON *paths = cJSON_GetObjectItemCaseSensitive(e, "paths");
    CHECK(cJSON_GetArraySize(paths) == 2);
    CHECK_STR(cJSON_GetArrayItem(paths, 1)->valuestring, "/srv/immich-db");
    b = sysinfo_backup_entries(NULL, &why);
    CHECK(b != NULL && cJSON_GetArraySize(b) == 0);

    CHECK(sysinfo_backup_entries("davis", &why) == NULL);
    CHECK(sysinfo_backup_entries("davis=", &why) == NULL);
    CHECK(sysinfo_backup_entries("=/srv/a", &why) == NULL);
    CHECK(sysinfo_backup_entries("Davis=/srv/a", &why) == NULL);
    CHECK(sysinfo_backup_entries("nylm=/srv/a", &why) == NULL);
    CHECK(why != NULL && strstr(why, "nylm") != NULL);
    CHECK(sysinfo_backup_entries("a=/srv/a,", &why) == NULL);
    CHECK(sysinfo_backup_entries("a=/srv/a,,/srv/b", &why) == NULL);
    CHECK(sysinfo_backup_entries("a=srv/a", &why) == NULL);
    CHECK(sysinfo_backup_entries("a=/srv/../etc", &why) == NULL);
    CHECK(sysinfo_backup_entries("a=/srv/a,/srv/a", &why) == NULL);
    CHECK(sysinfo_backup_entries("a=/srv/a a=/srv/b", &why) == NULL);
    CHECK(why != NULL && strstr(why, "twice") != NULL);

    /* 16 paths in an entry, not 17; 32 entries, not 33. */
    char line[400];
    used = (size_t)snprintf(line, sizeof line, "a=/p0");
    for (int i = 1; i < 16; i++)
        used += (size_t)snprintf(line + used, sizeof line - used, ",/p%d", i);
    CHECK(sysinfo_backup_entries(line, &why) != NULL);
    snprintf(line + used, sizeof line - used, ",/p16");
    CHECK(sysinfo_backup_entries(line, &why) == NULL);
    char entries[33 * 10 + 1];
    used = 0;
    for (int i = 0; i < 33; i++)
        used += (size_t)snprintf(entries + used, sizeof entries - used, "e%02d=/s%02d ", i, i);
    CHECK(sysinfo_backup_entries(entries, &why) == NULL);
    entries[32 * 9] = '\0';
    CHECK(cJSON_GetArraySize(sysinfo_backup_entries(entries, &why)) == 32);
}

static void test_units(void)
{
    cJSON *u = sysinfo_units(
        "Id=docker.service\nDescription=Docker Application Container Engine\n"
        "LoadState=loaded\nActiveState=active\nSubState=running\nResult=success\nType=notify\n"
        "ExecMainStatus=0\nExecMainStartTimestamp=@1700000000\nExecMainExitTimestamp=\n"
        "ActiveEnterTimestamp=@1700000001\nSomethingElse=x=y\n"
        "\n"
        "Id=nylm-disk-usage.service\nLoadState=not-found\nActiveState=inactive\n"
        "ExecMainStatus=\nExecMainStartTimestamp=n/a\n");
    CHECK(cJSON_GetArraySize(u) == 2);
    cJSON *a = cJSON_GetArrayItem(u, 0), *b = cJSON_GetArrayItem(u, 1);
    CHECK_STR(str(a, "unit"), "docker.service");
    CHECK_STR(str(a, "active"), "active");
    CHECK_STR(str(a, "sub"), "running");
    CHECK_STR(str(a, "type"), "notify");
    CHECK(num(a, "status") == 0);
    CHECK(num(a, "started") == 1700000000);
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(a, "ended")));
    CHECK(num(a, "since") == 1700000001);
    CHECK(cJSON_GetObjectItemCaseSensitive(a, "SomethingElse") == NULL);
    CHECK_STR(str(b, "load"), "not-found");
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(b, "status")));
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(b, "started")));

    u = sysinfo_units("");
    CHECK(u != NULL && cJSON_GetArraySize(u) == 0);
    CHECK(sysinfo_units("ActiveState=active\n") == NULL); /* no Id */
    CHECK(sysinfo_units("Id=x\nnot a property\n") == NULL);
}

static void test_du(void)
{
    cJSON *d = sysinfo_du(
        "2026-10-04T12:00:00+0200 koi disk-usage[12]: nylm-du\t1234\tdata\t/mnt/data/nylm\n"
        "2026-10-04T12:00:01+0200 koi disk-usage[12]: something else\n"
        "nylm-du\t0\tbackup:davis\t/srv/davis\n"
        "nylm-du\tx\tbad\t/x\n"
        "nylm-du\t1\ttoo\tmany\tparts\n"
        "nylm-du\t5\tlast\t/no-newline");
    CHECK(cJSON_GetArraySize(d) == 3);
    cJSON *a = cJSON_GetArrayItem(d, 0);
    CHECK_STR(str(a, "label"), "data");
    CHECK_STR(str(a, "path"), "/mnt/data/nylm");
    CHECK(num(a, "bytes") == 1234);
    CHECK_STR(str(cJSON_GetArrayItem(d, 1), "label"), "backup:davis");
    CHECK_STR(str(cJSON_GetArrayItem(d, 2), "path"), "/no-newline");
    d = sysinfo_du("");
    CHECK(d != NULL && cJSON_GetArraySize(d) == 0);
}

static void test_locked(void)
{
    char path[512];
    snprintf(path, sizeof path, "%s/lock", tmp);
    CHECK(sysinfo_locked(path) == 0); /* not there */
    write_file(tmp, "lock", "");
    CHECK(sysinfo_locked(path) == 0);
    int fd = open(path, O_RDONLY);
    CHECK(fd >= 0 && flock(fd, LOCK_EX) == 0);
    CHECK(sysinfo_locked(path) == 1);
    CHECK(sysinfo_locked(tmp) == 0); /* a folder can be locked too */
    close(fd);
    CHECK(sysinfo_locked(path) == 0);
    CHECK(remove(path) == 0);
}

static void test_smart(void)
{
    cJSON *d = sysinfo_smart(
        "[{\"smartctl\":{\"exit_status\":0},"
        "\"device\":{\"name\":\"/dev/sda\",\"type\":\"sat\",\"protocol\":\"ATA\"},"
        "\"model_name\":\"WDC WD40EFRX\",\"user_capacity\":{\"blocks\":7814037168,"
        "\"bytes\":4000787030016},\"smart_status\":{\"passed\":true},"
        "\"temperature\":{\"current\":34},\"power_on_time\":{\"hours\":23456},"
        "\"ata_smart_attributes\":{\"table\":["
        "{\"id\":5,\"name\":\"Reallocated_Sector_Ct\",\"raw\":{\"value\":8,\"string\":\"8\"}},"
        "{\"id\":197,\"name\":\"Current_Pending_Sector\",\"raw\":{\"value\":0}},"
        "{\"id\":198,\"name\":\"Offline_Uncorrectable\",\"raw\":{\"value\":1}}]}},"
        "{\"smartctl\":{\"exit_status\":0},"
        "\"device\":{\"name\":\"/dev/nvme0\",\"protocol\":\"NVMe\"},"
        "\"model_name\":\"Samsung SSD 980\",\"user_capacity\":{\"bytes\":1000204886016},"
        "\"smart_status\":{\"passed\":false},\"temperature\":{\"current\":41},"
        "\"power_on_time\":{\"hours\":900},\"nvme_smart_health_information_log\":"
        "{\"critical_warning\":0,\"available_spare\":100,\"percentage_used\":3,"
        "\"media_errors\":0}},"
        "{\"smartctl\":{\"exit_status\":2,\"messages\":[{\"string\":\"Device is in STANDBY "
        "mode, exit(2)\",\"severity\":\"information\"},{\"string\":\"Device is in STANDBY "
        "mode\",\"severity\":\"error\"}]},\"device\":{\"name\":\"/dev/sdb\"}}]");
    CHECK(cJSON_GetArraySize(d) == 3);
    cJSON *a = cJSON_GetArrayItem(d, 0), *n = cJSON_GetArrayItem(d, 1),
          *s = cJSON_GetArrayItem(d, 2);
    CHECK_STR(str(a, "device"), "/dev/sda");
    CHECK_STR(str(a, "model"), "WDC WD40EFRX");
    CHECK_STR(str(a, "protocol"), "ATA");
    CHECK(num(a, "size") == 4000787030016.0);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(a, "passed")));
    CHECK(num(a, "temperature") == 34);
    CHECK(num(a, "hours") == 23456);
    CHECK(num(a, "reallocated") == 8);
    CHECK(num(a, "pending") == 0);
    CHECK(num(a, "uncorrectable") == 1);
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(a, "percent_used")));
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(a, "error")));

    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(n, "passed")));
    CHECK(num(n, "percent_used") == 3);
    CHECK(num(n, "spare") == 100);
    CHECK(num(n, "media_errors") == 0);
    CHECK(num(n, "critical_warning") == 0);
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(n, "reallocated")));

    CHECK_STR(str(s, "device"), "/dev/sdb");
    CHECK_STR(str(s, "error"), "Device is in STANDBY mode");
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(s, "passed")));
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(s, "model")));

    d = sysinfo_smart("[]\n");
    CHECK(d != NULL && cJSON_GetArraySize(d) == 0);
    CHECK(sysinfo_smart("") == NULL);
    CHECK(sysinfo_smart("{}") == NULL);
    CHECK(sysinfo_smart("[1]") == NULL);
    CHECK(sysinfo_smart("[{\"device\":") == NULL);

    /* At most SYSINFO_MAX_DISKS. */
    char many[40 * 3 + 3];
    size_t used = (size_t)snprintf(many, sizeof many, "[{}");
    for (int i = 1; i < 40; i++)
        used += (size_t)snprintf(many + used, sizeof many - used, ",{}");
    snprintf(many + used, sizeof many - used, "]");
    CHECK(cJSON_GetArraySize(sysinfo_smart(many)) == SYSINFO_MAX_DISKS);
}

static void test_containers(void)
{
    CHECK(sysinfo_container_valid("davis"));
    CHECK(sysinfo_container_valid("immich_server-1.x"));
    CHECK(!sysinfo_container_valid(""));
    CHECK(!sysinfo_container_valid("-x"));
    CHECK(!sysinfo_container_valid("_x"));
    CHECK(!sysinfo_container_valid("a b"));
    CHECK(!sysinfo_container_valid("a/b"));
    CHECK(!sysinfo_container_valid("a:b"));
    char s[131];
    memset(s, 'c', sizeof s);
    s[128] = '\0';
    CHECK(sysinfo_container_valid(s));
    s[128] = 'c';
    s[129] = '\0';
    CHECK(!sysinfo_container_valid(s));

    CHECK(sysinfo_docker_time("1970-01-01T00:00:00Z") == 0);
    CHECK(sysinfo_docker_time("2026-10-04T12:30:15.123456789Z") == 1791117015);
    CHECK(sysinfo_docker_time("2024-02-29T00:00:00Z") == 1709164800);
    CHECK(sysinfo_docker_time("0001-01-01T00:00:00Z") == -1); /* never */
    CHECK(sysinfo_docker_time("2026-10-04T12:30:15+02:00") == -1);
    CHECK(sysinfo_docker_time("2026-13-04T12:30:15Z") == -1);
    CHECK(sysinfo_docker_time("2026-10-04 12:30:15Z") == -1);
    CHECK(sysinfo_docker_time("") == -1);
    CHECK(sysinfo_docker_time("2026-10-04T12:30:15.Zx") == -1);

    CHECK(sysinfo_docker_size("0B") == 0);
    CHECK(sysinfo_docker_size("12.5MiB") == 12.5 * 1048576);
    CHECK(sysinfo_docker_size("1.2GB") == 1.2e9);
    CHECK(sysinfo_docker_size("3kB") == 3000);
    CHECK(sysinfo_docker_size("2GiB") == 2.0 * 1073741824);
    CHECK(sysinfo_docker_size("12") == -1);
    CHECK(sysinfo_docker_size("MiB") == -1);
    CHECK(sysinfo_docker_size("1 MiB") == -1);
    CHECK(sysinfo_docker_size("--") == -1);

    cJSON *c = sysinfo_containers(
        "{\"containers\":["
        "{\"name\":\"/web\",\"image\":\"nginx:1\",\"state\":\"running\",\"health\":\"healthy\","
        "\"started\":\"2026-10-04T12:30:15.5Z\",\"finished\":\"0001-01-01T00:00:00Z\","
        "\"exit_code\":0,\"restarts\":2,\"ports\":{\"80/tcp\":[{\"HostIp\":\"0.0.0.0\","
        "\"HostPort\":\"9001\"},{\"HostIp\":\"::\",\"HostPort\":\"9001\"}],\"443/tcp\":null},"
        "\"project\":\"home\",\"service\":\"web\"},"
        "{\"name\":\"/davis\",\"image\":\"davis\",\"state\":\"exited\",\"health\":null,"
        "\"started\":\"2026-10-04T10:00:00Z\",\"finished\":\"2026-10-04T11:00:00Z\","
        "\"exit_code\":137,\"restarts\":0,\"ports\":{},\"project\":\"\",\"service\":\"\"}],"
        "\"stats\":[{\"name\":\"web\",\"cpu\":\"1.50%\",\"memory\":\"12.5MiB / 31.3GiB\","
        "\"memory_percent\":\"0.04%\"},{\"name\":\"davis\",\"cpu\":\"9%\",\"memory\":\"1MiB / 1GiB\","
        "\"memory_percent\":\"1%\"}]}");
    CHECK(cJSON_GetArraySize(c) == 2);
    cJSON *d = cJSON_GetArrayItem(c, 0), *w = cJSON_GetArrayItem(c, 1); /* by name */
    CHECK_STR(str(d, "name"), "davis");
    CHECK_STR(str(d, "state"), "exited");
    CHECK(num(d, "exit_code") == 137);
    CHECK(num(d, "finished") == 1791111600);
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(d, "cpu"))); /* not running */
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(d, "health")));
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(d, "project")));
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(d, "ports")) == 0);

    CHECK_STR(str(w, "name"), "web");
    CHECK_STR(str(w, "image"), "nginx:1");
    CHECK_STR(str(w, "health"), "healthy");
    CHECK(num(w, "started") == 1791117015);
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(w, "finished")));
    CHECK(num(w, "restarts") == 2);
    CHECK(num(w, "cpu") == 1.5);
    CHECK(num(w, "memory") == 12.5 * 1048576);
    CHECK(num(w, "memory_percent") == 0.04);
    CHECK_STR(str(w, "project"), "home");
    cJSON *ports = cJSON_GetObjectItemCaseSensitive(w, "ports");
    CHECK(cJSON_GetArraySize(ports) == 3);
    CHECK_STR(str(cJSON_GetArrayItem(ports, 0), "container"), "80/tcp");
    CHECK_STR(str(cJSON_GetArrayItem(ports, 0), "host"), "0.0.0.0:9001");
    CHECK_STR(str(cJSON_GetArrayItem(ports, 1), "host"), "[::]:9001");
    CHECK_STR(str(cJSON_GetArrayItem(ports, 2), "container"), "443/tcp");
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(ports, 2), "host")));

    c = sysinfo_containers("{\"containers\":[],\"stats\":[]}");
    CHECK(c != NULL && cJSON_GetArraySize(c) == 0);
    CHECK(sysinfo_containers("") == NULL);
    CHECK(sysinfo_containers("{\"containers\":[]}") == NULL);
    CHECK(sysinfo_containers("{\"containers\":[{\"name\":\"/x\"}],\"stats\":[]}") == NULL);
}

static void test_listening(void)
{
    static const char head[] =
        "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  "
        "timeout inode\n";
    char text[1024];
    cJSON *list = cJSON_CreateArray();

    snprintf(text, sizeof text, "%s"
             "   0: 0100007F:1F90 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000 "
             "       0 1 1 0000000000000000 100 0 0 10 0\n"
             "   1: 00000000:0016 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0\n"
             "   2: 0100007F:1F90 0100007F:D2A4 01 00000000:00000000 00:00000000 00000000  1000\n",
             head);
    CHECK(sysinfo_listening(text, "tcp", 0, list) == 0);
    CHECK(cJSON_GetArraySize(list) == 2); /* the connected one is left out */
    cJSON *a = cJSON_GetArrayItem(list, 0), *b = cJSON_GetArrayItem(list, 1);
    CHECK_STR(str(a, "proto"), "tcp");
    CHECK_STR(str(a, "address"), "127.0.0.1");
    CHECK(num(a, "port") == 8080);
    CHECK_STR(str(b, "address"), "0.0.0.0");
    CHECK(num(b, "port") == 22);

    snprintf(text, sizeof text, "%s"
             "   0: 00000000000000000000000000000000:0016 00000000000000000000000000000000:0000 0A x\n"
             "   1: 00000000000000000000000001000000:1F90 00000000000000000000000000000000:0000 0A x\n",
             head);
    CHECK(sysinfo_listening(text, "tcp", 1, list) == 0);
    CHECK(cJSON_GetArraySize(list) == 4);
    CHECK_STR(str(cJSON_GetArrayItem(list, 2), "address"), "::");
    CHECK_STR(str(cJSON_GetArrayItem(list, 3), "address"), "::1");

    snprintf(text, sizeof text, "%s"
             "  10: 00000000:2328 00000000:0000 07 00000000:00000000 00:00000000 00000000     0\n"
             "  11: 0100007F:0035 0100007F:9C40 01 00000000:00000000 00:00000000 00000000     0\n",
             head);
    CHECK(sysinfo_listening(text, "udp", 0, list) == 0);
    CHECK(cJSON_GetArraySize(list) == 5);
    CHECK_STR(str(cJSON_GetArrayItem(list, 4), "proto"), "udp");
    CHECK(num(cJSON_GetArrayItem(list, 4), "port") == 9000);

    /* Only the head: nothing. Malformed lines: an error. */
    CHECK(sysinfo_listening(head, "tcp", 0, list) == 0);
    CHECK(sysinfo_listening("", "tcp", 0, list) == -1);
    snprintf(text, sizeof text, "%s   0: 0100007F 00000000:0000 0A\n", head);
    CHECK(sysinfo_listening(text, "tcp", 0, list) == -1);
    snprintf(text, sizeof text, "%s   0: 0100007G:1F90 00000000:0000 0A\n", head);
    CHECK(sysinfo_listening(text, "tcp", 0, list) == -1);
    snprintf(text, sizeof text, "%s   0: 0100007F:1F90 00000000:0000 0A\n", head);
    CHECK(sysinfo_listening(text, "tcp", 1, list) == -1); /* v4 text, read as v6 */
    snprintf(text, sizeof text, "%s   0: 0100007F:1F90\n", head);
    CHECK(sysinfo_listening(text, "tcp", 0, list) == -1);
}

static void test_wireguard(void)
{
    static const char key_a[] = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
    static const char key_b[] = "bB+/0123456789abcdefghijklmnopqrstuvwxyzABC=";
    CHECK(sysinfo_wg_key_valid(key_a));
    CHECK(sysinfo_wg_key_valid(key_b));
    CHECK(!sysinfo_wg_key_valid("AAAA="));
    CHECK(!sysinfo_wg_key_valid("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));  /* no = */
    CHECK(!sysinfo_wg_key_valid("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA-A="));
    CHECK(!sysinfo_wg_key_valid(NULL));

    char text[1024];
    snprintf(text, sizeof text,
             "interface\twg0\t%s\t9000\n"
             "peer\twg0\t%s\t203.0.113.7:51820\t10.0.0.2/32,fd00::2/128\t1791117000\t1024\t2048\t25\n"
             "peer\twg0\t%s\t(none)\t(none)\t0\t0\t0\toff\n",
             key_a, key_b, key_a);
    cJSON *w = sysinfo_wireguard(text);
    CHECK(w != NULL);
    cJSON *ifaces = cJSON_GetObjectItemCaseSensitive(w, "interfaces");
    cJSON *peers = cJSON_GetObjectItemCaseSensitive(w, "peers");
    CHECK(cJSON_GetArraySize(ifaces) == 1 && cJSON_GetArraySize(peers) == 2);
    cJSON *i = cJSON_GetArrayItem(ifaces, 0);
    CHECK_STR(str(i, "name"), "wg0");
    CHECK_STR(str(i, "public_key"), key_a);
    CHECK(num(i, "port") == 9000);
    cJSON *p = cJSON_GetArrayItem(peers, 0), *q = cJSON_GetArrayItem(peers, 1);
    CHECK_STR(str(p, "interface"), "wg0");
    CHECK_STR(str(p, "public_key"), key_b);
    CHECK_STR(str(p, "endpoint"), "203.0.113.7:51820");
    cJSON *ips = cJSON_GetObjectItemCaseSensitive(p, "allowed_ips");
    CHECK(cJSON_GetArraySize(ips) == 2);
    CHECK_STR(cJSON_GetArrayItem(ips, 1)->valuestring, "fd00::2/128");
    CHECK(num(p, "handshake") == 1791117000);
    CHECK(num(p, "rx") == 1024 && num(p, "tx") == 2048 && num(p, "keepalive") == 25);
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(q, "endpoint")));
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(q, "handshake")));
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(q, "keepalive")));
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(q, "allowed_ips")) == 0);

    w = sysinfo_wireguard("\n");
    CHECK(w != NULL && cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(w, "peers")) == 0);
    /* A private key (wg's raw interface line has 5 fields) is never accepted. */
    snprintf(text, sizeof text, "wg0\t%s\t%s\t9000\toff\n", key_a, key_b);
    CHECK(sysinfo_wireguard(text) == NULL);
    snprintf(text, sizeof text, "interface\twg0\t%s\tx\n", key_a);
    CHECK(sysinfo_wireguard(text) == NULL);
    snprintf(text, sizeof text, "interface\twg0\tnot-a-key\t9000\n");
    CHECK(sysinfo_wireguard(text) == NULL);
    snprintf(text, sizeof text, "peer\twg0\t%s\t(none)\t(none)\t-1\t0\t0\toff\n", key_a);
    CHECK(sysinfo_wireguard(text) == NULL);
    snprintf(text, sizeof text, "peer\twg0\t%s\t(none)\n", key_a);
    CHECK(sysinfo_wireguard(text) == NULL);
}

int main(void)
{
    if (arena_init(4 * 1024 * 1024) != 0 || mkdtemp(tmp) == NULL)
        return 1;
    json_init();
    test_memory();
    test_uptime_load();
    test_unescape();
    test_mounts();
    test_temperatures();
    test_read_file();
    test_names();
    test_config();
    test_units();
    test_du();
    test_locked();
    test_smart();
    test_containers();
    test_listening();
    test_wireguard();
    static const char *const made[] = {
        "hwmon0/name", "hwmon0/temp1_input", "hwmon0/temp1_label", "hwmon0/temp3_input",
        "hwmon0/temp4_input", "hwmon1/temp1_input", "four", "empty", "hwmon0", "hwmon1",
    };
    for (size_t i = 0; i < sizeof made / sizeof made[0]; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", tmp, made[i]);
        CHECK(remove(path) == 0);
    }
    CHECK(rmdir(tmp) == 0);
    TEST_DONE();
}
