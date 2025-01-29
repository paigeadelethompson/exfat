/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024 The FreeBSD Foundation
 *
 * This software was developed by {Your Name or Organization}.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _FS_EXFAT_H_
#define _FS_EXFAT_H_

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/vnode.h>

/* ExFAT specific constants */
#define EXFAT_IOC_MAGIC       'E'
#define EXFAT_SECTOR_SIZE     512
#define EXFAT_SECTOR_BITS     9

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
    struct exfat_boot_record boot;
    uint32_t clusters_per_fat;
    uint32_t bytes_per_cluster;
    uint32_t root_cluster;
    uint32_t bitmap_cluster;     /* First cluster of allocation bitmap */
    uint64_t bitmap_size;        /* Size of allocation bitmap in bytes */
    uint32_t upcase_cluster;     /* First cluster of upcase table */
    void    *upcase;            /* Upcase table structure */
    char     volume_label[12];   /* Volume label (UTF-8, null-terminated) */
    uint8_t  volume_label_len;   /* Length of volume label */
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

/* Cluster constants */
#define EXFAT_CLUSTER_FREE       0x00000000
#define EXFAT_CLUSTER_BAD        0xFFFFFFF7
#define EXFAT_CLUSTER_END        0xFFFFFFFF

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
uint32_t exfat_cluster_next(struct exfat_mount *emp, uint32_t cluster);
int exfat_cluster_alloc(struct exfat_mount *emp, uint32_t *cluster);
void exfat_cluster_free(struct exfat_mount *emp, uint32_t cluster);

/* Conversion macros */
#define VFSTOEXFAT(mp)   ((struct exfat_mount *)((mp)->mnt_data))
#define VTOVFS(vp)       ((vp)->v_mount)
#define VTOVFSMP(vp)     ((struct exfat_mount *)((vp)->v_mount->mnt_data))

/* Additional function prototypes */
int exfat_get_node(struct mount *mp, uint32_t cluster, enum vtype type, struct vnode **vpp);
int exfat_read_directory(struct vnode *vp, struct uio *uio, int *eofflag);
int exfat_lookup_node(struct vnode *dvp, struct componentname *cnp, struct vnode **vpp);

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

#endif /* _FS_EXFAT_H_ */ 