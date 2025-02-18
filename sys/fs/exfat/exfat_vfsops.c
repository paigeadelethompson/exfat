/*-
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/fcntl.h>
#include <sys/namei.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/malloc.h>
#include <sys/conf.h>
#include <geom/geom.h>
#include <geom/geom_vfs.h>

#include "exfat.h"
#include "exfat_node.h"

/*
 * Mount options that can be specified
 */
static const char *exfat_opts[] = {
    "from", "async", "noatime", "force", "ro", "rw", NULL
};

/*
 * exfat_mountfs: Mount an exFAT filesystem
 *
 * Purpose:
 * - Initialize filesystem structures
 * - Read and validate boot sector
 * - Set up mount parameters
 * - Initialize bitmap and upcase table
 * - Handle mount errors
 *
 * Function calls:
 * - bread/brelse: Buffer cache operations
 * - g_vfs_open: Open GEOM provider
 * - exfat_init_bitmap: Initialize allocation bitmap
 * - exfat_init_upcase: Initialize upcase table
 * - exfat_init_nodes: Initialize node hash table
 */
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

    dev = devvp->v_rdev;
    if (atomic_cmpset_acq_ptr((uintptr_t *)&dev->si_mountpt, 0,
        (uintptr_t)mp) == 0) {
        return (EBUSY);
    }
    dev_ref(dev);

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

    emp = malloc(sizeof(*emp), M_EXFAT, M_WAITOK | M_ZERO);
    emp->mp = mp;
    emp->devvp = devvp;
    emp->g_consumer = cp;

    if (bootverbose)
        printf("exfat: [exfat_mountfs] reading boot sector\n");
    error = bread(devvp, 0, EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error) {
        if (bootverbose)
            printf("exfat: [exfat_mountfs] failed to read boot sector: %d\n", error);
        goto error_exit;
    }

    memcpy(&emp->boot, bp->b_data, sizeof(struct exfat_boot_record));
    brelse(bp);
    bp = NULL;

    if (memcmp(emp->boot.fs_name, "EXFAT   ", 8) != 0) {
        if (bootverbose)
            printf("exfat: [exfat_mountfs] invalid filesystem signature\n");
        error = EINVAL;
        goto error_exit;
    }

    /* Set up basic mount parameters */
    emp->bytes_per_cluster = EXFAT_SECTOR_SIZE << emp->boot.sectors_per_cluster_shift;
    emp->root_cluster = le32toh(emp->boot.root_dir_cluster);

    if (bootverbose)
        printf("exfat: [exfat_mountfs] root directory at cluster %u\n", emp->root_cluster);

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
    atomic_store_rel_ptr((uintptr_t *)&dev->si_mountpt, 0);
    dev_rel(dev);
    return error;
}

/*
 * exfat_mount: Handle mount(2) system call
 * 
 * Purpose:
 * - Process mount options
 * - Open device vnode
 * - Initialize filesystem
 * - Handle mount errors
 *
 * Function calls:
 * - vfs_filteropt: Filter mount options
 * - namei: Lookup device path
 * - exfat_mountfs: Mount filesystem
 */
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
    if (error) {
        if (bootverbose)
            printf("exfat: [exfat_mount] invalid mount options\n");
        return error;
    }

    if (mp->mnt_flag & MNT_UPDATE) {
        if (bootverbose)
            printf("exfat: [exfat_mount] filesystem update not supported\n");
        return EOPNOTSUPP;
    }

    from = vfs_getopts(mp->mnt_optnew, "from", &error);
    if (error) {
        if (bootverbose)
            printf("exfat: [exfat_mount] missing 'from' option\n");
        return error;
    }

    if (bootverbose)
        printf("exfat: [exfat_mount] looking up device %s\n", from);

    NDINIT(&nd, LOOKUP, FOLLOW, UIO_SYSSPACE, from);
    error = namei(&nd);
    if (error) {
        if (bootverbose)
            printf("exfat: [exfat_mount] device lookup failed: %d\n", error);
        return error;
    }

    devvp = nd.ni_vp;
    NDFREE_PNBUF(&nd);
    vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY | LK_NOWITNESS);

    if (!vn_isdisk(devvp)) {
        if (bootverbose)
            printf("exfat: [exfat_mount] not a block device\n");
        vput(devvp);
        return ENOTBLK;
    }

    error = exfat_mountfs(devvp, mp);
    if (error) {
        if (bootverbose)
            printf("exfat: [exfat_mount] mount failed: %d\n", error);
        vput(devvp);
        return error;
    }

    if (bootverbose)
        printf("exfat: [exfat_mount] mounted successfully\n");

    vfs_mountedfrom(mp, from);
    VOP_UNLOCK(devvp);
    return 0;
}

