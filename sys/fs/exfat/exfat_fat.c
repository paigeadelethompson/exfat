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

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/endian.h>

#include "exfat.h"

/*
 * Write a FAT entry
 */
static int
exfat_write_fat_entry(struct exfat_mount *emp, uint32_t cluster, uint32_t value)
{
    struct buf *bp;
    uint32_t fat_offset;
    uint32_t sec_offset;
    uint32_t *entry;
    int error;

    fat_offset = cluster * sizeof(uint32_t);
    sec_offset = fat_offset >> EXFAT_SECTOR_BITS;

    error = bread(EXFAT_DEV(emp->mp),
                 emp->boot.fat_offset + sec_offset,
                 EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error) {
        brelse(bp);
        return error;
    }

    entry = (uint32_t *)(bp->b_data + (fat_offset & (EXFAT_SECTOR_SIZE - 1)));
    *entry = htole32(value);
    error = bwrite(bp);

    return error;
}

/*
 * Find a free cluster
 */
static int
exfat_find_free_cluster(struct exfat_mount *emp, uint32_t start, uint32_t *cluster)
{
    uint32_t i;
    int error;

    for (i = start; i < emp->boot.cluster_count + 2; i++) {
        uint32_t value;
        error = exfat_read_fat_entry(emp, i, &value);
        if (error)
            return error;
        if (value == EXFAT_CLUSTER_FREE) {
            *cluster = i;
            return 0;
        }
    }

    /* Try from beginning if not found */
    if (start > 2) {
        for (i = 2; i < start; i++) {
            uint32_t value;
            error = exfat_read_fat_entry(emp, i, &value);
            if (error)
                return error;
            if (value == EXFAT_CLUSTER_FREE) {
                *cluster = i;
                return 0;
            }
        }
    }

    return ENOSPC;
}

/*
 * Allocate a new cluster
 */
int
exfat_cluster_alloc(struct exfat_mount *emp, uint32_t *cluster)
{
    int error;

    /* Find a free cluster in bitmap */
    error = exfat_find_free_cluster(emp, 2, cluster);
    if (error)
        return error;

    /* Mark cluster as allocated in bitmap */
    error = exfat_update_bitmap(emp, *cluster, 1);
    if (error)
        return error;

    /* Mark cluster as end of chain */
    error = exfat_write_fat_entry(emp, *cluster, EXFAT_CLUSTER_END);
    if (error) {
        exfat_update_bitmap(emp, *cluster, 0);
        return error;
    }

    return 0;
}

/*
 * Free a cluster chain
 */
void
exfat_cluster_free(struct exfat_mount *emp, uint32_t cluster)
{
    uint32_t next;

    while (cluster != EXFAT_CLUSTER_END && cluster >= 2) {
        if (exfat_read_fat_entry(emp, cluster, &next) != 0)
            break;
        exfat_update_bitmap(emp, cluster, 0);
        exfat_write_fat_entry(emp, cluster, EXFAT_CLUSTER_FREE);
        cluster = next;
    }
}

/*
 * Extend cluster chain
 */
int
exfat_cluster_extend(struct exfat_mount *emp, uint32_t *cluster)
{
    uint32_t new_cluster;
    int error;

    /* Allocate new cluster */
    error = exfat_cluster_alloc(emp, &new_cluster);
    if (error)
        return error;

    /* Link it to the chain if we have a previous cluster */
    if (*cluster != 0) {
        error = exfat_write_fat_entry(emp, *cluster, new_cluster);
        if (error) {
            exfat_write_fat_entry(emp, new_cluster, EXFAT_CLUSTER_FREE);
            return error;
        }
    }

    *cluster = new_cluster;
    return 0;
} 