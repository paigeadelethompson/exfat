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
#include <sys/filedesc.h>
#include <sys/proc.h>   /* Must be before namei.h */
#include <sys/fcntl.h>  /* Must be before namei.h */
#include <sys/uio.h>    /* Must be before namei.h */

/* Define NDF flags if not already defined */
#ifndef NDF_NO_FREE_PNBUF
#define NDF_NO_FREE_PNBUF   0x00000020
#endif
#ifndef NDF_ONLY_PNBUF
#define NDF_ONLY_PNBUF      (~NDF_NO_FREE_PNBUF)
#endif

#include <sys/namei.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/eventhandler.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/malloc.h>
#include <sys/conf.h>    /* For struct cdev */
#include <geom/geom.h>
#include <geom/geom_vfs.h>  /* For gb_bufops */
#include <sys/mutex.h>

#include "exfat.h"
#include "exfat_fat.h"
#include "exfat_node.h"

static vfs_mount_t     exfat_mount;
static vfs_unmount_t   exfat_unmount;
static vfs_root_t      exfat_root;
static vfs_statfs_t    exfat_statfs;
static vfs_sync_t      exfat_sync;

static struct vfsops exfat_vfsops = {
    .vfs_mount      = exfat_mount,
    .vfs_unmount    = exfat_unmount,
    .vfs_root       = exfat_root,
    .vfs_statfs     = exfat_statfs,
    .vfs_sync       = exfat_sync,
};

/* After the mount structure definition, add: */
MALLOC_DEFINE(M_EXFAT, "exfat", "EXFAT filesystem");

/* Mount options */
static const char *exfat_opts[] = {
    "from",
    NULL
};

/* Add these helper functions before exfat_mount(): */

/*
 * Read the FAT entry for a given cluster
 */
static int
exfat_read_fat_entry(struct exfat_mount *emp, uint32_t cluster, uint32_t *next)
{
    struct buf *bp;
    uint32_t fat_offset;
    uint32_t sec_offset;
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

    *next = le32toh(*(uint32_t *)(bp->b_data + (fat_offset & (EXFAT_SECTOR_SIZE - 1))));
    brelse(bp);
    
    return 0;
}

/*
 * Find the bitmap directory entry
 */
static int
exfat_find_bitmap(struct exfat_mount *emp, struct exfat_entry_bitmap *bitmap)
{
    struct buf *bp;
    uint32_t cluster, offset;
    int error;

    /* Start at root directory */
    cluster = emp->boot.root_dir_cluster;

    /* Read first cluster of root directory */
    error = bread(emp->devvp,
                 emp->boot.cluster_heap_offset + 
                 ((cluster - 2) << emp->boot.sectors_per_cluster_shift),
                 emp->bytes_per_cluster, NOCRED, &bp);
    if (error) {
        brelse(bp);
        return error;
    }

    /* Search for bitmap entry */
    for (offset = 0; offset < emp->bytes_per_cluster; offset += sizeof(struct exfat_entry_bitmap)) {
        struct exfat_entry_bitmap *entry = (struct exfat_entry_bitmap *)(bp->b_data + offset);
        
        if (entry->type == EXFAT_ENTRY_BITMAP) {
            memcpy(bitmap, entry, sizeof(*bitmap));
            brelse(bp);
            return 0;
        }
    }

    brelse(bp);
    return ENOENT;
}

/* Track number of mounted filesystems */
static volatile u_int exfat_mount_count = 0;
static struct mtx exfat_mount_lock;
MTX_SYSINIT(exfat_mount_lock, &exfat_mount_lock, "exfat_mount", MTX_DEF);

