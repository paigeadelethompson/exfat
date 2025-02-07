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

#ifndef _FS_EXFAT_FAT_H_
#define _FS_EXFAT_FAT_H_

#include "exfat.h"

/* FAT entry values */
#define EXFAT_CLUSTER_FREE       0x00000000  /* Free cluster */
#define EXFAT_CLUSTER_BAD        0xFFFFFFF7  /* Bad cluster */
#define EXFAT_CLUSTER_END        0xFFFFFFFF  /* End of chain */

/* Function prototypes */
int exfat_fat_init(struct exfat_mount *emp);
int exfat_fat_sync(struct exfat_mount *emp);
int exfat_fat_read(struct exfat_mount *emp, uint32_t cluster, uint32_t *next);
int exfat_fat_write(struct exfat_mount *emp, uint32_t cluster, uint32_t next);
int exfat_cluster_alloc(struct exfat_mount *emp, uint32_t *cluster);
int exfat_cluster_free(struct exfat_mount *emp, uint32_t cluster);
int exfat_cluster_next(struct exfat_mount *emp, uint32_t cluster);
int exfat_cluster_link(struct exfat_mount *emp, uint32_t cluster, uint32_t next);
int exfat_cluster_extend(struct exfat_mount *emp, uint32_t *cluster);

/* Cluster allocation and bad sector handling */
int exfat_cluster_alloc_sequence(struct exfat_mount *emp, uint32_t count, 
                                uint32_t *first_cluster);
int exfat_mark_cluster_bad(struct exfat_mount *emp, uint32_t cluster);

#endif /* _FS_EXFAT_FAT_H_ */ 