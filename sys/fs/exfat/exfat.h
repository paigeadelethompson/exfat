/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024 The FreeBSD Foundation
 *
 * This software was developed by Paige A. Thompson (Ravenhammer Research.)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 */

#ifndef _FS_EXFAT_H_
#define _FS_EXFAT_H_

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/time.h>   /* For struct timespec */
#include "exfat_node.h"   /* For struct exfat_hash_mtx */

/* Forward declarations */
struct exfat_node;
struct vnode;
struct mount;
enum vtype;  /* Forward declaration of vtype enum */

/* ExFAT specific constants */
#define EXFAT_IOC_MAGIC       'E'
#define EXFAT_SECTOR_SIZE     512
#define EXFAT_SECTOR_BITS     9

/* Cluster status values */
#define EXFAT_CLUSTER_FREE    0x00000000
#define EXFAT_CLUSTER_END     0xFFFFFFFF
#define EXFAT_CLUSTER_BAD     0xFFFFFFF7

/* ExFAT volume boot record structure */
struct exfat_boot_record {
    uint8_t  jump_boot[3];
    uint8_t  fs_name[8];
    uint8_t  must_be_zero[53];
    uint64_t partition_offset;
    uint64_t volume_length;
    uint32_t fat_offset;
    uint32_t fat_length;
    uint32_t cluster_heap_offset;
    uint32_t cluster_count;
    uint32_t root_dir_cluster;
    uint32_t volume_serial;
    uint16_t fs_revision;
    uint16_t volume_flags;
    uint8_t  volume_state;      /* Clean/dirty state */
    uint8_t  bytes_per_sector_shift;
    uint8_t  sectors_per_cluster_shift;
    uint8_t  number_of_fats;
    uint8_t  drive_select;
    uint8_t  percent_in_use;
    uint8_t  reserved[7];
    uint8_t  boot_code[390];
    uint16_t boot_signature;
} __packed;

/* Mount structure for ExFAT */
struct exfat_mount {
    struct mount *mp;
    struct vnode *devvp;      /* Device vnode */
    struct exfat_boot_record boot;
    uint32_t clusters_per_fat;
    uint32_t bytes_per_cluster;
    uint32_t root_cluster;
    uint32_t bitmap_cluster;     /* First cluster of allocation bitmap */
    uint64_t bitmap_size;        /* Size of allocation bitmap in bytes */
    uint32_t upcase_cluster;     /* First cluster of upcase table */
    struct exfat_upcase *upcase;   /* Upcase table */
    char     volume_label[12];   /* Volume label (UTF-8, null-terminated) */
    uint8_t  volume_label_len;   /* Length of volume label */
    uint32_t bitmap_sectors;      /* Number of sectors in bitmap */
    uint32_t clusters_count;     /* Total number of clusters */
    uint32_t mount_flags;            /* Mount flags */
    uint32_t error_count;            /* Count of I/O errors */
    struct timespec last_error_time;  /* Time of last error */
    uint32_t free_clusters;    /* Cached count of free clusters */
    /* Node hash table */
    LIST_HEAD(exfat_hashhead, exfat_node) *node_hash;
    u_long node_hash_mask;
    struct exfat_hash_mtx hash_mtx;
};

/* ExFAT Directory Entry Types */
#define EXFAT_ENTRY_EOD          0x00    /* End of directory */
#define EXFAT_ENTRY_BITMAP       0x81    /* Allocation bitmap */
#define EXFAT_ENTRY_UPCASE       0x82    /* Upper case table */
#define EXFAT_ENTRY_LABEL        0x83    /* Volume label */
#define EXFAT_ENTRY_FILE         0x85    /* File directory entry */
#define EXFAT_ENTRY_STREAM       0xC0    /* Stream extension */
#define EXFAT_ENTRY_NAME         0xC1    /* File name extension */
#define EXFAT_ENTRY_DELETED      0xE5    /* Deleted entry */

/* File attributes */
#define EXFAT_ATTR_READ_ONLY     0x01
#define EXFAT_ATTR_HIDDEN        0x02
#define EXFAT_ATTR_SYSTEM        0x04
#define EXFAT_ATTR_VOLUME_ID     0x08
#define EXFAT_ATTR_DIRECTORY     0x10
#define EXFAT_ATTR_ARCHIVE       0x20

/* Directory entry structures */
struct exfat_entry_file {
    uint8_t  type;               /* 0x85 */
    uint8_t  secondary_count;
    uint16_t checksum;
    uint16_t file_attributes;
    uint8_t  reserved1[2];
    uint32_t create_timestamp;
    uint32_t last_modified_timestamp;
    uint32_t last_access_timestamp;
    uint8_t  create_time_ms;
    uint8_t  last_modified_time_ms;
    uint8_t  create_tz;
    uint8_t  last_modified_tz;
    uint8_t  last_access_tz;
    uint8_t  reserved2[7];
} __packed;

