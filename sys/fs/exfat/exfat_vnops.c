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
#include <sys/namei.h>
#include <sys/kernel.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/malloc.h>
#include <sys/dirent.h>
#include <sys/uio.h>
#include <sys/fcntl.h>

#include "exfat.h"
#include "exfat_node.h"

static vop_lookup_t __unused exfat_lookup;
static vop_read_t       exfat_read;
static vop_write_t      exfat_write;
static vop_getattr_t    exfat_getattr;
static vop_inactive_t   exfat_inactive;
static vop_reclaim_t    exfat_reclaim;

static int exfat_read_cluster(struct exfat_mount *emp, uint32_t cluster, char *buffer);
static int exfat_write_cluster(struct exfat_mount *emp, uint32_t cluster, char *buffer);

static int
exfat_read_cluster(struct exfat_mount *emp, uint32_t cluster, char *buffer)
{
    if (bootverbose)
        printf("exfat: reading cluster %u\n", cluster);

    struct buf *bp;
    uint32_t sector;
    int error;

    sector = emp->boot.cluster_heap_offset +
             ((cluster - 2) << emp->boot.sectors_per_cluster_shift);

    error = bread(emp->devvp, sector, emp->bytes_per_cluster, NOCRED, &bp);
    if (error) {
        if (bootverbose)
            printf("exfat: cluster read failed: %d\n", error);
        brelse(bp);
        return error;
    }

    memcpy(buffer, bp->b_data, emp->bytes_per_cluster);
    brelse(bp);
    return 0;
}

/*
 * Write to a cluster
 */
static int
exfat_write_cluster(struct exfat_mount *emp, uint32_t cluster, char *buffer)
{
    if (bootverbose)
        printf("exfat: writing cluster %u\n", cluster);

    struct buf *bp;
    uint32_t sector;
    int error;

    sector = emp->boot.cluster_heap_offset +
             ((cluster - 2) << emp->boot.sectors_per_cluster_shift);

    error = bread(emp->devvp, sector, emp->bytes_per_cluster, NOCRED, &bp);
    if (error) {
        if (bootverbose)
            printf("exfat: cluster write failed: %d\n", error);
        brelse(bp);
        return error;
    }

    memcpy(bp->b_data, buffer, emp->bytes_per_cluster);
    error = bwrite(bp);
    if (error && bootverbose)
        printf("exfat: cluster write failed: %d\n", error);

    return error;
}

/*
 * Read file data
 */
static int
exfat_read(struct vop_read_args *ap)
{
    if (bootverbose)
        printf("exfat: reading file, offset %jd size %zd\n", 
               (intmax_t)ap->a_uio->uio_offset, ap->a_uio->uio_resid);

    struct vnode *vp = ap->a_vp;
    struct uio *uio = ap->a_uio;
    struct exfat_mount *emp = VTOVFSMP(vp);
    struct exfat_node *ep = VTOE(vp);
    char *cluster_buffer;
    uint32_t cluster, offset;
    size_t cluster_size = emp->bytes_per_cluster;
    int error = 0;

    /* Check if we're trying to read past EOF */
    if (uio->uio_offset >= ep->finfo.file_size) {
        if (bootverbose)
            printf("exfat: read beyond EOF (offset %jd, size %jd)\n",
                   (intmax_t)uio->uio_offset, (intmax_t)ep->finfo.file_size);
        return 0;
    }

    /* Allocate temporary buffer for cluster data */
    cluster_buffer = malloc(cluster_size, M_TEMP, M_WAITOK);

    /* Find starting cluster */
    cluster = ep->finfo.first_cluster;
    offset = uio->uio_offset;

    /* Skip to the cluster containing our offset */
    while (offset >= cluster_size) {
        cluster = exfat_cluster_next(emp, cluster);
        if (cluster == EXFAT_CLUSTER_END) {
            error = EIO;
            goto out;
        }
        offset -= cluster_size;
    }

    /* Read clusters until done */
    while (uio->uio_resid > 0 && uio->uio_offset < ep->finfo.file_size) {
        size_t len;

        /* Read current cluster */
        error = exfat_read_cluster(emp, cluster, cluster_buffer);
        if (error)
            goto out;

        /* Calculate how much to copy */
        len = MIN(cluster_size - offset,
                 MIN(uio->uio_resid,
                     ep->finfo.file_size - uio->uio_offset));

        /* Copy data to user buffer */
        error = uiomove(cluster_buffer + offset, len, uio);
        if (error)
            goto out;

        /* Move to next cluster if needed */
        if (uio->uio_resid > 0 && uio->uio_offset < ep->finfo.file_size) {
            cluster = exfat_cluster_next(emp, cluster);
            if (cluster == EXFAT_CLUSTER_END) {
                error = EIO;
                goto out;
            }
            offset = 0;
        }
    }

    /* Update access time */
    if (error == 0) {
        struct exfat_direntry_set es;
        off_t dir_offset;
        /* Find and update the directory entry */
        error = exfat_find_dirent(vp, ep->cluster, &es, &dir_offset);
        if (error == 0) {
            exfat_update_timestamps(&es.file, EXFAT_UTIME_ACCESS);
            error = exfat_write_direntry(vp, &es, dir_offset);
        }
    }

    if (error == 0) {
        if (bootverbose)
            printf("exfat: read complete, transferred %zd bytes\n",
                   ap->a_uio->uio_resid);
    }

out:
    free(cluster_buffer, M_TEMP);
    return error;
}

