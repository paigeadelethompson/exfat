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
    struct buf *bp;
    uint32_t sector;
    int error;

    sector = emp->boot.cluster_heap_offset +
             ((cluster - 2) << emp->boot.sectors_per_cluster_shift);

    error = bread(emp->devvp, sector, emp->bytes_per_cluster, NOCRED, &bp);
    if (error) {
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
    struct buf *bp;
    uint32_t sector;
    int error;

    sector = emp->boot.cluster_heap_offset +
             ((cluster - 2) << emp->boot.sectors_per_cluster_shift);

    error = bread(emp->devvp, sector, emp->bytes_per_cluster, NOCRED, &bp);
    if (error) {
        brelse(bp);
        return error;
    }

    memcpy(bp->b_data, buffer, emp->bytes_per_cluster);
    error = bwrite(bp);

    return error;
}

/*
 * Read file data
 */
static int
exfat_read(struct vop_read_args *ap)
{
    struct vnode *vp = ap->a_vp;
    struct uio *uio = ap->a_uio;
    struct exfat_mount *emp = VTOVFSMP(vp);
    struct exfat_node *ep = VTOE(vp);
    char *cluster_buffer;
    uint32_t cluster, offset;
    size_t cluster_size = emp->bytes_per_cluster;
    int error = 0;

    /* Check if we're trying to read past EOF */
    if (uio->uio_offset >= ep->finfo.file_size)
        return 0;

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
    struct vnode *vp = ap->a_vp;
    struct uio *uio = ap->a_uio;
    int *eofflag = ap->a_eofflag;
    struct exfat_scan_ctx ctx;
    struct exfat_direntry_set es;
    struct dirent dirent;
    int error;

    /* Check if this is a directory */
    if (vp->v_type != VDIR)
        return ENOTDIR;

    /* Initialize directory scanning */
    error = exfat_scan_directory(vp, &ctx);
    if (error)
        return error;

    /* Skip to the requested offset */
    while (uio->uio_offset > 0 && (error = exfat_next_dirent(&ctx, &es)) == 0) {
        uio->uio_offset--;
    }
    if (error && error != ENOENT) {
        exfat_scan_cleanup(&ctx);
        return error;
    }

    /* Read directory entries */
    while (uio->uio_resid >= sizeof(struct dirent)) {
        error = exfat_next_dirent(&ctx, &es);
        if (error == ENOENT) {
            if (eofflag)
                *eofflag = 1;
            break;
        }
        if (error) {
            exfat_scan_cleanup(&ctx);
            return error;
        }

        /* Fill in dirent structure */
        memset(&dirent, 0, sizeof(dirent));
        dirent.d_fileno = es.stream.first_cluster;
        dirent.d_type = (es.file.file_attributes & EXFAT_ATTR_DIRECTORY) ? DT_DIR : DT_REG;
        dirent.d_namlen = es.stream.name_length;
        
        /* Convert UTF-16 name to ASCII (simplified) */
        size_t i;
        for (i = 0; i < es.stream.name_length && i < sizeof(dirent.d_name) - 1; i++) {
            uint16_t uc = le16toh(es.name[i / 15].name[i % 15]);
            dirent.d_name[i] = (uc < 0x80) ? uc : '?';
        }
        dirent.d_name[i] = '\0';
        dirent.d_reclen = sizeof(struct dirent);

        /* Copy to user buffer */
        error = uiomove(&dirent, sizeof(dirent), uio);
        if (error) {
            exfat_scan_cleanup(&ctx);
            return error;
        }

        uio->uio_offset++;
    }

    exfat_scan_cleanup(&ctx);
    return 0;
}

/*
 * Write file data
 */
