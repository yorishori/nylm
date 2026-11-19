#define _XOPEN_SOURCE 700

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/arena.h"
#include "../src/dap.h"
#include "test.h"

static char root[] = "/tmp/nylm-test-dap-XXXXXX";

/* Writes text to root/rel, making its folders. */
static void put(const char *rel, const char *text)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    for (char *p = strchr(path + strlen(root) + 1, '/'); p != NULL; p = strchr(p + 1, '/')) {
        *p = '\0';
        mkdir(path, 0755);
        *p = '/';
    }
    FILE *f = fopen(path, "w");
    if (f != NULL) {
        fputs(text, f);
        fclose(f);
    }
}

/* root/sys/block/name -> the device folder at rel (made). */
static void disk(const char *name, const char *rel)
{
    char dir[1024], link[1024];
    snprintf(dir, sizeof dir, "%s/sys/%s", root, rel);
    char marker[1100];
    snprintf(marker, sizeof marker, "sys/%s/.keep", rel);
    put(marker, "");
    snprintf(link, sizeof link, "%s/sys/block/%s", root, name);
    put("sys/block/.keep", "");
    if (symlink(dir, link) != 0)
        perror("symlink");
}

/* nftw's callback: removes one file, link or (emptied) folder. */
static int remove_one(const char *path, const struct stat *sb, int type, struct FTW *ftw)
{
    (void)sb;
    (void)ftw;
    return type == FTW_DP ? rmdir(path) : unlink(path);
}

static void test_uuid(void)
{
    CHECK(dap_uuid_valid("0A1B-C2D3"));
    CHECK(dap_uuid_valid("0000-0000"));
    CHECK(dap_uuid_valid("FFFF-FFFF"));
    CHECK(!dap_uuid_valid("0a1b-c2d3"));
    CHECK(!dap_uuid_valid("0A1B-C2D"));
    CHECK(!dap_uuid_valid("0A1B-C2D34"));
    CHECK(!dap_uuid_valid("0A1BC2D3"));
    CHECK(!dap_uuid_valid("0A1B_C2D3"));
    CHECK(!dap_uuid_valid("GA1B-C2D3"));
    CHECK(!dap_uuid_valid("2f9c6a3e-1b2d-4c5e-8f90-a1b2c3d4e5f6"));
    CHECK(!dap_uuid_valid(""));
    CHECK(!dap_uuid_valid(NULL));
}

static void test_udev(void)
{
    struct dap_drive d;
    CHECK(dap_udev_parse("S:disk/by-uuid/0A1B-C2D3\nE:ID_FS_TYPE=exfat\nE:ID_FS_UUID=0A1B-C2D3\n"
                         "E:ID_FS_LABEL=FIIO_M11\nE:ID_VENDOR=FiiO\nE:ID_MODEL=M11_Plus\n", &d));
    CHECK_STR(d.fstype, "exfat");
    CHECK_STR(d.uuid, "0A1B-C2D3");
    CHECK_STR(d.label, "FIIO_M11");
    CHECK_STR(d.vendor, "FiiO");
    CHECK_STR(d.model, "M11_Plus");
    /* No newline at the end; no label. */
    CHECK(dap_udev_parse("E:ID_FS_TYPE=vfat\nE:ID_FS_UUID=1234-ABCD", &d));
    CHECK_STR(d.label, "");
    CHECK_STR(d.vendor, "");
    /* Not FAT or exFAT, or no valid UUID. */
    CHECK(!dap_udev_parse("E:ID_FS_TYPE=ext4\nE:ID_FS_UUID=1234-ABCD\n", &d));
    CHECK(!dap_udev_parse("E:ID_FS_TYPE=ntfs\nE:ID_FS_UUID=1234-ABCD\n", &d));
    CHECK(!dap_udev_parse("E:ID_FS_TYPE=vfat\nE:ID_FS_UUID=1234abcd\n", &d));
    CHECK(!dap_udev_parse("E:ID_FS_TYPE=vfat\n", &d));
    CHECK(!dap_udev_parse("", &d));
    CHECK(!dap_udev_parse("E:ID_FS_TYPE=vfatx\nE:ID_FS_UUID=1234-ABCD\n", &d));
    /* A value that does not fit, or is not text, is left out. */
    char long_label[200];
    snprintf(long_label, sizeof long_label,
             "E:ID_FS_TYPE=vfat\nE:ID_FS_UUID=1234-ABCD\nE:ID_FS_LABEL=%065d\n", 0);
    CHECK(dap_udev_parse(long_label, &d));
    CHECK_STR(d.label, "");
    snprintf(long_label, sizeof long_label,
             "E:ID_FS_TYPE=vfat\nE:ID_FS_UUID=1234-ABCD\nE:ID_FS_LABEL=%064d\n", 0);
    CHECK(dap_udev_parse(long_label, &d));
    CHECK(strlen(d.label) == 64);
    CHECK(dap_udev_parse("E:ID_FS_TYPE=vfat\nE:ID_FS_UUID=1234-ABCD\nE:ID_MODEL=a\tb\n", &d));
    CHECK_STR(d.model, "");
}

