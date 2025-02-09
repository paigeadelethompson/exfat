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

static int
exfat_init(struct vfsconf *vfsp)
{
    if (bootverbose)
        printf("exfat: by Paige A. Thompson <paige@paige.bio> (driver loaded)\n");
    return 0;
}

static struct vfsops exfat_vfsops = {
    .vfs_init       = exfat_init,
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
    struct g_consumer *cp;
    int ronly = (mp->mnt_flag & MNT_RDONLY) != 0;

    if (bootverbose)
        printf("exfat: mounting\n");

    if (vfs_filteropt(mp->mnt_optnew, exfat_opts))
        return (EINVAL);

    if (mp->mnt_flag & MNT_UPDATE) {
        if (bootverbose)
            printf("exfat: update mount\n");
        return (EOPNOTSUPP);
    }

    /* Get device path */
    from = vfs_getopts(mp->mnt_optnew, "from", &error);
    if (error) {
        if (bootverbose)
            printf("exfat: failed to get device path\n");
        return error;
    }

    /* Initialize the namei data to lookup the device */
    NDINIT(&nd, LOOKUP, FOLLOW | LOCKLEAF, UIO_SYSSPACE, from);
    error = namei(&nd);
    if (error) {
        if (bootverbose)
            printf("exfat: failed to lookup device: %d\n", error);
        return error;
    }

    devvp = nd.ni_vp;
    NDFREE_PNBUF(&nd);

    /* Check device type */
    if (devvp->v_type != VBLK && devvp->v_type != VREG && devvp->v_type != VCHR) {
        if (bootverbose)
            printf("exfat: not a block device, character device, or regular file (type=%d)\n", devvp->v_type);
        vput(devvp);
        return ENOTBLK;
    }

    /* Get a new vnode for the device */
    devvp = mntfs_allocvp(mp, devvp);
    struct cdev *dev = devvp->v_rdev;

    if (atomic_cmpset_acq_ptr((uintptr_t *)&dev->si_mountpt, 0, (uintptr_t)mp) == 0) {
        if (bootverbose)
            printf("exfat: device already mounted\n");
        mntfs_freevp(devvp);
        return (EBUSY);
    }

    /* Open the device through GEOM */
    if (bootverbose)
        printf("exfat: opening device through GEOM\n");
    g_topology_lock();
    error = g_vfs_open(devvp, &cp, "exfat", ronly ? 0 : 1);
    g_topology_unlock();
    if (error != 0) {
        if (bootverbose)
            printf("exfat: failed to open device through GEOM: %d\n", error);
        atomic_store_rel_ptr((uintptr_t *)&dev->si_mountpt, 0);
        mntfs_freevp(devvp);
        return (error);
    }
    dev_ref(dev);

    /* Allocate mount structure */
    emp = malloc(sizeof(*emp), M_EXFAT, M_WAITOK | M_ZERO);
    if (emp == NULL) {
        if (bootverbose)
            printf("exfat: failed to allocate mount structure\n");
        error = ENOMEM;
        goto fail;
    }

    /* Set up mount structure */
    emp->mp = mp;
    emp->devvp = devvp;
    
    /* Read boot sector */
    if (bootverbose)
        printf("exfat: reading boot sector\n");
    error = bread(devvp, 0, EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error) {
        if (bootverbose)
            printf("exfat: failed to read boot sector: %d\n", error);
        brelse(bp);
        goto fail;
    }

    /* Copy and verify boot sector */
    memcpy(&emp->boot, bp->b_data, sizeof(struct exfat_boot_record));
    if (bootverbose) {
        struct exfat_boot_record *bs = (struct exfat_boot_record *)bp->b_data;
        printf("exfat: boot sector raw values:\n");
        printf("  jump_boot: %02x %02x %02x\n", 
               bs->jump_boot[0], bs->jump_boot[1], bs->jump_boot[2]);
        printf("  fs_name: %.8s\n", bs->fs_name);
        printf("  partition_offset: %u\n", le64toh(bs->partition_offset));
        printf("  volume_length: %u\n", le64toh(bs->volume_length));
        printf("  fat_offset: %u\n", le32toh(bs->fat_offset));
        printf("  fat_length: %u\n", le32toh(bs->fat_length));
        printf("  cluster_heap_offset: %u\n", le32toh(bs->cluster_heap_offset));
        printf("  cluster_count: %u\n", le32toh(bs->cluster_count));
        printf("  root_dir_cluster: %u\n", le32toh(bs->root_dir_cluster));
        printf("  sectors_per_cluster_shift: %u\n", bs->sectors_per_cluster_shift);
    }

    brelse(bp);

    if (memcmp(emp->boot.fs_name, "EXFAT   ", 8) != 0) {
        if (bootverbose)
            printf("exfat: invalid filesystem signature\n");
        error = EINVAL;
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
    emp->root_cluster = emp->boot.root_dir_cluster;

    /* Initialize other mount structure fields */
    if (bootverbose)
        printf("exfat: initializing bitmap\n");
    error = exfat_init_bitmap(emp);
    if (error) {
        if (bootverbose)
            printf("exfat: failed to initialize bitmap: %d\n", error);
        goto fail;
    }

    if (bootverbose)
        printf("exfat: initializing upcase table\n");
    error = exfat_init_upcase(emp);
    if (error) {
        if (bootverbose)
            printf("exfat: failed to initialize upcase table: %d\n", error);
        goto fail;
    }

    /* Set the mount */
    mp->mnt_data = emp;
    vfs_mountedfrom(mp, from);

    if (bootverbose)
        printf("exfat: mount successful\n");
    return 0;

fail:
    if (bootverbose)
        printf("exfat: mount failed\n");
    if (emp) {
        if (emp->devvp) {
            g_vfs_close(cp);
            atomic_store_rel_ptr((uintptr_t *)&dev->si_mountpt, 0);
            mntfs_freevp(devvp);
        }
        free(emp, M_EXFAT);
    }
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
    struct exfat_mount *emp;

    emp = VFSTOEXFAT(mp);

    sbp->f_bsize = emp->bytes_per_cluster;
    sbp->f_iosize = emp->bytes_per_cluster;
    sbp->f_blocks = emp->boot.cluster_count;
    sbp->f_bfree = emp->free_clusters;
    sbp->f_bavail = emp->free_clusters;
    sbp->f_namemax = 255;

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