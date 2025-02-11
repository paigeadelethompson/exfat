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
 
#ifndef _FS_EXFAT_DIR_H_
#define _FS_EXFAT_DIR_H_

#include <sys/param.h>
#include <sys/types.h>
#include <sys/kernel.h>
#include "exfat.h"

/* Function prototypes */
int exfat_scan_directory(struct vnode *vp, struct exfat_scan_ctx *ctx);
int exfat_next_dirent(struct exfat_scan_ctx *ctx, struct exfat_direntry_set *es);
void exfat_scan_cleanup(struct exfat_scan_ctx *ctx);
int exfat_name_match(struct exfat_mount *emp, const struct exfat_direntry_set *es,
                    const char *name, size_t len);
uint16_t exfat_calc_name_hash(struct exfat_mount *emp, const uint16_t *name, size_t len);

/* Directory entry operations */
int exfat_create_entry(struct vnode *dvp, const char *name, int namelen,
                      uint16_t attr, struct timespec *ctime,
                      struct exfat_direntry_set *es);
int exfat_remove_entry(struct vnode *dvp, struct exfat_direntry_set *es, off_t offset);

/* Add these prototypes */
int exfat_create_direntry(struct exfat_mount *emp, uint32_t cluster,
                         struct exfat_entry_file *file,
                         struct exfat_entry_stream *stream,
                         struct exfat_entry_name *name,
                         size_t name_count);
int exfat_remove_direntry(struct exfat_mount *emp, uint32_t cluster);

#endif /* _FS_EXFAT_DIR_H_ */ 