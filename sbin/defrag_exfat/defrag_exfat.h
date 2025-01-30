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

#ifndef _DEFRAG_EXFAT_H_
#define _DEFRAG_EXFAT_H_

#include <sys/types.h>

/* Define __packed attribute once */
#ifndef __packed
#define __packed __attribute__((__packed__))
#endif

/* All structures in this file are packed */
#pragma pack(push, 1)

#ifdef __APPLE__
#define roundup2(x, y)  (((x)+((y)-1))&(~((y)-1)))
#include <libkern/OSByteOrder.h>
#define htobe16(x) OSSwapHostToBigInt16(x)
#define htole16(x) OSSwapHostToLittleInt16(x)
#define be16toh(x) OSSwapBigToHostInt16(x)
#define le16toh(x) OSSwapLittleToHostInt16(x)
#define htobe32(x) OSSwapHostToBigInt32(x)
#define htole32(x) OSSwapHostToLittleInt32(x)
#define be32toh(x) OSSwapBigToHostInt32(x)
#define le32toh(x) OSSwapLittleToHostInt32(x)
#define htobe64(x) OSSwapHostToBigInt64(x)
#define htole64(x) OSSwapHostToLittleInt64(x)
#define be64toh(x) OSSwapBigToHostInt64(x)
#define le64toh(x) OSSwapLittleToHostInt64(x)
#endif

/* Debug levels */
#define DEBUG_NONE    0
#define DEBUG_BASIC   1
#define DEBUG_DETAIL  2
#define DEBUG_DUMP    3

/* ExFAT constants */
#define EXFAT_SECTOR_SIZE          512
#define EXFAT_MAX_CLUSTERS         0xFFFFFFF5
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

/* ExFAT boot sector */
struct exfat_boot_sector {
    uint8_t  jump_boot[3];        /* Boot strap short or near jump */
    uint8_t  fs_name[8];          /* "EXFAT   " */
    uint8_t  must_be_zero[53];    /* Zero field */
    uint64_t partition_offset;     /* Partition offset in sectors */
    uint64_t volume_length;       /* Volume size in sectors */
    uint32_t fat_offset;          /* FAT offset in sectors */
    uint32_t fat_length;          /* FAT length in sectors */
    uint32_t cluster_heap_offset; /* Cluster heap offset */
    uint32_t cluster_count;       /* Total number of clusters */
    uint32_t root_dir_cluster;    /* First cluster of root dir */
    uint32_t volume_serial;       /* Volume serial number */
    uint16_t fs_revision;         /* Filesystem revision */
    uint16_t volume_flags;        /* Volume flags */
    uint8_t  bytes_per_sector_shift;    /* Log2 of sector size */
    uint8_t  sectors_per_cluster_shift; /* Log2 of cluster size */
    uint8_t  number_of_fats;      /* Number of FATs */
    uint8_t  drive_select;        /* Drive select */
    uint8_t  percent_in_use;      /* Percentage in use */
    uint8_t  reserved[7];         /* Reserved */
} __packed;

/* Directory entry structures */
struct exfat_entry_file {
    uint8_t  type;                /* Entry type */
    uint8_t  secondary_count;     /* Count of secondary entries */
    uint16_t checksum;           /* Name hash */
    uint16_t file_attributes;    /* File attributes */
    uint16_t reserved1;
    uint32_t create_timestamp;   /* Create timestamp */
    uint32_t last_modified_timestamp; /* Last modified timestamp */
    uint32_t last_access_timestamp;   /* Last access timestamp */
    uint8_t  create_time_ms;     /* 10ms units */
    uint8_t  last_modified_time_ms; /* 10ms units */
    uint8_t  create_tz;          /* Timezone offset */
    uint8_t  last_modified_tz;   /* Timezone offset */
    uint8_t  last_access_tz;     /* Timezone offset */
    uint8_t  reserved2[7];
} __packed;

struct exfat_entry_stream {
    uint8_t  type;               /* Entry type */
    uint8_t  flags;              /* Flags */
    uint8_t  reserved1;
    uint8_t  name_length;        /* Name length */
    uint16_t name_hash;          /* Name hash */
    uint16_t reserved2;
    uint64_t valid_data_length;  /* Valid data length */
    uint32_t reserved3;
    uint32_t first_cluster;      /* First cluster */
    uint64_t data_length;        /* Data length */
} __packed;

/* File info for defragmentation */
struct file_info {
    char *path;                  /* File path */
    uint32_t first_cluster;      /* First cluster */
    uint64_t size;              /* File size in bytes */
    uint32_t fragment_count;    /* Number of fragments */
    struct file_fragments *fragments; /* List of fragments */
    struct file_info *next;     /* Next file in list */
};

/* Defragmentation context */
struct defrag_exfat_ctx {
    const char *device;          /* Device/file being defragmented */
    int fd;                      /* File descriptor */
    int verbose;                 /* Verbosity level */
    int dry_run;                /* Don't actually modify filesystem */
    int force;                  /* Force defragmentation */
    
    /* Filesystem info */
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_count;
    uint32_t fat_offset;        /* FAT offset in sectors */
    uint32_t fat_length;        /* FAT length in sectors */
    uint32_t cluster_heap_offset; /* First cluster sector */
    uint32_t root_dir_cluster;   /* First cluster of root directory */
    
    /* File tracking */
    struct file_info *file_list; /* List of files */
    
    /* Statistics */
    uint32_t files_total;       /* Total files found */
    uint32_t files_fragmented;  /* Number of fragmented files */
    uint32_t files_defragged;   /* Number of files defragmented */
    uint32_t fragments_eliminated; /* Number of fragments eliminated */
    uint64_t bytes_moved;       /* Total bytes moved */
    
    /* Bitmap tracking */
    uint8_t *allocation_bitmap;  /* Cluster allocation bitmap */
    uint32_t bitmap_cluster;     /* First cluster of bitmap */
    
    /* Progress callback */
    void (*progress_cb)(struct defrag_exfat_ctx *ctx, 
                       const char *phase,
                       uint32_t current, 
                       uint32_t total);
};

/* File fragment info */
struct file_fragments {
    uint32_t start_cluster;     /* First cluster */
    uint32_t length;           /* Number of contiguous clusters */
    struct file_fragments *next;
};

/* Function prototypes */
int defrag_exfat(struct defrag_exfat_ctx *ctx);
int analyze_fragmentation(struct defrag_exfat_ctx *ctx);
int defrag_file(struct defrag_exfat_ctx *ctx, const char *path, struct file_fragments *frags);
void print_progress(struct defrag_exfat_ctx *ctx, const char *phase, 
                   uint32_t current, uint32_t total);
static int read_boot_sector(struct defrag_exfat_ctx *ctx);
static int read_fat(struct defrag_exfat_ctx *ctx);
static int scan_directory(struct defrag_exfat_ctx *ctx, uint32_t cluster, const char *path);
static int get_cluster_chain(struct defrag_exfat_ctx *ctx, uint32_t start_cluster, 
                           struct file_fragments **fragments);

#pragma pack(pop)

#endif /* _DEFRAG_EXFAT_H_ */ 