/* The server app's parsers of /proc, /sys and root action output. */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
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
