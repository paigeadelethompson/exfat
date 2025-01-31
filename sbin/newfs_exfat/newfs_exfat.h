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

/* ExFAT file attributes */
#define EXFAT_ATTR_READ_ONLY       0x0001
#define EXFAT_ATTR_HIDDEN          0x0002
#define EXFAT_ATTR_SYSTEM          0x0004
#define EXFAT_ATTR_VOLUME          0x0008
#define EXFAT_ATTR_DIRECTORY       0x0010
#define EXFAT_ATTR_ARCHIVE         0x0020

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

/* Format context structure */
struct mkfs_exfat_ctx {
    const char *device;          /* Device name */
    int fd;                      /* Device file descriptor */
    uint64_t total_sectors;      /* Total sectors on device */
    uint32_t bytes_per_sector;   /* Bytes per sector (usually 512) */
    uint32_t sectors_per_cluster; /* Sectors per cluster */
    uint32_t cluster_count;      /* Number of clusters */
    uint32_t fat_sectors;        /* Sectors per FAT */
    uint8_t number_of_fats;      /* Number of FATs (1 or 2) */
    struct exfat_boot_record boot;  /* Boot sector */
    uint32_t cluster_heap_offset; /* Offset to cluster heap */
    uint32_t bitmap_cluster;    /* First cluster of bitmap */
    uint32_t upcase_cluster;    /* First cluster of upcase table */
    uint32_t root_cluster;      /* First cluster of root directory */
    uint32_t first_cluster;     /* First available cluster */
    uint32_t volume_serial;     /* Volume serial number */
    const char *volume_label;   /* Volume label */
    int verbose;                /* Verbose output */
};

/* Directory entry structures */
struct exfat_entry_file {
    uint8_t  type;                 /* Entry type */
    uint8_t  secondary_count;      /* Count of secondary entries */
    uint16_t checksum;            /* Name hash */
    uint16_t file_attributes;     /* File attributes */
    uint16_t reserved1;
    uint32_t create_timestamp;    /* Create timestamp */
    uint32_t last_modified_timestamp; /* Last modified timestamp */
    uint32_t last_access_timestamp;   /* Last access timestamp */
    uint8_t  create_time_ms;      /* 10ms units */
    uint8_t  last_modified_time_ms; /* 10ms units */
    uint8_t  create_tz;           /* Timezone offset */
    uint8_t  last_modified_tz;    /* Timezone offset */
    uint8_t  last_access_tz;      /* Timezone offset */
    uint8_t  reserved2[7];
};

struct exfat_entry_stream {
    uint8_t  type;                /* Entry type */
    uint8_t  flags;               /* Flags */
    uint8_t  reserved1;
    uint8_t  name_length;         /* Name length */
    uint16_t name_hash;           /* Name hash */
    uint16_t reserved2;
    uint64_t valid_data_length;   /* Valid data length */
    uint32_t reserved3;
    uint32_t first_cluster;       /* First cluster */
    uint64_t data_length;         /* Data length */
};

struct exfat_entry_name {
    uint8_t  type;                /* Entry type */
    uint8_t  reserved1;
    uint16_t name[15];           /* UTF-16 characters */
};

struct exfat_direntry_set {
    struct exfat_entry_file file;
    struct exfat_entry_stream stream;
    struct exfat_entry_name name[16];
    int name_count;
};

struct exfat_entry_upcase {
    uint8_t  type;                /* Entry type */
    uint8_t  reserved1[3];
    uint32_t checksum;           /* Table checksum */
    uint8_t  reserved2[12];
    uint32_t first_cluster;       /* First cluster */
    uint64_t data_length;         /* Size of table in bytes */
};

struct exfat_entry_label {
    uint8_t  type;                /* Entry type */
    uint8_t  character_count;     /* Number of characters */
    uint16_t unicode_label[11];   /* UTF-16 characters */
    uint8_t  reserved[8];
};

/* Function prototypes */
int write_boot_sector(struct mkfs_exfat_ctx *ctx);
int write_fat(struct mkfs_exfat_ctx *ctx);
int write_root_dir(struct mkfs_exfat_ctx *ctx);
int write_bitmap(struct mkfs_exfat_ctx *ctx);
int write_upcase_table(struct mkfs_exfat_ctx *ctx);
void unix_time_to_exfat(const struct timespec *ts, uint32_t *date, uint32_t *time);
int exfat_utf8_to_utf16(const char *utf8, uint16_t *utf16, size_t maxout, size_t *lenout);

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

#endif /* _MKFS_EXFAT_H_ */ 