/* Update the mount implementation to read the boot sector: */
static int
exfat_mount(struct mount *mp)
{
    struct vnode *devvp;
    struct exfat_mount *emp = NULL;
    struct nameidata nd;
    char *from;
    struct buf *bp = NULL;
    int error;

    if (bootverbose)
        printf("exfat: mounting\n");

    if (vfs_filteropt(mp->mnt_optnew, exfat_opts))
        return (EINVAL);

    if (mp->mnt_flag & MNT_UPDATE) {
        if (bootverbose)
            printf("exfat: update mount\n");
        return (EOPNOTSUPP);  /* Not yet supported */
    }

    /* Allocate mount structure */
    emp = malloc(sizeof(struct exfat_mount), M_EXFAT, M_WAITOK | M_ZERO);
    if (emp == NULL)
        return (ENOMEM);

    /* Get device path */
    from = vfs_getopts(mp->mnt_optnew, "from", &error);
    if (error) {
        if (bootverbose)
            printf("exfat: failed to get device path\n");
        free(emp, M_EXFAT);
        return error;
    }

    /* Initialize the namei data to lookup the device */
    NDINIT(&nd, LOOKUP, FOLLOW | LOCKLEAF, UIO_SYSSPACE, from);
    error = namei(&nd);
    if (error) {
        if (bootverbose)
            printf("exfat: failed to lookup device: %d\n", error);
        free(emp, M_EXFAT);
        return error;
    }
    devvp = nd.ni_vp;
    NDFREE_PNBUF(&nd);

    /* Check if it's a block device */
    if (devvp->v_type != VBLK && devvp->v_type != VREG && devvp->v_type != VCHR) {
        if (bootverbose)
            printf("exfat: not a block device, character device, or regular file (type=%d)\n", devvp->v_type);
        vput(devvp);
        free(emp, M_EXFAT);
        return ENOTBLK;
    }

    /* Check if device is already mounted */
    if ((devvp->v_type == VBLK && devvp->v_rdev->si_mountpt != NULL) ||
        (devvp->v_type == VREG && devvp->v_mount != mp)) {
        if (bootverbose)
            printf("exfat: device already mounted\n");
        vput(devvp);
        free(emp, M_EXFAT);
        return EBUSY;
    }

    /* Open the device */
    error = VOP_OPEN(devvp, FREAD | (mp->mnt_flag & MNT_RDONLY ? 0 : FWRITE),
                     FSCRED, curthread, NULL);
    if (error) {
        if (bootverbose)
            printf("exfat: failed to open device: %d\n", error);
        vput(devvp);
        free(emp, M_EXFAT);
        return error;
    }

    /* Set up buffer I/O strategy */
    if (devvp->v_type == VCHR) {
        struct cdev *dev = devvp->v_rdev;
        struct g_consumer *cp;
        struct g_provider *pp;

        /* Get the GEOM provider */
        pp = g_dev_getprovider(dev);
        if (pp == NULL) {
            if (bootverbose)
                printf("exfat: failed to get provider for device\n");
            return ENXIO;
        }

        dev_ref(dev);
        devvp->v_bufobj.bo_ops = pp->geom->vfs_bp_ops;
        devvp->v_bufobj.bo_private = pp;
        devvp->v_bufobj.bo_bsize = EXFAT_SECTOR_SIZE;
    }

    /* Set the mounted device in mount structure */
    vfs_mountedfrom(mp, from);

    VOP_UNLOCK(devvp);
    emp->devvp = devvp;
    emp->mp = mp;
    mp->mnt_data = emp;

    /* Set device block size */
    vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
    error = vinvalbuf(devvp, V_SAVE, 0, 0);
    if (error) {
        if (bootverbose)
            printf("exfat: vinvalbuf failed: %d\n", error);
        VOP_UNLOCK(devvp);
        return error;
    }
    devvp->v_bufobj.bo_bsize = EXFAT_SECTOR_SIZE;
    VOP_UNLOCK(devvp);

    /* Initialize error tracking */
    emp->error_count = 0;
    vfs_timestamp(&emp->last_error_time);
    emp->mount_flags = 0;

    /* Read boot sector */
    error = bread(devvp, 0, EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error) {
        if (bootverbose)
            printf("exfat: failed to read boot sector: %d\n", error);
        brelse(bp);
        goto fail;
    }

    /* Check if filesystem needs recovery */
    if (emp->boot.volume_state == EXFAT_STATE_DIRTY) {
        if (bootverbose)
            printf("exfat: filesystem was not cleanly unmounted\n");
        emp->mount_flags |= EXFAT_MNT_FSCK;
    }

    /* Copy boot sector */
    memcpy(&emp->boot, bp->b_data, sizeof(struct exfat_boot_record));
    brelse(bp);

    /* Verify boot sector */
    if (memcmp(emp->boot.fs_name, "EXFAT   ", 8) != 0) {
        if (bootverbose)
            printf("exfat: invalid filesystem signature\n");
        goto fail;
    }

    if (bootverbose) {
        printf("exfat: filesystem parameters:\n");
        printf("  sectors per cluster: %d\n", 1 << emp->boot.sectors_per_cluster_shift);
        printf("  cluster count: %d\n", emp->boot.cluster_count);
        printf("  FAT offset: %d\n", emp->boot.fat_offset);
        printf("  FAT length: %d\n", emp->boot.fat_length);
        printf("  cluster heap offset: %d\n", emp->boot.cluster_heap_offset);
        printf("  root dir cluster: %d\n", emp->boot.root_dir_cluster);
    }

    /* Calculate mount parameters */
    emp->bytes_per_cluster = EXFAT_SECTOR_SIZE << emp->boot.sectors_per_cluster_shift;
    emp->clusters_count = emp->boot.cluster_count;

    /* Find and read bitmap directory entry */
    struct exfat_entry_bitmap bitmap;
    error = exfat_find_bitmap(emp, &bitmap);
    if (error) {
        if (bootverbose)
            printf("exfat: failed to find bitmap: %d\n", error);
        goto fail;
    }

    if (bootverbose)
        printf("exfat: mount successful\n");

    /* Calculate bitmap size in sectors */
    emp->bitmap_sectors = howmany(emp->clusters_count, EXFAT_SECTOR_SIZE * 8);

    /* Calculate important filesystem parameters */
    emp->clusters_per_fat = emp->boot.fat_length >> emp->boot.sectors_per_cluster_shift;
    emp->root_cluster = emp->boot.root_dir_cluster;

    /* Initialize allocation bitmap */
    error = exfat_init_bitmap(emp);
    if (error)
        goto fail;

    /* Scan first few clusters for bad sectors */
    if (bootverbose)
        printf("exfat: scanning first 1000 clusters for bad sectors\n");
    error = exfat_scan_clusters(emp, 2, MIN(1000, emp->boot.cluster_count));
    if (error && bootverbose)
        printf("exfat: bad sectors found during initial scan\n");

    /* Initialize upcase table */
    error = exfat_init_upcase(emp);
    if (error) {
        free(emp, M_EXFAT);
        return error;
    }

    /* Read volume label */
    error = exfat_read_volume_label(emp);
    if (error) {
        exfat_cleanup_upcase(emp);
        free(emp, M_EXFAT);
        return error;
    }

    /* Initialize cluster counts */
    error = exfat_update_percent_in_use(emp);
    if (error) {
        exfat_cleanup_upcase(emp);
        free(emp, M_EXFAT);
        return error;
    }

    /* Mark volume as dirty */
    error = exfat_set_volume_dirty(emp, 1);
    if (error) {
        free(emp, M_EXFAT);
        return error;
    }

    mtx_lock(&exfat_mount_lock);
    exfat_mount_count++;
    mtx_unlock(&exfat_mount_lock);

    return 0;

fail:
    exfat_cleanup_upcase(emp);
    free(emp, M_EXFAT);
    mp->mnt_data = NULL;

    if (bootverbose)
        printf("exfat: mount failed\n");
    return error;
}