/*
 * Get file attributes
 */
static int
exfat_getattr(struct vop_getattr_args *ap)
{
    if (bootverbose)
        printf("exfat: getting attributes for vnode %p\n", ap->a_vp);

    struct vnode *vp = ap->a_vp;
    struct vattr *vap = ap->a_vap;
    struct exfat_node *ep = VTOE(vp);
    struct exfat_mount *emp = VTOVFSMP(vp);

    VATTR_NULL(vap);

    vap->va_type = vp->v_type;
    vap->va_mode = (vp->v_type == VDIR) ? 0755 : 0644;
    vap->va_nlink = 1;
    vap->va_uid = 0;
    vap->va_gid = 0;
    vap->va_fsid = dev2udev(emp->devvp->v_rdev);
    vap->va_fileid = ep->cluster;
    vap->va_size = ep->finfo.file_size;
    vap->va_blocksize = emp->bytes_per_cluster;
    vap->va_atime = ep->access_time;
    vap->va_mtime = ep->modify_time;
    vap->va_ctime = ep->create_time;
    vap->va_gen = 1;
    vap->va_flags = 0;
    vap->va_rdev = 0;
    vap->va_bytes = ep->finfo.file_size;
    vap->va_filerev = 0;
    vap->va_vaflags = 0;

    /* Remove from hash table */
    mtx_lock(&exfat_node_hash_mtx);
    LIST_REMOVE(ep, hash);
    struct exfat_node_hash *hash = &exfat_node_hash[EXFAT_NODE_HASH(ep->cluster)];
    hash->lh_count--;
    mtx_unlock(&exfat_node_hash_mtx);

    return 0;
}

/*
 * Handle inactive vnode
 */
static int
exfat_inactive(struct vop_inactive_args *ap)
{
    if (bootverbose)
        printf("exfat: inactivating vnode %p\n", ap->a_vp);

    struct vnode *vp = ap->a_vp;

    vp->v_data = NULL;
    vgone(vp);

    return 0;
}

/*
 * Reclaim vnode
 */
static int
exfat_reclaim(struct vop_reclaim_args *ap)
{
    if (bootverbose)
        printf("exfat: reclaiming vnode %p\n", ap->a_vp);

    struct vnode *vp = ap->a_vp;
    struct exfat_node *ep = VTOE(vp);

    /* Remove from hash table */
    mtx_lock(&exfat_node_hash_mtx);
    LIST_REMOVE(ep, hash);
    struct exfat_node_hash *hash = &exfat_node_hash[EXFAT_NODE_HASH(ep->cluster)];
    hash->lh_count--;
    mtx_unlock(&exfat_node_hash_mtx);

    /* Free node */
    free(ep, M_EXFAT);
    vp->v_data = NULL;

    return 0;
}

/*
 * Read directory entries
 */
static int
exfat_readdir(struct vop_readdir_args *ap)
{
    if (bootverbose)
        printf("exfat: reading directory entries from vnode %p\n", ap->a_vp);

    /* For now, return empty directory */
    return 0;
}

/*
 * Write file data
 */
