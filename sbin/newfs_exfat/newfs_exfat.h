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

#endif /* _MKFS_EXFAT_H_ */ 