/*
 * Unmount the filesystem
 */
static int
exfat_unmount(struct mount *mp, int mntflags)
{
    if (bootverbose)
        printf("exfat: unmounting %s\n", mp->mnt_stat.f_mntfromname);

    struct exfat_mount *emp = VFSTOEXFAT(mp);
    int error;

    /* Check if filesystem needs repair */
    if (emp->mount_flags & (EXFAT_MNT_FSCK | EXFAT_MNT_ERRORS)) {
        if (bootverbose)
            printf("exfat: filesystem needs repair (%u errors)\n", 
                   emp->error_count);
    }

    /* Mark volume clean unless errors occurred */
    if ((emp->mount_flags & EXFAT_MNT_ERRORS) == 0) {
        error = exfat_set_volume_dirty(emp, 0);
        if (error && bootverbose)
            printf("exfat: failed to mark volume clean: %d\n", error);
    }

    if (bootverbose)
        printf("exfat: flushing vnodes\n");
    error = vflush(mp, 0, 0, curthread);
    if (error) {
        if (bootverbose)
            printf("exfat: vflush failed: %d\n", error);
        return error;
    }

    exfat_cleanup_upcase(emp);
    free(emp, M_EXFAT);
    mp->mnt_data = NULL;

    mtx_lock(&exfat_mount_lock);
    exfat_mount_count--;
    mtx_unlock(&exfat_mount_lock);

    if (bootverbose)
        printf("exfat: unmount successful\n");
    return error;
}