static int
exfat_write(struct vop_write_args *ap)
{
    if (bootverbose)
        printf("exfat: writing file, offset %jd size %zd\n",
               (intmax_t)ap->a_uio->uio_offset, ap->a_uio->uio_resid);

    struct vnode *vp = ap->a_vp;
    struct uio *uio = ap->a_uio;
    struct exfat_mount *emp = VTOVFSMP(vp);
    struct exfat_node *ep = VTOE(vp);
    char *cluster_buffer;
    uint32_t cluster, offset;
    size_t cluster_size = emp->bytes_per_cluster;
    int error = 0;

    /* Check if mounted read-only */
    if (vp->v_mount->mnt_flag & MNT_RDONLY) {
        if (bootverbose)
            printf("exfat: write attempted on read-only filesystem\n");
        return EROFS;
    }

    /* Allocate temporary buffer for cluster data */
    cluster_buffer = malloc(cluster_size, M_TEMP, M_WAITOK);

    /* Find starting cluster */
    cluster = ep->finfo.first_cluster;
    offset = uio->uio_offset;

    /* Skip to the cluster containing our offset */
    while (offset >= cluster_size) {
        cluster = exfat_cluster_next(emp, cluster);
        if (cluster == EXFAT_CLUSTER_END) {
            /* Need to allocate new cluster */
            uint32_t new_cluster;
            error = exfat_cluster_alloc(emp, &new_cluster);
            if (error)
                goto out;
            error = exfat_cluster_link(emp, cluster, new_cluster);
            if (error)
                goto out;
            cluster = new_cluster;
        }
        offset -= cluster_size;
    }

    /* Write clusters until done */
    while (uio->uio_resid > 0) {
        size_t len;

        /* Read existing cluster if we're not writing the whole thing */
        if (offset > 0 || uio->uio_resid < cluster_size) {
            error = exfat_read_cluster(emp, cluster, cluster_buffer);
            if (error)
                goto out;
        }

        /* Calculate how much to copy */
        len = MIN(cluster_size - offset, uio->uio_resid);

        /* Copy data from user buffer */
        error = uiomove(cluster_buffer + offset, len, uio);
        if (error)
            goto out;

        /* Write cluster back to disk */
        error = exfat_write_cluster(emp, cluster, cluster_buffer);
        if (error)
            goto out;

        /* Update file size if needed */
        if (uio->uio_offset > ep->finfo.file_size) {
            if (bootverbose)
                printf("exfat: extending file size from %jd to %jd\n",
                       (intmax_t)ep->finfo.file_size, (intmax_t)uio->uio_offset);
            ep->finfo.file_size = uio->uio_offset;
        }

        /* Move to next cluster if needed */
        if (uio->uio_resid > 0) {
            cluster = exfat_cluster_next(emp, cluster);
            if (cluster == EXFAT_CLUSTER_END) {
                /* Need to allocate new cluster */
                uint32_t new_cluster;
                error = exfat_cluster_alloc(emp, &new_cluster);
                if (error)
                    goto out;
                error = exfat_cluster_link(emp, cluster, new_cluster);
                if (error)
                    goto out;
                cluster = new_cluster;
            }
            offset = 0;
        }
    }

    /* Update modify time */
    if (error == 0) {
        struct exfat_direntry_set es;
        off_t dir_offset;
        /* Find and update the directory entry */
        error = exfat_find_dirent(vp, ep->cluster, &es, &dir_offset);
        if (error == 0) {
            exfat_update_timestamps(&es.file, EXFAT_UTIME_MODIFY);
            error = exfat_write_direntry(vp, &es, dir_offset);
        }
    }

out:
    free(cluster_buffer, M_TEMP);
    return error;
}

/*
 * Create a new file
 */
static int
exfat_create(struct vop_create_args *ap)
{
    if (bootverbose)
        printf("exfat: creating file '%s' in directory %p\n", 
               ap->a_cnp->cn_nameptr, ap->a_dvp);

    /* For now, return read-only */
    return EROFS;
}

/*
 * Create a new directory
 */
static int
exfat_mkdir(struct vop_mkdir_args *ap)
{
    if (bootverbose)
        printf("exfat: creating directory '%s' in directory %p\n",
               ap->a_cnp->cn_nameptr, ap->a_dvp);

    /* For now, return read-only */
    return EROFS;
}

/*
 * Remove a file
 */
