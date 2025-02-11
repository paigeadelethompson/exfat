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
#include <sys/rwlock.h>  /* For rw_wlock/rw_wunlock */
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
        printf("exfat: [exfat_init] initializing filesystem\n");
    return 0;
}

static struct vfsops exfat_vfsops = {
//    .vfs_init       = exfat_init,
    .vfs_mount      = exfat_mount,
//    .vfs_unmount    = exfat_unmount,
//    .vfs_root       = exfat_root,
//    .vfs_statfs     = exfat_statfs,
//    .vfs_sync       = exfat_sync,
};

/* After the mount structure definition, add: */
MALLOC_DEFINE(M_EXFAT, "exfat", "EXFAT filesystem");

/* Mount options */
static const char *exfat_opts[] = {
    "from",
    NULL
};

/* Update the mount implementation to read the boot sector: */
static int
exfat_mount(struct mount *mp)
{
    struct vnode *devvp;
    struct exfat_mount *emp = NULL;
    struct nameidata nd;
    char *from;
    struct buf *bp = NULL;
    struct g_consumer *cp;
    struct cdev *dev;
    int error;
    int ronly = (mp->mnt_flag & MNT_RDONLY) != 0;

    if (bootverbose)
        printf("exfat: [exfat_mount] mounting volume '%s'\n", mp->mnt_stat.f_mntfromname);

    if (vfs_filteropt(mp->mnt_optnew, exfat_opts))
        return (EINVAL);

    if (mp->mnt_flag & MNT_UPDATE) {
        if (bootverbose)
            printf("exfat: [exfat_mount] update mount\n");
        return (EOPNOTSUPP);
    }

    from = vfs_getopts(mp->mnt_optnew, "from", &error);
    if (error) {
        if (bootverbose)
            printf("exfat: [exfat_mount] failed to get device path\n");
        return error;
    }

    NDINIT(&nd, LOOKUP, FOLLOW | LOCKLEAF, UIO_SYSSPACE, from);
    error = namei(&nd);
    if (error) {
        if (bootverbose)
            printf("exfat: [exfat_mount] failed to lookup device: %d\n", error);
        return error;
    }

    devvp = nd.ni_vp;
    NDFREE_PNBUF(&nd);

    if (devvp->v_type != VBLK && devvp->v_type != VREG && devvp->v_type != VCHR) {
        if (bootverbose)
            printf("exfat: [exfat_mount] not a block device, character device, or regular file (type=%d)\n", devvp->v_type);
        vput(devvp);
        return ENOTBLK;
    }

    devvp = mntfs_allocvp(mp, devvp);
    dev = devvp->v_rdev;

    if (atomic_cmpset_acq_ptr((uintptr_t *)&dev->si_mountpt, 0, (uintptr_t)mp) == 0) {
        if (bootverbose)
            printf("exfat: [exfat_mount] device already mounted\n");
        mntfs_freevp(devvp);
        return (EBUSY);
    }

    if (bootverbose)
        printf("exfat: [exfat_mount] opening device through GEOM\n");
    g_topology_lock();
    error = g_vfs_open(devvp, &cp, "exfat", ronly ? 0 : 1);
    g_topology_unlock();
    if (error != 0) {
        if (bootverbose)
            printf("exfat: [exfat_mount] failed to open device through GEOM: %d\n", error);
        atomic_store_rel_ptr((uintptr_t *)&dev->si_mountpt, 0);
        mntfs_freevp(devvp);
        return (error);
    }
    dev_ref(dev);

    emp = malloc(sizeof(*emp), M_EXFAT, M_WAITOK | M_ZERO);
    if (emp == NULL) {
        if (bootverbose)
            printf("exfat: [exfat_mount] failed to allocate mount structure\n");
        error = ENOMEM;
        goto error_exit;
    }

    emp->mp = mp;
    emp->devvp = devvp;

    if (bootverbose)
        printf("exfat: [exfat_mount] reading boot sector\n");
    error = bread(devvp, 0, EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error) {
        if (bootverbose)
            printf("exfat: [exfat_mount] failed to read boot sector: %d\n", error);
        goto error_exit;
    }

    struct exfat_boot_record *bs = (struct exfat_boot_record *)bp->b_data;
    memcpy(emp->boot.jump_boot, bs->jump_boot, sizeof(bs->jump_boot));
    memcpy(emp->boot.fs_name, bs->fs_name, sizeof(bs->fs_name));
    emp->boot.volume_length = le64toh(bs->volume_length);
    emp->boot.fat_offset = le32toh(bs->fat_offset);
    emp->boot.fat_length = le32toh(bs->fat_length);
    emp->boot.cluster_heap_offset = le32toh(bs->cluster_heap_offset);
    emp->boot.cluster_count = le32toh(bs->cluster_count);
    emp->boot.root_dir_cluster = le32toh(bs->root_dir_cluster);
    emp->boot.volume_serial = le32toh(bs->volume_serial);
    emp->boot.fs_revision = le16toh(bs->fs_revision);
    emp->boot.volume_flags = le16toh(bs->volume_flags);
    emp->boot.bytes_per_sector_shift = bs->bytes_per_sector_shift;
    emp->boot.sectors_per_cluster_shift = bs->sectors_per_cluster_shift;
    emp->boot.number_of_fats = bs->number_of_fats;

    if (memcmp(emp->boot.fs_name, "EXFAT   ", 8) != 0) {
        if (bootverbose)
            printf("exfat: [exfat_mount] invalid filesystem signature\n");
        error = EINVAL;
        goto error_exit;
    }

    emp->bytes_per_cluster = EXFAT_SECTOR_SIZE << emp->boot.sectors_per_cluster_shift;
    emp->clusters_count = emp->boot.cluster_count;
    emp->root_cluster = emp->boot.root_dir_cluster;

    if (bootverbose)
        printf("exfat: [exfat_mount] initializing bitmap\n");
    error = exfat_init_bitmap(emp);
    if (error) {
        if (bootverbose)
            printf("exfat: [exfat_mount] failed to initialize bitmap: %d\n", error);
        goto error_exit;
    }

    if (bootverbose)
        printf("exfat: [exfat_mount] initializing upcase table\n");
    error = exfat_init_upcase(emp);
    if (error) {
        if (bootverbose)
            printf("exfat: [exfat_mount] failed to initialize upcase table: %d\n", error);
        goto error_exit;
    }

    error = exfat_init_nodes(emp);
    if (error) {
        printf("exfat: [exfat_mount] failed to initialize node hash table: %d\n", error);
        goto error_exit;
    }

    error = exfat_read_rootdir(emp);
    if (error)
        goto error_exit;

    mp->mnt_data = emp;
    vfs_mountedfrom(mp, from);

    if (bootverbose)
        printf("exfat: [exfat_mount] mount successful\n");
    return 0;

error_exit:
    if (bp != NULL)
        brelse(bp);

    if (cp != NULL) {
        g_topology_lock();
        g_vfs_close(cp);
        g_topology_unlock();
    }

    if (devvp != NULL) {
        atomic_store_rel_ptr((uintptr_t *)&dev->si_mountpt, 0);
        mntfs_freevp(devvp);
        dev_rel(dev);
    }

    if (emp != NULL) {
        if (emp->node_hash)
            exfat_destroy_nodes(emp);
        exfat_cleanup_upcase(emp);
        free(emp, M_EXFAT);
    }

    return (error);
}

