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
#include <sys/vnode.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/endian.h>

#include "exfat.h"
#include "exfat_node.h"
#include "exfat_dir.h"
#include "exfat_upcase.h"  /* For exfat_upcase() */

/* Forward declarations */
static int exfat_init_entry(struct exfat_mount *emp, uint32_t cluster, struct exfat_node *node);
static int exfat_write_entry(struct exfat_mount *emp, struct exfat_node *node);

/*
 * Initialize directory scanning
 */
int
exfat_scan_directory(struct vnode *vp, struct exfat_scan_ctx *ctx)
{
    if (bootverbose)
        printf("exfat: [%s] scanning directory vnode %p\n", __func__, vp);

    struct exfat_mount *emp = VTOVFSMP(vp);
    struct exfat_node *ep = VTOE(vp);
    int error;

    /* Initialize scan context */
    ctx->emp = emp;
    ctx->cluster = ep->finfo.first_cluster;
    ctx->offset = 0;
    ctx->bp = NULL;

    /* Read first cluster */
    error = bread(emp->devvp,
                 emp->boot.cluster_heap_offset +
                 ((ctx->cluster - 2) << emp->boot.sectors_per_cluster_shift),
                 emp->bytes_per_cluster, NOCRED, &ctx->bp);
    if (error) {
        if (bootverbose)
            printf("exfat: failed to read directory cluster: %d\n", error);
        brelse(ctx->bp);
        return error;
    }

    ctx->entry = (uint8_t *)ctx->bp->b_data;
    if (bootverbose)
        printf("exfat: directory scan initialized\n");
    return 0;
}

/*
 * Read next directory entry set
 */
int
exfat_next_dirent(struct exfat_scan_ctx *ctx, struct exfat_direntry_set *es)
{
    if (bootverbose)
        printf("exfat: [%s] reading next directory entry at offset %u\n", __func__, ctx->offset);

    struct buf *bp;
    uint32_t sector;
    int error;

    /* If we need a new sector */
    if (ctx->bp == NULL || ctx->offset >= EXFAT_SECTOR_SIZE) {
        if (bootverbose)
            printf("exfat: reading new sector for directory entries\n");

        if (ctx->bp != NULL)
            brelse(ctx->bp);

        /* Calculate sector number */
        sector = ctx->emp->boot.cluster_heap_offset +
                ((ctx->cluster - 2) << ctx->emp->boot.sectors_per_cluster_shift) +
                (ctx->offset >> EXFAT_SECTOR_BITS);

        /* Read the sector */
        error = bread(ctx->emp->devvp, sector, EXFAT_SECTOR_SIZE, NOCRED, &bp);
        if (error) {
            if (bootverbose)
                printf("exfat: failed to read directory sector: %d\n", error);
            ctx->bp = NULL;
            return error;
        }

        ctx->bp = bp;
        ctx->entry = (uint8_t *)bp->b_data;
        ctx->offset &= (EXFAT_SECTOR_SIZE - 1);
    }

    /* Check for end of directory */
    if (ctx->entry[ctx->offset] == EXFAT_ENTRY_EOD) {
        if (bootverbose)
            printf("exfat: reached end of directory\n");
        brelse(ctx->bp);
        ctx->bp = NULL;
        return ENOENT;
    }

    /* Skip deleted entries */
    while (ctx->entry[ctx->offset] == EXFAT_ENTRY_DELETED) {
        if (bootverbose)
            printf("exfat: skipping deleted entry\n");
        ctx->offset += sizeof(struct exfat_entry_file);
        if (ctx->offset >= EXFAT_SECTOR_SIZE) {
            /* Need to read next sector */
            return exfat_next_dirent(ctx, es);
        }
    }

    /* Read file entry */
    if (ctx->entry[ctx->offset] != EXFAT_ENTRY_FILE) {
        if (bootverbose)
            printf("exfat: [%s] invalid directory entry type: %d\n", __func__, ctx->entry[ctx->offset]);
        return EINVAL;
    }

    if (bootverbose)
        printf("exfat: [%s] reading file entry at offset %u\n", __func__, ctx->offset);

    memcpy(&es->file, ctx->entry + ctx->offset, sizeof(struct exfat_entry_file));
    ctx->offset += sizeof(struct exfat_entry_file);

    /* Read stream entry */
    if (ctx->offset >= EXFAT_SECTOR_SIZE) {
        error = exfat_next_dirent(ctx, es);
        if (error)
            return error;
    }
    if (ctx->entry[ctx->offset] != EXFAT_ENTRY_STREAM)
        return EINVAL;

    memcpy(&es->stream, ctx->entry + ctx->offset, sizeof(struct exfat_entry_stream));
    ctx->offset += sizeof(struct exfat_entry_stream);

    /* Read name entries */
    es->name_count = 0;
    while (es->name_count < es->file.secondary_count - 1) {
        if (ctx->offset >= EXFAT_SECTOR_SIZE) {
            error = exfat_next_dirent(ctx, es);
            if (error)
                return error;
        }
        if (ctx->entry[ctx->offset] != EXFAT_ENTRY_NAME)
            return EINVAL;

        memcpy(&es->name[es->name_count], ctx->entry + ctx->offset,
               sizeof(struct exfat_entry_name));
        es->name_count++;
        ctx->offset += sizeof(struct exfat_entry_name);
    }

    return 0;
}