static int
exfat_remove(struct vop_remove_args *ap)
{
    if (bootverbose)
        printf("exfat: removing file '%s' from directory %p\n",
               ap->a_cnp->cn_nameptr, ap->a_dvp);

    struct vnode *dvp = ap->a_dvp;
    struct vnode *vp = ap->a_vp;
    struct exfat_mount *emp = VTOVFSMP(dvp);
    struct exfat_node *ep = VTOE(vp);
    struct exfat_scan_ctx ctx;
    struct exfat_direntry_set es;
    int error;

    /* Find the directory entry */
    error = exfat_scan_directory(dvp, &ctx);
    if (error) {
        if (bootverbose)
            printf("exfat: failed to scan directory: %d\n", error);
        return error;
    }

    while ((error = exfat_next_dirent(&ctx, &es)) == 0) {
        if (es.stream.first_cluster == ep->cluster) {
            if (bootverbose)
                printf("exfat: found entry to remove at offset %jd\n", 
                       (intmax_t)ctx.offset);
            /* Found it - remove the entry */
            error = exfat_remove_entry(dvp, &es, ctx.offset);
            if (error == 0) {
                /* Free the clusters */
                exfat_cluster_free(emp, ep->finfo.first_cluster);
            }
            exfat_scan_cleanup(&ctx);
            return error;
        }
    }

    if (bootverbose)
        printf("exfat: entry not found\n");
    exfat_scan_cleanup(&ctx);
    return ENOENT;
}

/*
 * Remove a directory
 */
static int
exfat_rmdir(struct vop_rmdir_args *ap)
{
    if (bootverbose)
        printf("exfat: removing directory '%s' from directory %p\n",
               ap->a_cnp->cn_nameptr, ap->a_dvp);

    struct vnode *dvp = ap->a_dvp;
    struct vnode *vp = ap->a_vp;
    struct exfat_mount *emp = VTOVFSMP(dvp);
    struct exfat_node *ep = VTOE(vp);
    struct exfat_scan_ctx ctx;
    struct exfat_direntry_set es;
    int error;

    /* Check if directory is empty */
    error = exfat_scan_directory(vp, &ctx);
    if (error) {
        if (bootverbose)
            printf("exfat: failed to scan directory: %d\n", error);
        return error;
    }

    error = exfat_next_dirent(&ctx, &es);
    if (error != ENOENT) {
        if (bootverbose)
            printf("exfat: directory not empty\n");
        /* Directory not empty */
        exfat_scan_cleanup(&ctx);
        return ENOTEMPTY;
    }
    exfat_scan_cleanup(&ctx);

    /* Find and remove the directory entry */
    error = exfat_scan_directory(dvp, &ctx);
    if (error)
        return error;

    while ((error = exfat_next_dirent(&ctx, &es)) == 0) {
        if (es.stream.first_cluster == ep->cluster) {
            /* Found it - remove the entry */
            error = exfat_remove_entry(dvp, &es, ctx.offset);
            if (error == 0) {
                /* Free the clusters */
                exfat_cluster_free(emp, ep->finfo.first_cluster);
            }
            exfat_scan_cleanup(&ctx);
            return error;
        }
    }

    exfat_scan_cleanup(&ctx);
    return ENOENT;
}

/*
 * Rename a file/directory
 */
