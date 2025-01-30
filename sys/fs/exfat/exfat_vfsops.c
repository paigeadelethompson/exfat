/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024 The FreeBSD Foundation
 *
 * This software was developed by {Your Name or Organization}.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/namei.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/fcntl.h>
#include <sys/malloc.h>

#include "exfat.h"

static vfs_mount_t     exfat_mount;
static vfs_unmount_t   exfat_unmount;
static vfs_root_t      exfat_root;
static vfs_statfs_t    exfat_statfs;
static vfs_setlabel_t  exfat_setlabel;
static vfs_sync_t      exfat_sync;

static struct vfsops exfat_vfsops = {
    .vfs_mount      = exfat_mount,
    .vfs_unmount    = exfat_unmount,
    .vfs_root       = exfat_root,
    .vfs_statfs     = exfat_statfs,
    .vfs_setlabel   = exfat_setlabel,
    .vfs_sync       = exfat_sync,
};

/* After the mount structure definition, add: */
MALLOC_DEFINE(M_EXFAT, "exfat", "EXFAT filesystem");

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
 * Get next cluster in chain
 */
uint32_t
exfat_cluster_next(struct exfat_mount *emp, uint32_t cluster)
{
    uint32_t next;
    int error;

    if (cluster < 2 || cluster >= emp->boot.cluster_count + 2)
        return EXFAT_CLUSTER_END;

    error = exfat_read_fat_entry(emp, cluster, &next);
    if (error)
        return EXFAT_CLUSTER_END;

    return next;
}

/* Update the mount implementation to read the boot sector: */
static int
exfat_mount(struct mount *mp)
{
    struct exfat_mount *emp;
    struct buf *bp;
    int error;

    /* Allocate mount structure */
    emp = malloc(sizeof(struct exfat_mount), M_EXFAT, M_WAITOK | M_ZERO);
    mp->mnt_data = emp;
    emp->mp = mp;

    /* Read boot sector */
    error = bread(EXFAT_DEV(mp), 0, EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error) {
        free(emp, M_EXFAT);
        return error;
    }

    /* Copy and verify boot sector */
    memcpy(&emp->boot, bp->b_data, sizeof(struct exfat_boot_record));
    brelse(bp);

    if (emp->boot.fs_name[0] != 'E' ||
        emp->boot.fs_name[1] != 'X' ||
        emp->boot.fs_name[2] != 'F' ||
        emp->boot.fs_name[3] != 'A' ||
        emp->boot.fs_name[4] != 'T' ||
        emp->boot.fs_name[5] != ' ' ||
        emp->boot.fs_name[6] != ' ' ||
        emp->boot.fs_name[7] != ' ') {
        free(emp, M_EXFAT);
        return EINVAL;
    }

    /* Calculate important filesystem parameters */
    emp->bytes_per_cluster = EXFAT_SECTOR_SIZE << emp->boot.sectors_per_cluster_shift;
    emp->clusters_per_fat = emp->boot.fat_length >> emp->boot.sectors_per_cluster_shift;
    emp->root_cluster = emp->boot.root_dir_cluster;

    /* Initialize allocation bitmap */
    error = exfat_init_bitmap(emp);
    if (error) {
        free(emp, M_EXFAT);
        return error;
    }

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

    return 0;
}

/*
 * Unmount the filesystem
 */
static int
exfat_unmount(struct mount *mp, int mntflags)
{
    struct exfat_mount *emp = VFSTOEXFAT(mp);
    int error;

    /* Update percent-in-use before unmounting */
    if ((mp->mnt_flag & MNT_RDONLY) == 0) {
        error = exfat_update_percent_in_use(emp);
        if (error)
            return error;

        /* Mark volume as clean */
        error = exfat_set_volume_dirty(emp, 0);
        if (error)
            return error;
    }

    error = vflush(mp, 0, 0, curthread);
    if (error)
        return error;

    exfat_cleanup_upcase(emp);
    free(emp, M_EXFAT);
    mp->mnt_data = NULL;

    return 0;
}

/*
 * Get root vnode
 */
static int
exfat_root(struct mount *mp, int flags, struct vnode **vpp)
{
    struct exfat_mount *emp;
    struct vnode *vp;
    int error;

    emp = VFSTOEXFAT(mp);

    error = exfat_get_node(mp, emp->root_cluster, VDIR, &vp);
    if (error)
        return error;

    *vpp = vp;
    return 0;
}

/*
 * Get filesystem statistics
 */
static int
exfat_statfs(struct mount *mp, struct statfs *sbp)
{
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
 * Set volume label
 */
static int
exfat_setlabel(struct mount *mp, const char *label, struct thread *td)
{
    struct exfat_mount *emp = VFSTOEXFAT(mp);
    size_t len;

    /* Check for write access */
    if (mp->mnt_flag & MNT_RDONLY)
        return EROFS;

    len = strlen(label);

    /* Update volume label */
    return exfat_write_volume_label(emp, label, len);
}

/*
 * Sync the filesystem
 */
static int
exfat_sync(struct mount *mp, int waitfor)
{
    struct exfat_mount *emp = VFSTOEXFAT(mp);
    struct vnode *vp;
    int error, allerror = 0;

    /* Don't sync if mounted read-only */
    if (mp->mnt_flag & MNT_RDONLY)
        return 0;

    /* First flush all vnodes */
    if (waitfor == MNT_WAIT) {
        MNT_VNODE_FOREACH_ALL(vp, mp, v) {
            error = VOP_FSYNC(vp, MNT_WAIT, curthread);
            if (error)
                allerror = error;
            vrele(vp);
        }
    }

    /* Then sync memory-mapped files */
    error = vfs_msync(mp, MNT_WAIT, curthread);
    if (error)
        allerror = error;

    /* Finally update percent-in-use after all changes are flushed */
    error = exfat_update_percent_in_use(emp);
    if (error)
        allerror = error;

    return allerror;
}

/* Module load/unload handlers */
static int
exfat_init(struct vfsconf *vfsp)
{
    exfat_node_init();
    return 0;
}

static int
exfat_uninit(struct vfsconf *vfsp)
{
    exfat_node_uninit();
    return 0;
}

static struct vfsconf exfat_vfsconf = {
    .vfc_name = "exfat",
    .vfc_vfsops = &exfat_vfsops,
    .vfc_typenum = -1,
    .vfc_flags = VFCF_READONLY,
    .vfc_init = exfat_init,
    .vfc_uninit = exfat_uninit,
};

DECLARE_MODULE(exfat, exfat_vfsconf, SI_SUB_VFS, SI_ORDER_MIDDLE);
MODULE_VERSION(exfat, 1); 