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

#ifndef _FSCK_EXFAT_H_
#define _FSCK_EXFAT_H_

#include <sys/types.h>
#include <sys/param.h>
#include <time.h>

/* ExFAT timestamp macros */
#define EXFAT_YEAR(date)    (((date) >> 9) + 1980)
#define EXFAT_MONTH(date)   (((date) >> 5) & 0xF)
#define EXFAT_DAY(date)     ((date) & 0x1F)
#define EXFAT_HOUR(time)    ((time) >> 11)
#define EXFAT_MINUTE(time)  (((time) >> 5) & 0x3F)
#define EXFAT_SECOND(time)  (((time) & 0x1F) << 1)

/* ExFAT filesystem constants */
#define EXFAT_SECTOR_SIZE          512
#define EXFAT_SECTOR_BITS          9
#define EXFAT_MAX_NAMELEN          255
#define EXFAT_LABEL_MAX_LEN        11

/* ExFAT cluster status */
#define EXFAT_CLUSTER_FREE         0x00000000
#define EXFAT_CLUSTER_BAD          0xFFFFFFF7
#define EXFAT_CLUSTER_END          0xFFFFFFFF

/* ExFAT directory entry types */
#define EXFAT_ENTRY_EOD            0x00
#define EXFAT_ENTRY_DELETED        0xE5
#define EXFAT_ENTRY_BITMAP         0x81
#define EXFAT_ENTRY_UPCASE         0x82
#define EXFAT_ENTRY_LABEL          0x83
#define EXFAT_ENTRY_FILE           0x85
#define EXFAT_ENTRY_STREAM         0xC0
#define EXFAT_ENTRY_NAME           0xC1

/* ExFAT file attributes */
#define EXFAT_ATTR_READ_ONLY       0x0001
#define EXFAT_ATTR_HIDDEN          0x0002
#define EXFAT_ATTR_SYSTEM          0x0004
#define EXFAT_ATTR_VOLUME          0x0008
#define EXFAT_ATTR_DIRECTORY       0x0010
#define EXFAT_ATTR_ARCHIVE         0x0020

/* ExFAT boot sector constants */
#define EXFAT_BOOT_SIGNATURE       0xAA55
#define EXFAT_BOOT_CODE_SIZE       390
#define EXFAT_BOOT_REGION_SIZE     24  /* sectors */

/* ExFAT volume flags */
#define EXFAT_VOL_DIRTY           0x0001
#define EXFAT_VOL_ACTIVE_FAT      0x0002

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

/* Directory entry size */
#define EXFAT_ENTRY_SIZE          32

/* Maximum number of directory entries */
#define EXFAT_MAX_DIR_ENTRIES     65536

/* Maximum number of secondary entries */
#define EXFAT_MAX_SECONDARY       17   /* 1 stream + 16 name entries */

/* Name entry */
struct exfat_entry_name {
    uint8_t  type;                /* Entry type */
    uint8_t  reserved1;
    uint16_t name[15];           /* UTF-16 characters */
};

/* Bitmap entry */
struct exfat_entry_bitmap {
    uint8_t  type;                /* Entry type */
    uint8_t  flags;               /* Flags */
    uint8_t  reserved[18];
    uint32_t first_cluster;       /* First cluster of bitmap */
    uint64_t data_length;         /* Size of bitmap in bytes */
};

/* Upcase table entry */
struct exfat_entry_upcase {
    uint8_t  type;                /* Entry type */
    uint8_t  reserved1[3];
    uint32_t checksum;           /* Table checksum */
    uint8_t  reserved2[12];
    uint32_t first_cluster;       /* First cluster of table */
    uint64_t data_length;         /* Size of table in bytes */
};

/* Volume label entry */
struct exfat_entry_label {
    uint8_t  type;                /* Entry type */
    uint8_t  character_count;     /* Number of characters */
    uint16_t volume_label[11];    /* UTF-16 characters */
    uint8_t  reserved[8];
};

/* Mount structure */
struct exfat_mount {
    struct exfat_boot_record boot;  /* Boot sector */
    uint32_t *fat;                /* FAT table */
    uint8_t *bitmap;              /* Allocation bitmap */
    uint16_t *upcase;             /* Upcase table */
    uint32_t bitmap_cluster;      /* First cluster of bitmap */
};

/* Directory entry set */
struct exfat_direntry_set {
    struct exfat_entry_file file;
    struct exfat_entry_stream stream;
    struct exfat_entry_name name[16];
    int name_count;
};

/* fsck context structure */
struct fsck_exfat_ctx {
    const char *device;          /* Device name */
    int fd;                      /* Device file descriptor */
    struct exfat_mount *emp;     /* Mount structure */
    int modified;                /* Set if filesystem was modified */
    int errors;                  /* Number of errors found */
    int fix_errors;              /* Fix errors if found */
    int verbose;                 /* Verbose output */
    uint32_t lost_found_cluster; /* First cluster of lost+found directory */
    uint32_t next_lost_file;    /* Counter for lost file names */
    uint32_t bitmap_cluster;     /* First cluster of allocation bitmap */
};

/* Error severity levels */
#define FSCK_ERR_FATAL      1   /* Fatal error - abort */
#define FSCK_ERR_SERIOUS    2   /* Serious error - continue with caution */
#define FSCK_ERR_NORMAL     3   /* Normal error - can be fixed */

/* Function prototypes */
/* Core checking functions */
int check_boot_sector(struct fsck_exfat_ctx *ctx);
int check_fat(struct fsck_exfat_ctx *ctx);
int check_root_dir(struct fsck_exfat_ctx *ctx);
int check_directory(struct fsck_exfat_ctx *ctx, uint32_t cluster);
int check_file(struct fsck_exfat_ctx *ctx, struct exfat_direntry_set *es);
int check_bitmap(struct fsck_exfat_ctx *ctx);
int check_cluster_chain(struct fsck_exfat_ctx *ctx, uint32_t start_cluster, uint64_t expected_size);
int check_upcase_table(struct fsck_exfat_ctx *ctx);

/* Utility functions */
void report_error(struct fsck_exfat_ctx *ctx, int level, const char *fmt, ...);
uint16_t exfat_calc_name_hash(struct exfat_mount *emp, const uint16_t *name, size_t len);
uint16_t exfat_checksum_direntry(struct exfat_direntry_set *es);
int write_cluster(struct fsck_exfat_ctx *ctx, uint32_t cluster, const void *buffer);
int write_direntry(struct fsck_exfat_ctx *ctx, uint32_t dir_cluster, struct exfat_direntry_set *es);
int find_free_cluster(struct fsck_exfat_ctx *ctx, uint32_t *cluster);
int get_next_cluster(struct fsck_exfat_ctx *ctx, uint32_t cluster, uint32_t *next);
void unix_time_to_exfat(const struct timespec *ts, uint32_t *date, uint32_t *time);
int exfat_utf8_to_utf16(const char *utf8, uint16_t *utf16, size_t maxout, size_t *lenout);

/* Recovery functions */
int create_lost_found_dir(struct fsck_exfat_ctx *ctx);
int create_lost_file(struct fsck_exfat_ctx *ctx, uint32_t cluster);

#endif /* _FSCK_EXFAT_H_ */ 