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
 
#ifndef _FS_EXFAT_VOLUME_H_
#define _FS_EXFAT_VOLUME_H_

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include "exfat.h"

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

/* Sector checksum operations */
int exfat_verify_sector(struct buf *bp);
void exfat_update_sector_checksum(struct buf *bp);

/* Volume operations */
int exfat_read_boot_sector(struct exfat_mount *emp);
int exfat_read_volume_label(struct exfat_mount *emp);
int exfat_write_volume_label(struct exfat_mount *emp, const char *label, size_t len);
int exfat_read_rootdir(struct exfat_mount *emp);

/* Mount-time directory scanning without vnode requirement */
int exfat_scan_directory_raw(struct exfat_mount *emp, uint32_t cluster, struct exfat_scan_ctx *ctx);

#endif /* _FS_EXFAT_VOLUME_H_ */ 