struct exfat_entry_stream {
    uint8_t  type;               /* 0xC0 */
    uint8_t  flags;
    uint8_t  reserved1;
    uint8_t  name_length;
    uint16_t name_hash;
    uint16_t reserved2;
    uint64_t valid_data_length;
    uint32_t reserved3;
    uint32_t first_cluster;
    uint64_t data_length;
} __packed;

struct exfat_entry_name {
    uint8_t  type;               /* 0xC1 */
    uint8_t  flags;
    uint16_t name[15];           /* UTF-16 characters */
} __packed;

struct exfat_entry_bitmap {
    uint8_t  type;               /* 0x81 */
    uint8_t  flags;
    uint8_t  reserved[18];
    uint32_t first_cluster;
    uint64_t data_length;
} __packed;

struct exfat_entry_upcase {
    uint8_t  type;               /* 0x82 */
    uint8_t  reserved1[3];
    uint32_t checksum;
    uint8_t  reserved2[12];
    uint32_t first_cluster;
    uint64_t data_length;
} __packed;

struct exfat_entry_label {
    uint8_t  type;               /* 0x83 */
    uint8_t  character_count;
    uint16_t unicode_label[11];
    uint8_t  reserved[8];
} __packed;

/* Directory entry set structure */
struct exfat_direntry_set {
    struct exfat_entry_file file;
    struct exfat_entry_stream stream;
    struct exfat_entry_name name[17];  /* Maximum name entries */
    uint8_t name_count;
};

/* Directory scanning context */
struct exfat_scan_ctx {
    struct exfat_mount *emp;
    uint32_t cluster;           /* Current cluster */
    uint32_t offset;           /* Offset within cluster */
    struct buf *bp;            /* Current buffer */
    uint8_t *entry;            /* Current entry pointer */
};

/* In-memory file info structure */
struct exfat_file_info {
    uint32_t first_cluster;
    uint64_t file_size;
    uint64_t valid_size;
    uint16_t attributes;
    uint32_t last_modified_time;
    struct timespec create_time;
    struct timespec modified_time;
    struct timespec access_time;
};

/* Function prototypes */
int exfat_cluster_next(struct exfat_mount *emp, uint32_t cluster);
int exfat_cluster_alloc(struct exfat_mount *emp, uint32_t *cluster);
int exfat_cluster_free(struct exfat_mount *emp, uint32_t cluster);
int exfat_cluster_link(struct exfat_mount *emp, uint32_t cluster, uint32_t next);

/* Conversion macros */
#define VFSTOEXFAT(mp)   ((struct exfat_mount *)((mp)->mnt_data))
#define VTOVFS(vp)       ((vp)->v_mount)
#define VTOVFSMP(vp)     ((struct exfat_mount *)((vp)->v_mount->mnt_data))
#define VTOE(vp)   ((struct exfat_node *)(vp)->v_data)
#define ETOV(ep)   ((ep)->vnode)

/* Additional function prototypes */
int exfat_get_node(struct mount *mp, uint32_t cluster, int type, struct vnode **vpp);
int exfat_read_directory(struct vnode *vp, struct uio *uio, int *eofflag);
int exfat_lookup_node(struct vnode *dvp, struct componentname *cnp, struct vnode **vpp);
int exfat_create_entry(struct vnode *dvp, const char *name, int namelen,
                      uint16_t attr, struct timespec *ctime,
                      struct exfat_direntry_set *es);
int exfat_scan_directory(struct vnode *vp, struct exfat_scan_ctx *ctx);
int exfat_next_dirent(struct exfat_scan_ctx *ctx, struct exfat_direntry_set *es);
void exfat_scan_cleanup(struct exfat_scan_ctx *ctx);
int exfat_remove_entry(struct vnode *dvp, struct exfat_direntry_set *es, off_t offset);

/* VFS operations */
extern struct vop_vector exfat_vnodeops;

/* FAT operations */
int exfat_cluster_extend(struct exfat_mount *emp, uint32_t *cluster);

/* Access control */
int exfat_access(struct vnode *vp, accmode_t accmode, struct ucred *cred,
                 struct thread *td);

/* Volume label character restrictions */
#define EXFAT_LABEL_MAX_LEN     11

/* Invalid characters for volume label */
static const uint8_t exfat_invalid_chars[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,  /* Control chars */
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    '*', '?', '/', '\\', '|', '.', '"', '<', '>', ':',  /* Reserved chars */
    0x7F  /* DEL */
};

/* Volume flags */
#define EXFAT_VOL_DIRTY        0x0001  /* Volume is dirty */
#define EXFAT_VOL_ACTIVE_FAT   0x0002  /* Second FAT is active */

/* Volume state values */
#define EXFAT_STATE_CLEAN     0x00
#define EXFAT_STATE_DIRTY     0x01

/* Timestamp update flags */
#define EXFAT_UTIME_ACCESS    0x01
#define EXFAT_UTIME_MODIFY    0x02
#define EXFAT_UTIME_CREATE    0x04

/* Volume serial number generation */
#define EXFAT_SERIAL_DATE_MASK  0xFFFF0000
#define EXFAT_SERIAL_TIME_MASK  0x0000FFFF
#define EXFAT_SERIAL_DATE_SHIFT 16

