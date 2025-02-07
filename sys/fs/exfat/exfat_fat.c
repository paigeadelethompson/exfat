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

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/endian.h>

#include "exfat.h"
#include "exfat_fat.h"

/*
 * Read a FAT entry
 */
static int
exfat_fat_read(struct exfat_mount *emp, uint32_t cluster, uint32_t *next)
{
    if (bootverbose)
        printf("exfat: reading FAT entry for cluster %u\n", cluster);

    struct buf *bp;
    uint32_t fat_offset, sector, offset;
    int error;

    fat_offset = cluster * sizeof(uint32_t);
    sector = emp->boot.fat_offset + (fat_offset >> EXFAT_SECTOR_BITS);
    offset = fat_offset & (EXFAT_SECTOR_SIZE - 1);

    error = bread(emp->devvp, sector, EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error) {
        if (bootverbose)
            printf("exfat: failed to read FAT sector: %d\n", error);
        brelse(bp);
        return error;
    }

    *next = le32dec((uint8_t *)bp->b_data + offset);
    if (bootverbose)
        printf("exfat: cluster %u -> %u\n", cluster, *next);

    brelse(bp);
    return 0;
}

/*
 * Write a FAT entry
 */
static int
exfat_fat_write(struct exfat_mount *emp, uint32_t cluster, uint32_t next)
{
    if (bootverbose)
        printf("exfat: writing FAT entry: cluster %u -> %u\n", cluster, next);

    struct buf *bp;
    uint32_t fat_offset, sector, offset;
    int error;

    fat_offset = cluster * sizeof(uint32_t);
    sector = emp->boot.fat_offset + (fat_offset >> EXFAT_SECTOR_BITS);
    offset = fat_offset & (EXFAT_SECTOR_SIZE - 1);

    error = bread(emp->devvp, sector, EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error) {
        if (bootverbose)
            printf("exfat: failed to read FAT sector: %d\n", error);
        brelse(bp);
        return error;
    }

    le32enc((uint8_t *)bp->b_data + offset, next);
    error = bwrite(bp);

    if (error) {
        if (bootverbose)
            printf("exfat: failed to write FAT sector: %d\n", error);
        return error;
    }

    if (bootverbose)
        printf("exfat: FAT entry written successfully\n");

    return 0;
}

/*
 * Find first free cluster from bitmap
 */
static int
exfat_find_free_cluster(struct exfat_mount *emp, uint32_t *cluster)
{
    if (bootverbose)
        printf("exfat: searching for free cluster\n");

    struct buf *bp;
    uint32_t sector, bit, byte;
    uint8_t mask;
    int error;

    for (sector = 0; sector < emp->bitmap_sectors; sector++) {
        error = bread(emp->devvp, 
                     emp->boot.cluster_heap_offset - emp->bitmap_sectors + sector,
                     EXFAT_SECTOR_SIZE, NOCRED, &bp);
        if (error) {
            if (bootverbose)
                printf("exfat: failed to read bitmap sector: %d\n", error);
            brelse(bp);
            return error;
        }

        for (byte = 0; byte < EXFAT_SECTOR_SIZE; byte++) {
            if (((uint8_t *)bp->b_data)[byte] != 0xFF) {
                for (bit = 0; bit < 8; bit++) {
                    mask = 1 << bit;
                    if (!(((uint8_t *)bp->b_data)[byte] & mask)) {
                        *cluster = (sector * EXFAT_SECTOR_SIZE + byte) * 8 + bit + 2;
                        if (*cluster >= emp->clusters_count + 2)
                            continue;
                        ((uint8_t *)bp->b_data)[byte] |= mask;
                        error = bwrite(bp);
                        if (bootverbose)
                            printf("exfat: found free cluster %u\n", *cluster);
                        return error;
                    }
                }
            }
        }
        brelse(bp);
    }

    if (bootverbose)
        printf("exfat: no free clusters found\n");
    return ENOSPC;
}

/*
 * Mark cluster as used/free in bitmap
 */
static int
exfat_bitmap_set(struct exfat_mount *emp, uint32_t cluster, int used)
{
    struct buf *bp;
    uint32_t sector, byte, bit;
    uint8_t mask;
    int error;

    cluster -= 2; // Convert to bitmap index
    sector = cluster / (EXFAT_SECTOR_SIZE * 8);
    byte = (cluster % (EXFAT_SECTOR_SIZE * 8)) / 8;
    bit = cluster % 8;
    mask = 1 << bit;

    error = bread(emp->devvp,
                 emp->boot.cluster_heap_offset - emp->bitmap_sectors + sector,
                 EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error) {
        brelse(bp);
        return error;
    }

    if (used)
        ((uint8_t *)bp->b_data)[byte] |= mask;
    else
        ((uint8_t *)bp->b_data)[byte] &= ~mask;

    error = bwrite(bp);
    return error;
}

/*
 * Allocate a new cluster
 */
int
exfat_cluster_alloc(struct exfat_mount *emp, uint32_t *cluster)
{
    if (bootverbose)
        printf("exfat: allocating new cluster\n");
    int error;

    error = exfat_find_free_cluster(emp, cluster);
    if (error) {
        if (bootverbose)
            printf("exfat: failed to find free cluster: %d\n", error);
        return error;
    }

    if (bootverbose)
        printf("exfat: allocated cluster %d\n", *cluster);
    return 0;
}

/*
 * Link two clusters in the FAT
 */
int
exfat_cluster_link(struct exfat_mount *emp, uint32_t current, uint32_t next)
{
    return exfat_fat_write(emp, current, next);
}

/*
 * Free a cluster in the FAT
 */
int
exfat_cluster_free(struct exfat_mount *emp, uint32_t cluster)
{
    if (bootverbose)
        printf("exfat: freeing cluster chain starting at %d\n", cluster);
    uint32_t next;
    int error;

    while (cluster != EXFAT_CLUSTER_END) {
        error = exfat_fat_read(emp, cluster, &next);
        if (error) {
            if (bootverbose)
                printf("exfat: error reading FAT entry: %d\n", error);
            return error;
        }

        error = exfat_bitmap_set(emp, cluster, 0);
        if (error)
            return error;

        error = exfat_fat_write(emp, cluster, EXFAT_CLUSTER_FREE);
        if (error)
            return error;

        cluster = next;
    }

    return 0;
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
        error = exfat_fat_write(emp, *cluster, new_cluster);
        if (error) {
            exfat_fat_write(emp, new_cluster, EXFAT_CLUSTER_FREE);
            return error;
        }
    }

    *cluster = new_cluster;
    return 0;
}

/*
 * Get next cluster in chain
 */
uint32_t
exfat_cluster_next(struct exfat_mount *emp, uint32_t cluster)
{
    uint32_t next;
    int error;

    error = exfat_fat_read(emp, cluster, &next);
    if (error)
        return EXFAT_CLUSTER_END;

    return next;
} 