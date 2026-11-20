#ifndef DAP_H
#define DAP_H

#include <stddef.h>

/*
 * DAPs (digital audio players) plugged into the server: USB drives with a
 * FAT or exFAT file system, found without root in /sys, udev's data and
 * the mount table. One at a time is mounted at DAP_MOUNT by the root
 * action dap-mount (deploy/actions/), for user nylm, without devices,
 * setuid or programs, and unmounted by dap-umount.
 */

#define DAP_MOUNT     "/mnt/nylm-dap"
#define DAP_UUID_LEN  9   /* "ABCD-1234" */
#define DAP_MAX_TEXT  64  /* bytes in a label, vendor, model, name */
#define DAP_MAX       16  /* drives listed */

struct dap_drive {
    char dev[32];                 /* "sdb1" */
    char majmin[24];              /* "8:17" */
    char uuid[DAP_UUID_LEN + 1];
    char fstype[8];               /* "vfat" or "exfat" */
    char label[DAP_MAX_TEXT + 1]; /* "" if none */
    char vendor[DAP_MAX_TEXT + 1];
    char model[DAP_MAX_TEXT + 1];
    unsigned long long size;      /* bytes */
    char mounted[256];            /* where it is mounted; "" if not */
};

/* 1 if s is a FAT or exFAT file system UUID as udev and blkid show it:
 * XXXX-XXXX, X an upper-case hex digit. */
int dap_uuid_valid(const char *s);

/*
 * Reads udev's data about a block device (text: its lines, "E:KEY=value")
 * into d: ID_FS_TYPE, ID_FS_UUID, ID_FS_LABEL, ID_VENDOR, ID_MODEL (each
 * kept only if it is text that fits; "_" stays as udev wrote it). 1 if it
 * is a FAT or exFAT file system with a valid UUID, else 0.
 */
int dap_udev_parse(const char *text, struct dap_drive *d);

/*
 * Where the device majmin ("8:17") is mounted, from the text of a
 * mountinfo file, into out (escapes such as \040 decoded): 1 if it is,
 * 0 if not or the path does not fit.
 */
int dap_mount_of(const char *mountinfo, const char *majmin, char *out, size_t outlen);

/* The device ("8:17") mounted at path, from mountinfo text, into out: 1 if
 * one is, else 0. */
int dap_device_at(const char *mountinfo, const char *path, char *out, size_t outlen);

/*
 * Lists the FAT and exFAT file systems on USB drives: each partition of a
 * disk whose /sys/block entry is below a USB device (or the disk itself
 * when it has none), with what udev and the mount table say. sys, udev and
 * mountinfo are where those are ("/sys", "/run/udev/data",
 * "/proc/self/mountinfo"). The number found (at most max, by device name),
 * or -1 if /sys can not be read (logged).
 */
int dap_list(const char *sys, const char *udev, const char *mountinfo, struct dap_drive *out,
             int max);

#endif