/*
 * Unmount the filesystem
 */
static int
exfat_unmount(struct mount *mp, int mntflags)
{
    struct exfat_mount *emp = VFSTOEXFAT(mp);
    int error = 0;
    int flags = 0;

    if (bootverbose)
        printf("exfat: [exfat_unmount] unmounting filesystem\n");

    /* Set flags for vflush */
    if (mntflags & MNT_FORCE)
        flags |= FORCECLOSE;

    /* Flush all vnodes first */
    error = vflush(mp, 0, flags, curthread);
    if (error) {
        printf("exfat: [exfat_unmount] vflush failed: %d\n", error);
        return error;
    }

    /* Clean up mount structure */
    if (emp->node_hash)
        exfat_destroy_nodes(emp);
    exfat_cleanup_upcase(emp);
    free(emp, M_EXFAT);
    mp->mnt_data = NULL;

    if (bootverbose)
        printf("exfat: [exfat_unmount] unmount successful\n");

    return error;
}

/*
 * Get root vnode for mounted filesystem
 */
static int
exfat_root(struct mount *mp, int flags, struct vnode **vpp)
{
    struct exfat_mount *emp = VFSTOEXFAT(mp);
    int error;

    if (bootverbose)
        printf("exfat: [exfat_root] getting root vnode\n");

    error = exfat_get_node(mp, emp->root_cluster, EXFAT_TYPE_DIR, vpp);
    if (error) {
        printf("exfat: [exfat_root] failed to get root node: %d\n", error);
        return error;
    }

    if (bootverbose)
        printf("exfat: [exfat_root] root vnode setup complete\n");

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

    if (bootverbose)
        printf("exfat: [exfat_statfs] getting filesystem statistics\n");
    return 0;
}

/*
 * Sync the filesystem
 */
static int
exfat_sync(struct mount *mp, int waitfor)
{
    if (bootverbose)
        printf("exfat: [exfat_sync] syncing filesystem\n");
    struct exfat_mount *emp = VFSTOEXFAT(mp);
    struct vnode *vp;
    int error, allerror = 0;

    /* Don't sync if mounted read-only */
    if (mp->mnt_flag & MNT_RDONLY)
        return 0;

    /* First flush all vnodes */
    if (waitfor == MNT_WAIT) {
        if (bootverbose)
            printf("exfat: [exfat_sync] flushing all vnodes\n");
        struct vnode *mvp;
        vp = TAILQ_FIRST(&mp->mnt_nvnodelist);
        while (vp != NULL) {
            VI_LOCK(vp);
            mvp = TAILQ_NEXT(vp, v_nmntvnodes);
            VI_UNLOCK(vp);
            error = VOP_FSYNC(vp, MNT_WAIT, curthread);
            if (error) {
                if (bootverbose)
                    printf("exfat: [exfat_sync] vnode sync failed: %d\n", error);
                allerror = error;
            }
            vrele(vp);
            vp = mvp;
        }
    }

    /* Then sync memory-mapped files */
    if (bootverbose)
        printf("exfat: [exfat_sync] syncing memory-mapped files\n");
    error = vfs_stdsync(mp, MNT_WAIT);
    if (error) {
        if (bootverbose)
            printf("exfat: [exfat_sync] memory-mapped sync failed: %d\n", error);
        allerror = error;
    }

    /* Finally update percent-in-use after all changes are flushed */
    if (bootverbose)
        printf("exfat: [exfat_sync] updating percent-in-use\n");
    error = exfat_update_percent_in_use(emp);
    if (error) {
        if (bootverbose)
            printf("exfat: [exfat_sync] failed to update percent-in-use: %d\n", error);
        allerror = error;
    }

    if (bootverbose)
        printf("exfat: [exfat_sync] sync %s\n", allerror ? "failed" : "successful");
    return allerror;
}

VFS_SET(exfat_vfsops, exfat, 0);
MODULE_VERSION(exfat, 1); 