static int
exfat_rename(struct vop_rename_args *ap)
{
    if (bootverbose)
        printf("exfat: renaming '%s' to '%s'\n",
               ap->a_fcnp->cn_nameptr, ap->a_tcnp->cn_nameptr);

    struct vnode *fdvp = ap->a_fdvp;    /* from directory vnode */
    struct vnode *fvp = ap->a_fvp;      /* from file/dir vnode */
    struct vnode *tdvp = ap->a_tdvp;    /* to directory vnode */
    struct vnode *tvp = ap->a_tvp;      /* to file/dir vnode (if exists) */
    struct exfat_node *fep = VTOE(fvp);
    struct exfat_scan_ctx ctx;
    struct exfat_direntry_set es, new_es;
    struct timespec ts;
    int error;

    /* Check for cross-device rename */
    if (fdvp->v_mount != tdvp->v_mount) {
        if (bootverbose)
            printf("exfat: cross-device rename not allowed\n");
        return EXDEV;
    }

    /* Get current time */
    vfs_timestamp(&ts);

    /* If target exists, return error */
    if (tvp) {
        if (bootverbose)
            printf("exfat: target already exists\n");
        return EEXIST;
    }

    /* Find the source directory entry */
    error = exfat_scan_directory(fdvp, &ctx);
    if (error) {
        if (bootverbose)
            printf("exfat: failed to scan source directory: %d\n", error);
        return error;
    }

    while ((error = exfat_next_dirent(&ctx, &es)) == 0) {
        if (es.stream.first_cluster == fep->cluster) {
            if (bootverbose)
                printf("exfat: found source entry at offset %jd\n", 
                       (intmax_t)ctx.offset);

            /* Remove the old entry */
            error = exfat_remove_entry(fdvp, &es, ctx.offset);
            if (error) {
                if (bootverbose)
                    printf("exfat: failed to remove old entry: %d\n", error);
                exfat_scan_cleanup(&ctx);
                return error;
            }

            /* Create new entry in target directory */
            error = exfat_create_entry(tdvp, ap->a_tcnp->cn_nameptr,
                                     ap->a_tcnp->cn_namelen,
                                     es.file.file_attributes, &ts, &new_es);
            if (error) {
                if (bootverbose)
                    printf("exfat: failed to create new entry: %d\n", error);
                /* Try to restore the old entry */
                exfat_create_entry(fdvp, ap->a_fcnp->cn_nameptr,
                                 ap->a_fcnp->cn_namelen,
                                 es.file.file_attributes, &ts, &es);
                exfat_scan_cleanup(&ctx);
                return error;
            }

            if (bootverbose)
                printf("exfat: rename successful\n");

            exfat_scan_cleanup(&ctx);
            return 0;
        }
    }

    if (bootverbose)
        printf("exfat: source entry not found\n");
    exfat_scan_cleanup(&ctx);
    return ENOENT;
}

static int
exfat_cachedlookup(struct vop_cachedlookup_args *ap)
{
    if (bootverbose)
        printf("exfat: cached lookup for '%s' in directory %p\n",
               ap->a_cnp->cn_nameptr, ap->a_dvp);

    int error = exfat_lookup_node(ap->a_dvp, ap->a_cnp, ap->a_vpp);
    if (error && bootverbose)
        printf("exfat: lookup failed: %d\n", error);
    return error;
}

static int
exfat_access_wrapper(struct vop_access_args *ap)
{
    if (bootverbose)
        printf("exfat: checking access for vnode %p, mode 0x%x\n",
               ap->a_vp, ap->a_accmode);

    int error = exfat_access(ap->a_vp, ap->a_accmode, ap->a_cred, ap->a_td);
    if (error && bootverbose)
        printf("exfat: access denied: %d\n", error);
    return error;
}

/*
 * Find a directory entry by cluster number
 */
int
exfat_find_dirent(struct vnode *vp, uint32_t cluster,
    struct exfat_direntry_set *es, off_t *offset)
{
    if (bootverbose)
        printf("exfat: searching for cluster %u in directory %p\n", 
               cluster, vp);

    struct exfat_scan_ctx ctx;
    int error;

    /* Initialize directory scan */
    error = exfat_scan_directory(vp, &ctx);
    if (error) {
        if (bootverbose)
            printf("exfat: failed to scan directory: %d\n", error);
        return error;
    }

    /* Scan for matching entry */
    while ((error = exfat_next_dirent(&ctx, es)) == 0) {
        if (es->stream.first_cluster == cluster) {
            if (bootverbose)
                printf("exfat: found entry at offset %jd\n", 
                       (intmax_t)ctx.offset);
            /* Found it - return offset */
            if (offset)
                *offset = ctx.offset;
            exfat_scan_cleanup(&ctx);
            return 0;
        }
    }

    if (bootverbose)
        printf("exfat: entry not found\n");
    exfat_scan_cleanup(&ctx);
    return ENOENT;
}

/*
 * Handle open operation
 */
static int
exfat_open(struct vop_open_args *ap)
{
    if (bootverbose)
        printf("exfat: opening vnode %p, flags 0x%x\n", ap->a_vp, ap->a_mode);

    struct vnode *vp = ap->a_vp;
    int flags = ap->a_mode;

    /* Check if this is a regular file or directory */
    if (vp->v_type != VREG && vp->v_type != VDIR)
        return EINVAL;

    /* Check write access for regular files */
    if (vp->v_type == VREG && (flags & FWRITE)) {
        /* Check if mounted read-only */
        if (vp->v_mount->mnt_flag & MNT_RDONLY)
            return EROFS;
    }

    return 0;
}

