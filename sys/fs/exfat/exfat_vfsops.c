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
#include <sys/vmmeter.h>  /* For vfs_busy */

#include "exfat.h"
#include "exfat_fat.h"
#include "exfat_node.h"

/* Mount options */
static const char *exfat_opts[] = {
    "from",         /* device path */
    "async",        /* async writes */
    "noatime",      /* don't update access times */
    "force",        /* force mount */
    "ro",          /* read-only */
    "rw",          /* read-write */
    NULL
};

static vfs_mount_t     exfat_mount;
static vfs_unmount_t   exfat_unmount;
static vfs_root_t      exfat_root;
static vfs_statfs_t    exfat_statfs;
static vfs_sync_t      exfat_sync;
static vfs_init_t      exfat_init;

static int exfat_mountfs(struct vnode *devvp, struct mount *mp);
static int exfat_reload(struct mount *mp);

static int
exfat_init(struct vfsconf *vfsp)
{
    if (bootverbose)
        printf("exfat: [exfat_init] initializing filesystem\n");
    exfat_init_vnops();
    return 0;
}

static int
exfat_mountfs(struct vnode *devvp, struct mount *mp)
{
    struct exfat_mount *emp;
    struct buf *bp = NULL;
    struct g_consumer *cp;
    struct cdev *dev;
    int error;
    int ronly = (mp->mnt_flag & MNT_RDONLY) != 0;

    if (bootverbose)
        printf("exfat: [exfat_mountfs] mounting device\n");

    /* Set up the device vnode */
    dev = devvp->v_rdev;
    if (atomic_cmpset_acq_ptr((uintptr_t *)&dev->si_mountpt, 0,
        (uintptr_t)mp) == 0) {
        return (EBUSY);
    }
    dev_ref(dev);

    /* Open the device through GEOM */
    g_topology_lock();
    error = g_vfs_open(devvp, &cp, "exfat", ronly ? 0 : 1);
    g_topology_unlock();
    if (error) {
        if (bootverbose)
            printf("exfat: [exfat_mountfs] failed to open device: %d\n", error);
        atomic_store_rel_ptr((uintptr_t *)&dev->si_mountpt, 0);
        dev_rel(dev);
        return error;
    }

    /* Allocate mount structure */
    emp = malloc(sizeof(*emp), M_EXFAT, M_WAITOK | M_ZERO);
    emp->mp = mp;
    emp->devvp = devvp;
    emp->g_consumer = cp;

    /* Read boot sector */
    if (bootverbose)
        printf("exfat: [exfat_mountfs] reading boot sector\n");
    error = bread(devvp, 0, EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error) {
        if (bootverbose)
            printf("exfat: [exfat_mountfs] failed to read boot sector: %d\n", error);
        goto error_exit;
    }

    /* Copy and validate boot sector */
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
            printf("exfat: [exfat_mountfs] invalid filesystem signature\n");
        error = EINVAL;
        goto error_exit;
    }

    /* Set up mount parameters */
    emp->bytes_per_cluster = EXFAT_SECTOR_SIZE << emp->boot.sectors_per_cluster_shift;
    emp->clusters_count = emp->boot.cluster_count;
    emp->root_cluster = emp->boot.root_dir_cluster;

    /* Initialize filesystem structures */
    if (bootverbose)
        printf("exfat: [exfat_mountfs] initializing bitmap\n");
    error = exfat_init_bitmap(emp);
    if (error)
        goto error_exit;

    if (bootverbose)
        printf("exfat: [exfat_mountfs] initializing upcase table\n");
    error = exfat_init_upcase(emp);
    if (error)
        goto error_exit;

    if (bootverbose)
        printf("exfat: [exfat_mountfs] initializing node hash table\n");
    error = exfat_init_nodes(emp);
    if (error)
        goto error_exit;

    /* Read root directory */
    error = exfat_read_rootdir(emp);
    if (error)
        goto error_exit;

    /* Set up mount point */
    mp->mnt_data = emp;
    mp->mnt_stat.f_fsid.val[0] = dev2udev(devvp->v_rdev);
    mp->mnt_stat.f_fsid.val[1] = mp->mnt_vfc->vfc_typenum;
    mp->mnt_flag |= MNT_LOCAL;

    if (bootverbose)
        printf("exfat: [exfat_mountfs] filesystem mounted successfully\n");
    return 0;

error_exit:
    if (bp)
        brelse(bp);
    if (cp) {
        g_topology_lock();
        g_vfs_close(cp);
        g_topology_unlock();
    }
    if (emp) {
        if (emp->node_hash)
            exfat_destroy_nodes(emp);
        exfat_cleanup_upcase(emp);
        free(emp, M_EXFAT);
    }
    return error;
}

