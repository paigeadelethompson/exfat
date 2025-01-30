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

#ifndef _FSCK_EXFAT_H_
#define _FSCK_EXFAT_H_

#include <sys/param.h>
#include <sys/types.h>

/* Exit codes */
#define FSCK_OK              0    /* No errors */
#define FSCK_FIXED          1    /* File system errors corrected */
#define FSCK_RELOAD        2    /* System should be rebooted */
#define FSCK_NOGO          4    /* Major errors left uncorrected */
#define FSCK_SKIP          8    /* File system was skipped */
#define FSCK_ERROR        16    /* Error accessing file system */

/* Program flags */
#define CHECK_PREEN     0x01    /* Preen mode */
#define CHECK_FORCE     0x02    /* Force check */
#define CHECK_VERBOSE   0x04    /* Verbose output */
#define CHECK_PROGRESS  0x08    /* Show progress */

/* Structure for filesystem state */
struct exfat_fsck {
    const char *device;         /* Device name */
    int fd;                     /* Device file descriptor */
    int flags;                  /* Program flags */
    off_t dev_size;            /* Device size */
    struct exfat_boot_record boot;  /* Boot sector */
    uint32_t *fat;             /* FAT table */
    uint8_t *bitmap;           /* Allocation bitmap */
    uint32_t errors;           /* Number of errors found */
    uint32_t fixed;            /* Number of errors fixed */
};

/* Function prototypes */
int check_boot_sector(struct exfat_fsck *fsck);
int check_fat(struct exfat_fsck *fsck);
int check_bitmap(struct exfat_fsck *fsck);
int check_rootdir(struct exfat_fsck *fsck);
int check_clusters(struct exfat_fsck *fsck);

#endif /* _FSCK_EXFAT_H_ */ 