/*
 * Clean up directory scanning context
 */
void
exfat_scan_cleanup(struct exfat_scan_ctx *ctx)
{
    if (bootverbose)
        printf("exfat: cleaning up directory scan\n");

    if (ctx->bp != NULL) {
        brelse(ctx->bp);
        ctx->bp = NULL;
    }
}

/*
 * Compare filename with directory entry
 */
int
exfat_name_match(struct exfat_mount *emp, const struct exfat_direntry_set *es,
                const char *name, size_t len)
{
    if (bootverbose)
        printf("exfat: [%s] comparing name '%s' (len %zu)\n", __func__, name, len);

    uint16_t uname[256];
    size_t ulen, i;

    /* Convert ASCII name to UTF-16 */
    for (i = 0; i < len && i < 255; i++)
        uname[i] = (uint16_t)name[i];
    ulen = i;

    /* Check length first */
    if (ulen != es->stream.name_length) {
        if (bootverbose)
            printf("exfat: [%s] name length mismatch (%zu != %u)\n", 
                   __func__, ulen, es->stream.name_length);
        return 0;
    }

    int match = (exfat_name_compare(emp, uname, es->name[0].name, 
                                   MIN(ulen, es->stream.name_length)) == 0);
    if (bootverbose)
        printf("exfat: [%s] name comparison %s\n", __func__, match ? "matched" : "failed");

    return match;
}

/*
 * Calculate name hash
 */
uint16_t
exfat_calc_name_hash(struct exfat_mount *emp, const uint16_t *name, size_t len)
{
    uint16_t hash = 0;
    size_t i;

    for (i = 0; i < len; i++) {
        uint16_t c = exfat_upcase(emp, le16toh(name[i]));
        hash = ((hash << 15) | (hash >> 1)) + (c & 0xFF);
        hash = ((hash << 15) | (hash >> 1)) + (c >> 8);
    }

    return hash;
}

/*
 * Initialize a directory entry
 */
static int
exfat_init_entry(struct exfat_mount *emp, uint32_t cluster, struct exfat_node *node)
{
    /* Initialize basic node info */
    node->cluster = cluster;
    node->mp = emp->mp;
    
    return 0;
}

/*
 * Write a directory entry to disk
 */
static int
exfat_write_entry(struct exfat_mount *emp, struct exfat_node *node)
{
    struct buf *bp;
    uint32_t sector;
    int error;

    /* Calculate sector number */
    sector = emp->boot.cluster_heap_offset +
             ((node->cluster - 2) << emp->boot.sectors_per_cluster_shift);

    /* Read sector */
    error = bread(emp->devvp, sector, EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error) {
        brelse(bp);
        return error;
    }

    /* Write entry data */
    memcpy(bp->b_data, &node->finfo, sizeof(struct exfat_fileinfo));
    
    error = bwrite(bp);
    return error;
}

/*
 * Create directory entry
 */
int
exfat_create_direntry(struct exfat_mount *emp, uint32_t cluster, 
                     struct exfat_entry_file *file,
                     struct exfat_entry_stream *stream, 
                     struct exfat_entry_name *name,
                     size_t name_count)
{
    struct exfat_node *ep = VTOE(emp->devvp);
    int error;

    /* Initialize new entry */
    error = exfat_init_entry(emp, cluster, ep);
    if (error)
        return error;

    /* Set entry data */
    ep->type = EXFAT_ENTRY_FILE;
    memcpy(&ep->finfo, file, sizeof(struct exfat_entry_file));
    memcpy(&ep->stream, stream, sizeof(struct exfat_entry_stream));
    memcpy(&ep->name, name, name_count * sizeof(struct exfat_entry_name));

    /* Write entry to disk */
    error = exfat_write_entry(emp, ep);
    if (error)
        return error;

    return 0;
}

/*
 * Remove directory entry
 */
int
exfat_remove_direntry(struct exfat_mount *emp, uint32_t cluster)
{
    struct exfat_node *ep = VTOE(emp->devvp);
    int error;

    /* Initialize entry */
    error = exfat_init_entry(emp, cluster, ep);
    if (error)
        return error;

    /* Set entry type to deleted */
    ep->type = EXFAT_ENTRY_DELETED;

    /* Write entry to disk */
    error = exfat_write_entry(emp, ep);
    if (error)
        return error;

    return 0;
} 