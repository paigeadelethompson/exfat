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
    struct exfat_mount *emp;
    struct exfat_node *ep;
    struct buf *bp = NULL;
    uint32_t sector;
    int error = 0;

    /* Basic parameter validation */
    if (vp == NULL || ctx == NULL) {
        printf("exfat: [exfat_scan_directory] NULL parameters\n");
        return EINVAL;
    }

    /* Validate vnode before accessing any fields */
    if (vp->v_type != VDIR) {
        printf("exfat: [exfat_scan_directory] not a directory vnode\n");
        return EINVAL;
    }

    /* Get mount point carefully */
    struct mount *mp = vp->v_mount;
    if (mp == NULL) {
        printf("exfat: [exfat_scan_directory] NULL mount pointer\n");
        return EINVAL;
    }

    /* Get mount data carefully */
    emp = mp->mnt_data;
    if (emp == NULL) {
        printf("exfat: [exfat_scan_directory] NULL mount data\n");
        return EINVAL;
    }

    /* Get node data carefully */
    ep = vp->v_data;
    if (ep == NULL) {
        printf("exfat: [exfat_scan_directory] NULL node\n");
        return EINVAL;
    }

    /* Validate node cluster */
    if (ep->cluster == 0) {
        printf("exfat: [exfat_scan_directory] invalid cluster 0\n");
        return EINVAL;
    }

    if (bootverbose) {
        printf("exfat: [exfat_scan_directory] scanning directory:\n");
        printf("  vnode: %p\n", vp);
        printf("  mount: %p\n", emp);
        printf("  node: %p\n", ep);
        printf("  cluster: %u\n", ep->cluster);
    }

    /* Initialize scan context */
    ctx->emp = emp;
    ctx->cluster = ep->cluster;
    ctx->offset = 0;
    ctx->bp = NULL;
    ctx->entry = NULL;

    /* Validate cluster number */
    if (ctx->cluster < EXFAT_CLUSTER_FIRST || 
        ctx->cluster >= emp->clusters_count + 2) {
        printf("exfat: [exfat_scan_directory] invalid cluster %u\n", ctx->cluster);
        return EINVAL;
    }

    /* Calculate first sector of cluster */
    sector = emp->boot.cluster_heap_offset + 
            ((ctx->cluster - 2) << emp->boot.sectors_per_cluster_shift);

    if (bootverbose) {
        printf("exfat: [exfat_scan_directory] reading first sector:\n");
        printf("  cluster_heap_offset: %u\n", emp->boot.cluster_heap_offset);
        printf("  sectors_per_cluster_shift: %u\n", emp->boot.sectors_per_cluster_shift);
        printf("  calculated sector: %u\n", sector);
    }

    /* Validate sector number */
    if (sector >= emp->boot.volume_length) {
        printf("exfat: [exfat_scan_directory] sector %u beyond volume length %lu\n",
               sector, emp->boot.volume_length);
        return EINVAL;
    }

    /* Read first sector */
    error = bread(emp->devvp, sector, EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error || bp == NULL) {
        printf("exfat: [exfat_scan_directory] bread failed: %d\n", error);
        goto out;
    }

    /* Validate buffer data */
    if (bp->b_data == NULL) {
        printf("exfat: [exfat_scan_directory] NULL buffer data\n");
        error = EINVAL;
        goto out;
    }

    /* Set up scan context */
    ctx->bp = bp;
    ctx->entry = (uint8_t *)bp->b_data;

    if (bootverbose)
        printf("exfat: [exfat_scan_directory] scan initialized successfully\n");

    return 0;

out:
    if (bp != NULL) {
        brelse(bp);
    }
    ctx->bp = NULL;
    ctx->entry = NULL;
    return error;
}

/*
 * Read next directory entry set
 */
