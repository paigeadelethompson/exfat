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

#ifndef _FS_EXFAT_VOLUME_H_
#define _FS_EXFAT_VOLUME_H_

#include <sys/param.h>
#include <sys/systm.h>

/* Volume label entry */
struct exfat_entry_label {
    uint8_t  type;               /* 0x83 */
    uint8_t  character_count;    /* Length of label (max 11) */
    uint16_t unicode_label[11];  /* UTF-16 encoded label */
    uint8_t  reserved[8];
} __packed;

/* Bitmap entry */
struct exfat_entry_bitmap {
    uint8_t  type;               /* 0x81 */
    uint8_t  flags;
    uint8_t  reserved[18];
    uint32_t first_cluster;      /* First cluster of bitmap */
    uint64_t data_length;        /* Size of bitmap in bytes */
} __packed;

/* Function prototypes */
int exfat_read_volume_label(struct exfat_mount *emp);
int exfat_write_volume_label(struct exfat_mount *emp, const char *label, size_t len);
int exfat_init_bitmap(struct exfat_mount *emp);
void exfat_cleanup_bitmap(struct exfat_mount *emp);

/* Bitmap operations */
int exfat_get_cluster_status(struct exfat_mount *emp, uint32_t cluster, int *used);
int exfat_find_free_cluster(struct exfat_mount *emp, uint32_t start, uint32_t *cluster);
int exfat_update_bitmap(struct exfat_mount *emp, uint32_t cluster, int allocated);

/* Volume serial number operations */
int exfat_update_serial(struct exfat_mount *emp);
void exfat_serial_to_string(uint32_t serial, char *str);

/* Volume flag operations */
int exfat_set_volume_dirty(struct exfat_mount *emp, int dirty);
int exfat_set_active_fat(struct exfat_mount *emp, int second_fat);
int exfat_volume_is_dirty(struct exfat_mount *emp);
int exfat_get_active_fat(struct exfat_mount *emp);

/* Time conversion functions */
void exfat_unix2exfat(struct timespec *ts, struct exfat_timespec *extime);
void exfat_exfat2unix(struct exfat_timespec *extime, struct timespec *ts);
int exfat_update_volume_time(struct exfat_mount *emp);

/* Volume statistics */
int exfat_update_percent_in_use(struct exfat_mount *emp);

#endif /* _FS_EXFAT_VOLUME_H_ */ 