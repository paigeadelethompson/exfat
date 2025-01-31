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

#ifndef _MKFS_EXFAT_H_
#define _MKFS_EXFAT_H_

#include <sys/types.h>
#include <sys/param.h>

/* Format context structure */
struct mkfs_exfat_ctx {
    const char *device;          /* Device name */
    int fd;                      /* Device file descriptor */
    off_t dev_size;             /* Device size in bytes */
    uint32_t cluster_size;      /* Cluster size in bytes */
    uint32_t volume_serial;     /* Volume serial number */
    char *volume_label;         /* Volume label */
    struct exfat_boot_record boot;  /* Boot sector */
    uint32_t cluster_heap_offset; /* Offset to cluster heap */
    uint32_t cluster_count;     /* Total number of clusters */
    uint32_t bitmap_cluster;    /* First cluster of bitmap */
    uint32_t upcase_cluster;    /* First cluster of upcase table */
    uint32_t root_cluster;      /* First cluster of root directory */
    uint32_t first_cluster;     /* First available cluster */
    int verbose;                /* Verbose output */
};

/* Function prototypes */
int write_boot_sector(struct mkfs_exfat_ctx *ctx);
int write_fat(struct mkfs_exfat_ctx *ctx);
int write_root_dir(struct mkfs_exfat_ctx *ctx);
int write_bitmap(struct mkfs_exfat_ctx *ctx);
int write_upcase_table(struct mkfs_exfat_ctx *ctx);

/* ExFAT filesystem constants */
#define EXFAT_SECTOR_SIZE          512
#define EXFAT_SECTOR_BITS          9
#define EXFAT_MAX_NAMELEN          255
#define EXFAT_LABEL_MAX_LEN        11

/* ExFAT boot sector constants */
#define EXFAT_BOOT_SIGNATURE       0xAA55
#define EXFAT_BOOT_CODE_SIZE       390
#define EXFAT_BOOT_REGION_SIZE     24  /* sectors */

/* ExFAT volume flags */
#define EXFAT_VOL_DIRTY           0x0001
#define EXFAT_VOL_ACTIVE_FAT      0x0002

/* Default formatting parameters */
#define EXFAT_DEFAULT_CLUSTER_SIZE   32768    /* 32KB */
#define EXFAT_MIN_CLUSTER_SIZE       4096     /* 4KB */
#define EXFAT_MAX_CLUSTER_SIZE       32*1024*1024  /* 32MB */
#define EXFAT_DEFAULT_FATS           1        /* Number of FATs */
#define EXFAT_DEFAULT_ROOTDIR_SIZE   65536    /* Root dir size */

/* Formatting limits */
#define EXFAT_MIN_VOLUME_SIZE    (64*1024*1024)   /* 64MB minimum */
#define EXFAT_MAX_VOLUME_SIZE    (128LL*1024*1024*1024*1024)  /* 128TB max */
#define EXFAT_MAX_CLUSTERS       0xFFFFFFF5  /* Maximum valid cluster number */

/* Default values */
#define EXFAT_DEFAULT_REVISION   0x0100  /* Version 1.00 */
#define EXFAT_DEFAULT_DRIVE      0x80    /* First drive */

/* Validation macros */
#define EXFAT_IS_POWER2(n)      (((n) & ((n) - 1)) == 0)
#define EXFAT_VALID_CLUSTERSIZE(n) \
    (EXFAT_IS_POWER2(n) && \
     (n) >= EXFAT_MIN_CLUSTER_SIZE && \
     (n) <= EXFAT_MAX_CLUSTER_SIZE)

/* ExFAT filesystem parameters */
struct exfat_boot_record {
    uint8_t  jump_boot[3];         /* Boot strap short or near jump */
    uint8_t  fs_name[8];           /* "EXFAT   " */
    uint8_t  must_be_zero[53];     /* Zero field */
    uint64_t partition_offset;      /* Partition offset in sectors */
    uint64_t volume_length;        /* Volume length in sectors */
    uint32_t fat_offset;           /* FAT offset in sectors */
    uint32_t fat_length;           /* FAT length in sectors */
    uint32_t cluster_heap_offset;  /* Cluster heap offset in sectors */
    uint32_t cluster_count;        /* Total number of clusters */
    uint32_t root_dir_cluster;     /* First cluster of root directory */
    uint32_t volume_serial;        /* Volume serial number */
    uint16_t fs_revision;          /* Filesystem revision */
    uint16_t volume_flags;         /* Volume flags */
    uint8_t  bytes_per_sector_shift;  /* Bytes per sector shift */
    uint8_t  sectors_per_cluster_shift; /* Sectors per cluster shift */
    uint8_t  number_of_fats;       /* Number of FATs */
    uint8_t  drive_select;         /* Drive select */
    uint8_t  percent_in_use;       /* Percentage of clusters in use */
    uint8_t  reserved[7];          /* Reserved */
    uint8_t  boot_code[EXFAT_BOOT_CODE_SIZE];  /* Boot code */
    uint16_t boot_signature;       /* Boot signature */
};

#endif /* _MKFS_EXFAT_H_ */ 