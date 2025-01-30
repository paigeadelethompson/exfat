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

/* Forward declarations */
struct exfat_mount;
struct exfat_direntry_set;

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
};

/* Error severity levels */
#define FSCK_ERR_FATAL      1   /* Fatal error - abort */
#define FSCK_ERR_SERIOUS    2   /* Serious error - continue with caution */
#define FSCK_ERR_NORMAL     3   /* Normal error - can be fixed */

/* Function prototypes */
int check_boot_sector(struct fsck_exfat_ctx *ctx);
int check_fat(struct fsck_exfat_ctx *ctx);
int check_root_dir(struct fsck_exfat_ctx *ctx);
int check_directory(struct fsck_exfat_ctx *ctx, uint32_t cluster);
int check_file(struct fsck_exfat_ctx *ctx, struct exfat_direntry_set *es);
int check_cluster_chain(struct fsck_exfat_ctx *ctx, uint32_t start_cluster);
void report_error(struct fsck_exfat_ctx *ctx, int level, const char *fmt, ...);

#endif /* _FSCK_EXFAT_H_ */ 