/*
 * Handle close operation
 */
static int
exfat_close(struct vop_close_args *ap)
{
    if (bootverbose)
        printf("exfat: closing vnode %p\n", ap->a_vp);

    struct vnode *vp = ap->a_vp;

    /* Flush any dirty data */
    if ((vp->v_type == VREG) && (vp->v_iflag & VI_DOINGINACT)) {
        return VOP_FSYNC(vp, MNT_WAIT, ap->a_td);
    }

    return 0;
}

/*
 * Sync file to disk
 */
static int
exfat_fsync(struct vop_fsync_args *ap)
{
    if (bootverbose)
        printf("exfat: syncing vnode %p\n", ap->a_vp);

    struct vnode *vp = ap->a_vp;
    struct exfat_node *ep = VTOE(vp);
    int error = 0;

    /* Check if mounted read-only */
    if (vp->v_mount->mnt_flag & MNT_RDONLY)
        return 0;

    /* Flush file data */
    if (vp->v_type == VREG) {
        error = vn_fsync_buf(vp, ap->a_waitfor);
        if (error)
            return error;
    }

    /* Update timestamps if needed */
    if (vp->v_iflag & VI_DOINGINACT) {
        struct exfat_direntry_set es;
        off_t offset;

        /* Find and update directory entry */
        error = exfat_find_dirent(vp, ep->cluster, &es, &offset);
        if (error)
            return error;

        /* Update timestamps */
        exfat_update_timestamps(&es.file, EXFAT_UTIME_MODIFY);

        /* Write back directory entry */
        error = exfat_write_direntry(vp, &es, offset);
        if (error)
            return error;

        vp->v_iflag &= ~VI_DOINGINACT;
    }

    return 0;
}

/*
 * Handle buffer I/O
 */
static int
exfat_strategy(struct vop_strategy_args *ap)
{
    if (bootverbose)
        printf("exfat: strategy for vnode %p, block %jd\n", 
               ap->a_vp, (intmax_t)ap->a_bp->b_blkno);

    struct vnode *vp = ap->a_vp;
    struct buf *bp = ap->a_bp;
    struct exfat_mount *emp = VTOVFSMP(vp);
    struct exfat_node *ep = VTOE(vp);
    daddr_t sector;
    uint32_t cluster, offset;

    /* Calculate sector from file offset */
    cluster = ep->finfo.first_cluster;
    offset = bp->b_blkno * DEV_BSIZE;

    /* Skip to correct cluster */
    while (offset >= emp->bytes_per_cluster) {
        cluster = exfat_cluster_next(emp, cluster);
        if (cluster == EXFAT_CLUSTER_END)
            return EIO;
        offset -= emp->bytes_per_cluster;
    }

    /* Calculate sector within cluster */
    sector = emp->boot.cluster_heap_offset +
             ((cluster - 2) << emp->boot.sectors_per_cluster_shift) +
             (offset >> EXFAT_SECTOR_BITS);

    /* Adjust buffer for device */
    bp->b_blkno = sector;

    /* Pass to underlying device */
    return VOP_STRATEGY(emp->devvp, bp);
}

struct vop_vector exfat_vnodeops = {
    .vop_default    = &default_vnodeops,
    .vop_lookup     = VOP_PANIC,       /* Use vfs_cache_lookup */
    .vop_cachedlookup = exfat_cachedlookup,
    .vop_create     = exfat_create,
    .vop_mkdir      = exfat_mkdir,
    .vop_remove     = exfat_remove,
    .vop_rmdir      = exfat_rmdir,
    .vop_read       = exfat_read,
    .vop_write      = exfat_write,
    .vop_getattr    = exfat_getattr,
    .vop_readdir    = exfat_readdir,
    .vop_inactive   = exfat_inactive,
    .vop_reclaim    = exfat_reclaim,
    .vop_rename     = exfat_rename,
    .vop_access     = exfat_access_wrapper,
    .vop_open       = exfat_open,
    .vop_close      = exfat_close,
    .vop_fsync      = exfat_fsync,
    .vop_strategy   = exfat_strategy,
}; 

// Add external function declarations
int exfat_cluster_alloc(struct exfat_mount *emp, uint32_t *cluster);
int exfat_cluster_link(struct exfat_mount *emp, uint32_t current, uint32_t next);
int exfat_cluster_free(struct exfat_mount *emp, uint32_t cluster); 