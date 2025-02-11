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
    struct exfat_mount *emp;
    struct nameidata nd;
    struct g_consumer *cp;
    struct buf *bp = NULL;
    struct cdev *dev;
    char *from;
    int error;
    int ronly = (mp->mnt_flag & MNT_RDONLY) != 0;

    if (bootverbose)
        printf("exfat: [exfat_mount] mounting volume '%s'\n", mp->mnt_stat.f_mntfromname);

    if ((error = vfs_filteropt(mp->mnt_optnew, exfat_opts)) != 0)
        return (error);

    from = vfs_getopts(mp->mnt_optnew, "from", &error);
    if (error)
        return (error);

    // Initialize the namei data to lookup the device
    NDINIT(&nd, LOOKUP, FOLLOW | LOCKLEAF, UIO_SYSSPACE, from);
    error = namei(&nd);
    if (error)
        return (error);

    devvp = nd.ni_vp;
    NDFREE_PNBUF(&nd);

    if (!vn_isdisk(devvp, &error)) {
        vput(devvp);
        return (error);
    }

    /*
     * Open the device and set up the consumer
     */
    g_topology_lock();
    error = g_vfs_open(devvp, &cp, "exfat", ronly ? 0 : 1);
    g_topology_unlock();
    if (error) {
        vput(devvp);
        return (error);
    }

    emp = malloc(sizeof(*emp), M_EXFAT, M_WAITOK | M_ZERO);
    emp->mp = mp;
    emp->g_consumer = cp;  // Store the consumer
    emp->devvp = devvp;
    
    // Read and validate boot sector here...

    // Initialize core mount structures
    mp->mnt_data = emp;
    mp->mnt_stat.f_fsid.val[0] = dev2udev(devvp->v_rdev);
    mp->mnt_stat.f_fsid.val[1] = mp->mnt_vfc->vfc_typenum;
    mp->mnt_flag |= MNT_LOCAL;
    
    // Set up the root vnode
    error = exfat_read_rootdir(emp);
    if (error) {
        g_topology_lock(); 
        g_vfs_close(cp);
        g_topology_unlock();
        free(emp, M_EXFAT);
        vput(devvp);
        return error;
    }

    vfs_mountedfrom(mp, from);
    return 0;
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