/*
 * exfat_unmount: Handle unmount(2) system call
 *
 * Purpose:
 * - Flush cached data
 * - Close device vnode
 * - Free filesystem resources
 * - Handle unmount errors
 *
 * Function calls:
 * - vflush: Flush vnodes
 * - g_vfs_close: Close GEOM provider
 * - exfat_destroy_nodes: Free node hash table
 */
static int
exfat_unmount(struct mount *mp, int mntflags)
{
    if (bootverbose)
        printf("exfat: [exfat_unmount] unmounting filesystem\n");
    struct exfat_mount *emp = VFSTOEXFAT(mp);
    int error = 0;
    int flags = 0;

    if (mntflags & MNT_FORCE)
        flags |= FORCECLOSE;

    error = vflush(mp, 0, flags, curthread);
    if (error && !(flags & FORCECLOSE))
        return error;

    if (emp && emp->devvp) {
        struct cdev *dev = emp->devvp->v_rdev;
        g_topology_lock();
        g_vfs_close(emp->g_consumer);
        g_topology_unlock();
        vrele(emp->devvp);
        atomic_store_rel_ptr((uintptr_t *)&dev->si_mountpt, 0);
        dev_rel(dev);
    }

    if (emp) {
        if (emp->node_hash)
            exfat_destroy_nodes(emp);
        exfat_cleanup_upcase(emp);
        free(emp, M_EXFAT);
        mp->mnt_data = NULL;
    }

    return error;
}

/*
 * exfat_root: Get root directory vnode
 *
 * Purpose:
 * - Return vnode for root directory
 * - Initialize root vnode if needed
 * - Handle root lookup errors
 *
 * Function calls:
 * - exfat_get_node: Get vnode for cluster
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

    // if (bootverbose)
    //     printf("exfat: [exfat_root] got root vnode %p, adding reference\n", *vpp);

    // /* Hold reference on root vnode */
    // vref(*vpp);

    // if (bootverbose)
    //     printf("exfat: [exfat_root] root vnode setup complete, returning\n");

    return 0;
}

/*
 * exfat_statfs: Get filesystem statistics
 *
 * Purpose:
 * - Return filesystem information
 * - Calculate free space
 * - Set filesystem parameters
 *
 * Function calls:
 * - None (uses cached mount info)
 */
static int
exfat_statfs(struct mount *mp, struct statfs *sbp)
{
    if (bootverbose)
        printf("exfat: [exfat_statfs] getting filesystem statistics\n");
    struct exfat_mount *emp = VFSTOEXFAT(mp);

    sbp->f_bsize = emp->bytes_per_cluster;
    sbp->f_iosize = emp->bytes_per_cluster;
    sbp->f_blocks = emp->clusters_count;
    sbp->f_bfree = emp->free_clusters;
    sbp->f_bavail = emp->free_clusters;
    sbp->f_files = 0;
    sbp->f_ffree = 0;
    sbp->f_namemax = 255;

    return 0;
}

/*
 * exfat_init: Initialize filesystem
 *
 * Purpose:
 * - Register vnode operations
 * - Initialize filesystem structures
 * - Called during module load
 *
 * Function calls:
 * - exfat_init_vnops: Initialize vnode operations
 */
static int
exfat_init(struct vfsconf *vfsp)
{
    if (bootverbose)
        printf("exfat: [exfat_init] initializing filesystem\n");
   
    /* Register vnode operations */
    exfat_init_vnops();
    return 0;
}

static struct vfsops exfat_vfsops = {
    .vfs_mount      = exfat_mount,
    .vfs_unmount    = exfat_unmount,
    .vfs_root       = exfat_root,
    .vfs_statfs     = exfat_statfs,
    .vfs_init       = exfat_init,
};

MALLOC_DEFINE(M_EXFAT, "exfat", "EXFAT filesystem");
VFS_SET(exfat_vfsops, exfat, 0);
MODULE_VERSION(exfat, 1);