/*
 * Get root vnode
 */
static int
exfat_root(struct mount *mp, int flags, struct vnode **vpp)
{
    if (bootverbose)
        printf("exfat: getting root vnode\n");
    struct exfat_mount *emp;
    struct vnode *vp;
    int error;

    emp = VFSTOEXFAT(mp);

    error = exfat_get_node(mp, emp->root_cluster, VDIR, &vp);
    if (error) {
        if (bootverbose)
            printf("exfat: failed to get root node: %d\n", error);
        return error;
    }

    *vpp = vp;
    if (bootverbose)
        printf("exfat: root vnode obtained\n");
    return 0;
}

/*
 * Get filesystem statistics
 */
static int
exfat_statfs(struct mount *mp, struct statfs *sbp)
{
    if (bootverbose)
        printf("exfat: getting filesystem statistics\n");
    struct exfat_mount *emp;
    uint32_t free_clusters = 0;
    uint32_t cluster;

    emp = VFSTOEXFAT(mp);

    /* Count free clusters */
    for (cluster = 2; cluster < emp->boot.cluster_count + 2; cluster++) {
        uint32_t next;
        if (exfat_read_fat_entry(emp, cluster, &next) == 0 && 
            next == EXFAT_CLUSTER_FREE)
            free_clusters++;
    }

    if (bootverbose)
        printf("exfat: found %u free clusters\n", free_clusters);

    sbp->f_bsize = emp->bytes_per_cluster;
    sbp->f_iosize = emp->bytes_per_cluster;
    sbp->f_blocks = emp->boot.cluster_count;
    sbp->f_bfree = free_clusters;
    sbp->f_bavail = free_clusters;
    /* Set maximum filename length */
    sbp->f_namemax = 255;

    /* Copy volume label */
    strlcpy(sbp->f_mntfromname, emp->volume_label, sizeof(sbp->f_mntfromname));

    return 0;
}

/*
 * Sync the filesystem
 */
static int
exfat_sync(struct mount *mp, int waitfor)
{
    if (bootverbose)
        printf("exfat: syncing filesystem\n");
    struct exfat_mount *emp = VFSTOEXFAT(mp);
    struct vnode *vp;
    int error, allerror = 0;

    /* Don't sync if mounted read-only */
    if (mp->mnt_flag & MNT_RDONLY)
        return 0;

    /* First flush all vnodes */
    if (waitfor == MNT_WAIT) {
        if (bootverbose)
            printf("exfat: flushing all vnodes\n");
        struct vnode *mvp;
        vp = TAILQ_FIRST(&mp->mnt_nvnodelist);
        while (vp != NULL) {
            VI_LOCK(vp);
            mvp = TAILQ_NEXT(vp, v_nmntvnodes);
            VI_UNLOCK(vp);
            error = VOP_FSYNC(vp, MNT_WAIT, curthread);
            if (error) {
                if (bootverbose)
                    printf("exfat: vnode sync failed: %d\n", error);
                allerror = error;
            }
            vrele(vp);
            vp = mvp;
        }
    }

    /* Then sync memory-mapped files */
    if (bootverbose)
        printf("exfat: syncing memory-mapped files\n");
    error = vfs_stdsync(mp, MNT_WAIT);
    if (error) {
        if (bootverbose)
            printf("exfat: memory-mapped sync failed: %d\n", error);
        allerror = error;
    }

    /* Finally update percent-in-use after all changes are flushed */
    if (bootverbose)
        printf("exfat: updating percent-in-use\n");
    error = exfat_update_percent_in_use(emp);
    if (error) {
        if (bootverbose)
            printf("exfat: failed to update percent-in-use: %d\n", error);
        allerror = error;
    }

    if (bootverbose)
        printf("exfat: sync %s\n", allerror ? "failed" : "successful");
    return allerror;
}

VFS_SET(exfat_vfsops, exfat, 0);
MODULE_VERSION(exfat, 1); 