static int
exfat_write(struct vop_write_args *ap)
{
    struct vnode *vp = ap->a_vp;
    struct uio *uio = ap->a_uio;
    struct exfat_mount *emp = VTOVFSMP(vp);
    struct exfat_node *ep = VTOE(vp);
    char *cluster_buffer;
    uint32_t cluster, offset;
    size_t cluster_size = emp->bytes_per_cluster;
    int error = 0;

    if (uio->uio_offset < 0)
        return EINVAL;

    /* Allocate temporary buffer for cluster data */
    cluster_buffer = malloc(cluster_size, M_TEMP, M_WAITOK);

    /* Find or allocate starting cluster */
    cluster = ep->finfo.first_cluster;
    offset = uio->uio_offset;

    /* Allocate first cluster if needed */
    if (cluster == 0 && uio->uio_resid > 0) {
        error = exfat_cluster_alloc(emp, &cluster);
        if (error)
            goto out;
        ep->finfo.first_cluster = cluster;
    }

    /* Skip to the cluster containing our offset */
    while (offset >= cluster_size && cluster != EXFAT_CLUSTER_END) {
        uint32_t next = exfat_cluster_next(emp, cluster);
        if (next == EXFAT_CLUSTER_END) {
            /* Need to extend the chain */
            error = exfat_cluster_extend(emp, &cluster);
            if (error)
                goto out;
        } else {
            cluster = next;
        }
        offset -= cluster_size;
    }

    /* Write clusters */
    while (uio->uio_resid > 0) {
        size_t len;

        /* Extend cluster chain if needed */
        if (cluster == EXFAT_CLUSTER_END) {
            error = exfat_cluster_extend(emp, &cluster);
            if (error)
                goto out;
        }

        /* If not writing a full cluster, read existing data first */
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

        /* Write cluster back */
        error = exfat_write_cluster(emp, cluster, cluster_buffer);
        if (error)
            goto out;

        /* Update file size if needed */
        if (uio->uio_offset > ep->finfo.file_size)
            ep->finfo.file_size = uio->uio_offset;

        /* Move to next cluster */
        offset = 0;
        cluster = exfat_cluster_next(emp, cluster);
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
    struct vnode *dvp = ap->a_dvp;
    struct vnode **vpp = ap->a_vpp;
    struct componentname *cnp = ap->a_cnp;
    struct vattr *vap = ap->a_vap;
    struct exfat_mount *emp = VTOVFSMP(dvp);
    struct exfat_direntry_set es;
    struct timespec ts;
    uint32_t cluster;
    int error;

    /* Get current time */
    vfs_timestamp(&ts);

    /* Create directory entry */
    error = exfat_create_entry(dvp, cnp->cn_nameptr, cnp->cn_namelen,
                             EXFAT_ATTR_ARCHIVE, &ts, &es);
    if (error)
        return error;

    /* Allocate first cluster if needed */
    if (vap->va_size > 0) {
        error = exfat_cluster_alloc(emp, &cluster);
        if (error)
            return error;
        es.stream.first_cluster = cluster;
    }

    /* Get the vnode */
    error = exfat_get_node(dvp->v_mount, es.stream.first_cluster, VREG, vpp);
    if (error)
        return error;

    return 0;
}

/*
 * Create a new directory
 */
static int
exfat_mkdir(struct vop_mkdir_args *ap)
{
    struct vnode *dvp = ap->a_dvp;
    struct vnode **vpp = ap->a_vpp;
    struct componentname *cnp = ap->a_cnp;
    struct exfat_mount *emp = VTOVFSMP(dvp);
    struct exfat_direntry_set es;
    struct timespec ts;
    uint32_t cluster;
    int error;

    /* Get current time */
    vfs_timestamp(&ts);

    /* Allocate first cluster for directory */
    error = exfat_cluster_alloc(emp, &cluster);
    if (error)
        return error;

    /* Create directory entry */
    error = exfat_create_entry(dvp, cnp->cn_nameptr, cnp->cn_namelen,
                             EXFAT_ATTR_DIRECTORY, &ts, &es);
    if (error) {
        exfat_cluster_free(emp, cluster);
        return error;
    }

    es.stream.first_cluster = cluster;

    /* Get the vnode */
    error = exfat_get_node(dvp->v_mount, cluster, VDIR, vpp);
    if (error) {
        exfat_cluster_free(emp, cluster);
        return error;
    }

    return 0;
}

/*
 * Remove a file
 */
static int
exfat_remove(struct vop_remove_args *ap)
{
    struct vnode *dvp = ap->a_dvp;
    struct vnode *vp = ap->a_vp;
    struct exfat_mount *emp = VTOVFSMP(dvp);
    struct exfat_node *ep = VTOE(vp);
    struct exfat_scan_ctx ctx;
    struct exfat_direntry_set es;
    int error;

    /* Find the directory entry */
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
 * Remove a directory
 */
static int
exfat_rmdir(struct vop_rmdir_args *ap)
{
    struct vnode *dvp = ap->a_dvp;
    struct vnode *vp = ap->a_vp;
    struct exfat_mount *emp = VTOVFSMP(dvp);
    struct exfat_node *ep = VTOE(vp);
    struct exfat_scan_ctx ctx;
    struct exfat_direntry_set es;
    int error;

    /* Check if directory is empty */
    error = exfat_scan_directory(vp, &ctx);
    if (error)
        return error;

    error = exfat_next_dirent(&ctx, &es);
    if (error != ENOENT) {
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
    struct vnode *fdvp = ap->a_fdvp;    /* from directory vnode */
    struct vnode *fvp = ap->a_fvp;      /* from file/dir vnode */
    struct componentname *fcnp = ap->a_fcnp;
    struct vnode *tdvp = ap->a_tdvp;    /* to directory vnode */
    struct vnode *tvp = ap->a_tvp;      /* to file/dir vnode (if exists) */
    struct componentname *tcnp = ap->a_tcnp;
    struct exfat_node *fep = VTOE(fvp);
    struct exfat_scan_ctx ctx;
    struct exfat_direntry_set es, new_es;
    struct timespec ts;
    int error;

    /* Check for cross-device rename */
    if (fdvp->v_mount != tdvp->v_mount)
        return EXDEV;

    /* Get current time */
    vfs_timestamp(&ts);

    /* If target exists, return error */
    if (tvp)
        return EEXIST;

    /* Find the source directory entry */
    error = exfat_scan_directory(fdvp, &ctx);
    if (error)
        return error;

    while ((error = exfat_next_dirent(&ctx, &es)) == 0) {
        if (es.stream.first_cluster == fep->cluster) {
            /* Found source entry - save its cluster chain info */
            uint32_t first_cluster = es.stream.first_cluster;
            uint64_t file_size = es.stream.data_length;

            /* Remove the old entry */
            error = exfat_remove_entry(fdvp, &es, ctx.offset);
            if (error) {
                exfat_scan_cleanup(&ctx);
                return error;
            }

            /* Create new entry in target directory */
            error = exfat_create_entry(tdvp, tcnp->cn_nameptr, tcnp->cn_namelen,
                                     es.file.file_attributes, &ts, &new_es);
            if (error) {
                /* Try to restore the old entry */
                exfat_create_entry(fdvp, fcnp->cn_nameptr, fcnp->cn_namelen,
                                 es.file.file_attributes, &ts, &es);
                exfat_scan_cleanup(&ctx);
                return error;
            }

            /* Set cluster chain info in new entry */
            new_es.stream.first_cluster = first_cluster;
            new_es.stream.data_length = file_size;
            new_es.stream.valid_data_length = file_size;

            /* Update timestamps */
            unix_time_to_exfat(&ts, &new_es.file.last_modified_timestamp, &new_es.file.last_modified_timestamp);
            new_es.file.last_access_timestamp = new_es.file.last_modified_timestamp;

            exfat_scan_cleanup(&ctx);
            return 0;
        }
    }

    exfat_scan_cleanup(&ctx);
    return ENOENT;
}

static int
exfat_cachedlookup(struct vop_cachedlookup_args *ap)
{
    return exfat_lookup_node(ap->a_dvp, ap->a_cnp, ap->a_vpp);
}

static int
exfat_access_wrapper(struct vop_access_args *ap)
{
    return exfat_access(ap->a_vp, ap->a_accmode, ap->a_cred, ap->a_td);
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
}; 