static int
exfat_mount(struct mount *mp)
{
    struct vnode *devvp;
    struct nameidata nd;
    char *from;
    int error;

    if (bootverbose)
        printf("exfat: [exfat_mount] mounting filesystem\n");

    error = vfs_filteropt(mp->mnt_optnew, exfat_opts);
    if (error)
        return error;

    if (mp->mnt_flag & MNT_UPDATE) {
        if (bootverbose)
            printf("exfat: [exfat_mount] updating mount\n");
        return exfat_reload(mp);
    }

    /* Lookup mount device */
    from = vfs_getopts(mp->mnt_optnew, "from", &error);
    if (error)
        return error;

    NDINIT(&nd, LOOKUP, FOLLOW | LOCKLEAF, UIO_SYSSPACE, from);
    error = namei(&nd);
    if (error)
        return error;

    devvp = nd.ni_vp;
    NDFREE_PNBUF(&nd);

    if (!vn_isdisk(devvp)) {
        vput(devvp);
        return ENOTBLK;
    }

    /* Get new vnode for block device */
    error = vfs_busy(mp, MBF_NOWAIT);  /* Don't wait forever */
    if (error) {
        vput(devvp);
        return error;
    }

    error = exfat_mountfs(devvp, mp);
    if (error) {
        vfs_unbusy(mp);
        mntfs_freevp(devvp);
        return error;
    }

    vfs_mountedfrom(mp, from);
    return 0;
}

static int
exfat_reload(struct mount *mp)
{
    if (bootverbose)
        printf("exfat: [exfat_reload] reloading filesystem\n");

    /* For now, don't support update mounting */
    return EOPNOTSUPP;
}

static int
exfat_unmount(struct mount *mp, int mntflags)
{
    struct exfat_mount *emp = VFSTOEXFAT(mp);
    int error = 0;
    int flags = 0;

    if (bootverbose)
        printf("exfat: [exfat_unmount] unmounting filesystem\n");

    if (mntflags & MNT_FORCE)
        flags |= FORCECLOSE;

    /* Check if mount is already being torn down */
    if (mp->mnt_kern_flag & MNTK_UNMOUNT) {
        if (bootverbose)
            printf("exfat: [exfat_unmount] already unmounting\n");
        return 0;
    }

    /* Try flush with timeout */
    error = vflush(mp, 0, flags | VNORESOURCE, curthread);
    if (error && !(flags & FORCECLOSE))
        return error;

    if (emp && emp->devvp) {
        /* Try lock with timeout */
        error = vn_lock(emp->devvp, LK_EXCLUSIVE | LK_TIMELOCK);
        if (error == 0) {
            error = VOP_CLOSE(emp->devvp, FREAD|FWRITE, NOCRED, curthread);
            VOP_UNLOCK(emp->devvp);
        }
        vrele(emp->devvp);
    }

    /* Free mount structure */
    free(emp->node_hash, M_EXFAT);
    mtx_destroy(&emp->hash_mtx.mtx);
    free(emp, M_EXFAT);
    mp->mnt_data = NULL;

    return error;
}

static int
exfat_root(struct mount *mp, int flags, struct vnode **vpp)
{
    struct exfat_mount *emp = VFSTOEXFAT(mp);
    int error;

    if (bootverbose)
        printf("exfat: [exfat_root] getting root vnode\n");

    error = exfat_get_node(mp, emp->root_cluster, EXFAT_TYPE_DIR, vpp);
    if (error) {
        if (bootverbose)
            printf("exfat: [exfat_root] failed to get root node: %d\n", error);
        return error;
    }

    return 0;
}

static int
exfat_statfs(struct mount *mp, struct statfs *sbp)
{
    struct exfat_mount *emp = VFSTOEXFAT(mp);

    if (bootverbose)
        printf("exfat: [exfat_statfs] getting filesystem statistics\n");

    sbp->f_bsize = emp->bytes_per_cluster;
    sbp->f_iosize = emp->bytes_per_cluster;
    sbp->f_blocks = emp->clusters_count;
    sbp->f_bfree = emp->free_clusters;
    sbp->f_bavail = emp->free_clusters;
    sbp->f_files = 0;  /* No way to know */
    sbp->f_ffree = 0;  /* No way to know */
    sbp->f_namemax = 255;

    return 0;
}

static int
exfat_sync(struct mount *mp, int waitfor)
{
    struct vnode *vp, *mvp;
    int error, allerror = 0;

    if (bootverbose)
        printf("exfat: [exfat_sync] syncing filesystem\n");

    if (mp->mnt_flag & MNT_RDONLY)
        return 0;

    /* Sync all vnodes */
    if (waitfor == MNT_WAIT) {
        vp = TAILQ_FIRST(&mp->mnt_nvnodelist);
        while (vp != NULL) {
            VI_LOCK(vp);
            mvp = TAILQ_NEXT(vp, v_nmntvnodes);
            VI_UNLOCK(vp);
            error = VOP_FSYNC(vp, waitfor, curthread);
            if (error)
                allerror = error;
            vrele(vp);
            vp = mvp;
        }
    }

    /* Sync memory-mapped files */
    if ((error = vfs_stdsync(mp, waitfor)) != 0)
        allerror = error;

    return allerror;
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

VFS_SET(exfat_vfsops, exfat, 0);
MODULE_VERSION(exfat, 1); 