static void test_mounts(void)
{
    const char *info =
        "22 1 259:2 / / rw,relatime shared:1 - ext4 /dev/nvme0n1p2 rw\n"
        "40 22 8:17 / /mnt/nylm-dap rw,nosuid,nodev,noexec - exfat /dev/sdb1 rw\n"
        "41 22 8:33 / /media/My\\040Player rw - vfat /dev/sdc1 rw\n";
    char out[64];
    CHECK(dap_mount_of(info, "8:17", out, sizeof out) == 1);
    CHECK_STR(out, "/mnt/nylm-dap");
    CHECK(dap_mount_of(info, "8:33", out, sizeof out) == 1);
    CHECK_STR(out, "/media/My Player");
    CHECK(dap_mount_of(info, "8:1", out, sizeof out) == 0);
    CHECK(dap_mount_of(info, "8:17", out, 13) == 0); /* does not fit */
    CHECK(dap_device_at(info, "/mnt/nylm-dap", out, sizeof out) == 1);
    CHECK_STR(out, "8:17");
    CHECK(dap_device_at(info, "/mnt/nylm-da", out, sizeof out) == 0);
    CHECK(dap_device_at(info, "/media/My Player", out, sizeof out) == 1);
    CHECK_STR(out, "8:33");
    CHECK(dap_device_at("", "/", out, sizeof out) == 0);
    CHECK(dap_device_at("garbage\n\n", "/", out, sizeof out) == 0);
}

static void test_list(void)
{
    if (mkdtemp(root) == NULL) {
        CHECK(0);
        return;
    }
    /* sdb: a USB stick, exFAT on sdb1, ext4 on sdb2. */
    const char *usb = "devices/pci0000:00/0000:00:14.0/usb2/2-1/2-1:1.0/host6/block/sdb";
    disk("sdb", usb);
    char rel[512];
    snprintf(rel, sizeof rel, "sys/%s/sdb1/partition", usb);
    put(rel, "1\n");
    snprintf(rel, sizeof rel, "sys/%s/sdb1/dev", usb);
    put(rel, "8:17\n");
    snprintf(rel, sizeof rel, "sys/%s/sdb1/size", usb);
    put(rel, "62521344\n");
    snprintf(rel, sizeof rel, "sys/%s/sdb2/partition", usb);
    put(rel, "2\n");
    snprintf(rel, sizeof rel, "sys/%s/sdb2/dev", usb);
    put(rel, "8:18\n");
    put("udev/b8:17", "E:ID_FS_TYPE=exfat\nE:ID_FS_UUID=0A1B-C2D3\nE:ID_FS_LABEL=PLAYER\n"
                      "E:ID_VENDOR=FiiO\nE:ID_MODEL=M11\n");
    put("udev/b8:18", "E:ID_FS_TYPE=ext4\nE:ID_FS_UUID=2f9c6a3e-1b2d-4c5e-8f90-a1b2c3d4e5f6\n");
    /* sdc: a USB player without partitions, FAT on the whole disk. */
    const char *usb2 = "devices/pci0000:00/0000:00:14.0/usb2/2-2/2-2:1.0/host7/block/sdc";
    disk("sdc", usb2);
    snprintf(rel, sizeof rel, "sys/%s/dev", usb2);
    put(rel, "8:32\n");
    snprintf(rel, sizeof rel, "sys/%s/size", usb2);
    put(rel, "1024\n");
    put("udev/b8:32", "E:ID_FS_TYPE=vfat\nE:ID_FS_UUID=1234-ABCD\n");
    /* sda: a SATA disk with a FAT partition (the EFI one): not USB. */
    const char *sata = "devices/pci0000:00/0000:00:17.0/ata1/host0/block/sda";
    disk("sda", sata);
    snprintf(rel, sizeof rel, "sys/%s/sda1/partition", sata);
    put(rel, "1\n");
    snprintf(rel, sizeof rel, "sys/%s/sda1/dev", sata);
    put(rel, "8:1\n");
    put("udev/b8:1", "E:ID_FS_TYPE=vfat\nE:ID_FS_UUID=AAAA-BBBB\n");
    /* A loop device: not a name to look at, never USB. */
    disk("loop0", "devices/virtual/block/loop0");
    put("mountinfo", "40 22 8:17 / /mnt/nylm-dap rw - exfat /dev/sdb1 rw\n");

    char sys[600], udev[600], mountinfo[600];
    snprintf(sys, sizeof sys, "%s/sys", root);
    snprintf(udev, sizeof udev, "%s/udev", root);
    snprintf(mountinfo, sizeof mountinfo, "%s/mountinfo", root);
    struct dap_drive d[DAP_MAX];
    int n = dap_list(sys, udev, mountinfo, d, DAP_MAX);
    CHECK(n == 2);
    if (n == 2) {
        CHECK_STR(d[0].dev, "sdb1");
        CHECK_STR(d[0].majmin, "8:17");
        CHECK_STR(d[0].uuid, "0A1B-C2D3");
        CHECK_STR(d[0].fstype, "exfat");
        CHECK_STR(d[0].label, "PLAYER");
        CHECK(d[0].size == 62521344ULL * 512);
        CHECK_STR(d[0].mounted, "/mnt/nylm-dap");
        CHECK_STR(d[1].dev, "sdc");
        CHECK_STR(d[1].uuid, "1234-ABCD");
        CHECK(d[1].size == 1024 * 512);
        CHECK_STR(d[1].mounted, "");
    }
    /* At most max. */
    CHECK(dap_list(sys, udev, mountinfo, d, 1) == 1);
    /* Without a mount table, nothing is mounted. */
    snprintf(mountinfo, sizeof mountinfo, "%s/none", root);
    CHECK(dap_list(sys, udev, mountinfo, d, DAP_MAX) == 2);
    CHECK_STR(d[0].mounted, "");
    /* No /sys/block: an error. */
    snprintf(sys, sizeof sys, "%s/nothing", root);
    CHECK(dap_list(sys, udev, mountinfo, d, DAP_MAX) == -1);
    CHECK(nftw(root, remove_one, 16, FTW_DEPTH | FTW_PHYS) == 0);
}

int main(void)
{
    if (arena_init(1 << 22) != 0)
        return 1;
    test_uuid();
    test_udev();
    test_mounts();
    test_list();
    TEST_DONE();
}