/* Time/date conversion macros */
#define EXFAT_TIME(h,m,s)  ((h) << 11 | (m) << 5 | (s) >> 1)
#define EXFAT_DATE(y,m,d)  (((y)-1980) << 9 | (m) << 5 | (d))

/* Time/date extraction macros */
#define EXFAT_HOUR(t)      ((t) >> 11)
#define EXFAT_MINUTE(t)    (((t) >> 5) & 0x3F)
#define EXFAT_SECOND(t)    (((t) & 0x1F) << 1)
#define EXFAT_YEAR(d)      (1980 + ((d) >> 9))
#define EXFAT_MONTH(d)     (((d) >> 5) & 0x0F)
#define EXFAT_DAY(d)       ((d) & 0x1F)

/* Volume timestamp structure */
struct exfat_timespec {
    uint32_t date;         /* Date in ExFAT format */
    uint32_t time;         /* Time in ExFAT format */
    uint8_t  time_ms;      /* Milliseconds (0-199) */
    uint8_t  tz_offset;    /* Timezone offset in 15-minute increments */
};

/* Function prototypes */
int exfat_find_dirent(struct vnode *vp, uint32_t cluster,
    struct exfat_direntry_set *es, off_t *offset);
void exfat_update_timestamps(struct exfat_entry_file *file, int flags);
int exfat_write_direntry(struct vnode *vp, struct exfat_direntry_set *es, off_t offset);

/* UTF conversion functions */
int exfat_utf16_to_utf8(const uint16_t *src, size_t srclen,
                        char *dst, size_t dstlen, size_t *outlen);
int exfat_utf8_to_utf16(const char *src, uint16_t *dst,
                        size_t dstlen, size_t *outlen);

/* Error codes */
#define EXFAT_ERROR_NONE        0
#define EXFAT_ERROR_IO          EIO
#define EXFAT_ERROR_NOMEM       ENOMEM
#define EXFAT_ERROR_NOTFOUND    ENOENT
#define EXFAT_ERROR_EXISTS      EEXIST
#define EXFAT_ERROR_NOSPC       ENOSPC
#define EXFAT_ERROR_INVAL       EINVAL

/* vnode types */
#define EXFAT_VREG    1    /* regular file */
#define EXFAT_VDIR    2    /* directory */

/* Memory allocation type */
MALLOC_DECLARE(M_EXFAT);

/* Volume operations */
int exfat_init_bitmap(struct exfat_mount *emp);
int exfat_init_upcase(struct exfat_mount *emp);
void exfat_cleanup_upcase(struct exfat_mount *emp);
int exfat_scan_cluster(struct exfat_mount *emp, uint32_t cluster);
int exfat_scan_clusters(struct exfat_mount *emp, uint32_t start, uint32_t count);
int exfat_read_volume_label(struct exfat_mount *emp);
int exfat_write_volume_label(struct exfat_mount *emp, const char *label, size_t len);
int exfat_validate_label(const char *label, size_t len);
int exfat_update_percent_in_use(struct exfat_mount *emp);
int exfat_set_volume_dirty(struct exfat_mount *emp, int dirty);

/* Sector checksum operations */
int exfat_verify_sector(struct buf *bp);
void exfat_update_sector_checksum(struct buf *bp);
int exfat_handle_bad_sector(struct exfat_mount *emp, daddr_t sector);

/* Device access macro */
#define EXFAT_DEV(mp)    (VFSTOEXFAT(mp)->devvp)

/* Time conversion functions */
void unix_time_to_exfat(const struct timespec *ts, uint32_t *date, uint32_t *time);
void exfat_time_to_unix(uint32_t date, uint32_t time, struct timespec *ts);

/* Mount flags */
#define EXFAT_MNT_FSCK     0x0001    /* Filesystem check needed */
#define EXFAT_MNT_REPAIR   0x0002    /* Filesystem repair in progress */
#define EXFAT_MNT_ERRORS   0x0004    /* Filesystem has errors */

/* Error handling flags */
#define EXFAT_EH_NONE      0x00
#define EXFAT_EH_READ      0x01    /* Read error occurred */
#define EXFAT_EH_WRITE     0x02    /* Write error occurred */
#define EXFAT_EH_FAT       0x04    /* FAT corruption detected */
#define EXFAT_EH_BITMAP    0x08    /* Bitmap inconsistency */
#define EXFAT_EH_CLUSTER   0x10    /* Bad cluster detected */

/* Function prototypes */
int exfat_handle_error(struct exfat_mount *emp, struct vnode *vp, int error, int flags);
int exfat_extend_file(struct vnode *vp, off_t new_size);
int exfat_init_directory(struct exfat_mount *emp, uint32_t cluster);
int exfat_mount(struct mount *mp);
int exfat_unmount(struct mount *mp, int mntflags);
int exfat_root(struct mount *mp, int flags, struct vnode **vpp);
void exfat_cleanup_bitmap(struct exfat_mount *emp);
int exfat_read_rootdir(struct exfat_mount *emp);

#endif /* _FS_EXFAT_H_ */ 