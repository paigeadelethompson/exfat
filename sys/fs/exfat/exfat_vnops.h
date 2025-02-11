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

#ifndef _EXFAT_VNOPS_H_
#define _EXFAT_VNOPS_H_

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/namei.h>
#include <sys/kernel.h>
#include <sys/vnode.h>

/* VFS vnode operations */
int exfat_lookup(struct vop_lookup_args *);
int exfat_cachedlookup(struct vop_cachedlookup_args *);
int exfat_create(struct vop_create_args *);
int exfat_mkdir(struct vop_mkdir_args *);
int exfat_remove(struct vop_remove_args *);
int exfat_rmdir(struct vop_rmdir_args *);
int exfat_read(struct vop_read_args *);
int exfat_write(struct vop_write_args *);
int exfat_getattr(struct vop_getattr_args *);
int exfat_readdir(struct vop_readdir_args *);
int exfat_inactive(struct vop_inactive_args *);
int exfat_reclaim(struct vop_reclaim_args *);
int exfat_rename(struct vop_rename_args *);
int exfat_access_wrapper(struct vop_access_args *);
int exfat_open(struct vop_open_args *);
int exfat_close(struct vop_close_args *);
int exfat_fsync(struct vop_fsync_args *);
int exfat_strategy(struct vop_strategy_args *);

int exfat_read_cluster(struct vnode *vp, struct exfat_mount *emp, uint32_t cluster, char *buffer);
int exfat_write_cluster(struct vnode *vp, struct exfat_mount *emp, uint32_t cluster, char *buffer);

/* Helper functions */
int exfat_access(struct vnode *, accmode_t, struct ucred *, struct thread *);
int exfat_dir_read(struct vnode *, struct uio *, struct ucred *);
int exfat_dir_write(struct vnode *, struct uio *, struct ucred *);
int exfat_file_read(struct vnode *, struct uio *, struct ucred *);
int exfat_file_write(struct vnode *, struct uio *, struct ucred *);

static vop_lookup_t __unused exfat_lookup;
static vop_read_t       exfat_read;
static vop_write_t      exfat_write;
static vop_getattr_t    exfat_getattr;
static vop_inactive_t   exfat_inactive;
static vop_reclaim_t    exfat_reclaim;

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

#endif /* _EXFAT_VNOPS_H_ */  