int
exfat_next_dirent(struct exfat_scan_ctx *ctx, struct exfat_direntry_set *es)
{
    printf("exfat: [exfat_read_next_entry] reading next directory entry at offset %u\n", ctx->offset);

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
        printf("exfat: [exfat_read_next_entry] invalid directory entry type: %d\n", ctx->entry[ctx->offset]);
        return EINVAL;
    }

    if (bootverbose)
        printf("exfat: [exfat_read_next_entry] reading file entry at offset %u\n", ctx->offset);

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
    printf("exfat: [exfat_compare_name] comparing name '%s' (len %zu)\n", name, len);

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
                   "exfat_compare_name", ulen, es->stream.name_length);
        return 0;
    }

    int match = (exfat_name_compare(emp, uname, es->name[0].name, 
                                   MIN(ulen, es->stream.name_length)) == 0);
    if (bootverbose)
        printf("exfat: [exfat_compare_name] name comparison %s\n", match ? "matched" : "failed");

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

    /* Write entry data - write the file entry first */
    struct exfat_entry_file *file = (struct exfat_entry_file *)bp->b_data;
    file->type = node->type;
    file->file_attributes = node->finfo.attributes;
    
    /* Write stream entry after file entry */
    struct exfat_entry_stream *stream = (struct exfat_entry_stream *)(file + 1);
    stream->first_cluster = node->finfo.first_cluster;
    stream->data_length = node->finfo.file_size;
    stream->valid_data_length = node->finfo.valid_size;
    
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
    ep->type = file->type;
    ep->finfo.attributes = file->file_attributes;
    ep->finfo.first_cluster = stream->first_cluster;
    ep->finfo.file_size = stream->data_length;
    ep->finfo.valid_size = stream->valid_data_length;

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
    ep->finfo.first_cluster = 0;
    ep->finfo.file_size = 0;
    ep->finfo.valid_size = 0;
    ep->finfo.attributes = 0;

    /* Write entry to disk */
    error = exfat_write_entry(emp, ep);
    if (error)
        return error;

    return 0;
}

/*
 * Scan directory during mount (doesn't require a vnode)
 */
int
exfat_scan_directory_mount(struct exfat_mount *emp, uint32_t cluster, struct exfat_scan_ctx *ctx)
{
    struct buf *bp = NULL;
    uint32_t sector;
    int error = 0;

    if (bootverbose)
        printf("exfat: [exfat_scan_directory_mount] scanning cluster %u\n", cluster);

    /* Initialize scan context */
    ctx->emp = emp;
    ctx->cluster = cluster;
    ctx->offset = 0;
    ctx->bp = NULL;
    ctx->entry = NULL;

    /* Calculate first sector of cluster */
    sector = emp->boot.cluster_heap_offset + 
            ((cluster - 2) << emp->boot.sectors_per_cluster_shift);

    if (bootverbose)
        printf("exfat: [exfat_scan_directory_mount] reading sector %u\n", sector);

    /* Read first sector */
    error = bread(emp->devvp, sector, EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error || bp == NULL) {
        printf("exfat: [exfat_scan_directory_mount] bread failed: %d\n", error);
        goto out;
    }

    /* Set up scan context */
    ctx->bp = bp;
    ctx->entry = (uint8_t *)bp->b_data;

    return 0;

out:
    if (bp != NULL)
        brelse(bp);
    return error;
}

int
exfat_read_rootdir(struct exfat_mount *emp)
{
    struct exfat_scan_ctx ctx;
    int error;

    if (bootverbose) {
        printf("exfat: [exfat_read_rootdir] reading root directory:\n");
        printf("  cluster_heap_offset: %u\n", emp->boot.cluster_heap_offset);
        printf("  root_cluster: %u\n", emp->root_cluster);
        printf("  sectors_per_cluster_shift: %u\n", emp->boot.sectors_per_cluster_shift);
    }

    error = exfat_scan_directory_mount(emp, emp->root_cluster, &ctx);
    if (error) {
        printf("exfat: [exfat_read_rootdir] failed to initialize bitmap: %d\n", error);
        return error;
    }

    return 0;
} 