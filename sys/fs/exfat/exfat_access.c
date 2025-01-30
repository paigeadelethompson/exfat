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
#include <sys/kernel.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/priv.h>
#include <sys/namei.h>

#include "exfat.h"
#include "exfat_node.h"  /* For struct exfat_node definition */

/*
 * Check access permissions
 */
int
exfat_access(struct vnode *vp, accmode_t accmode, struct ucred *cred,
             struct thread *td)
{
    struct exfat_node *ep = VTOE(vp);

    /* Handle read-only filesystem */
    if ((accmode & VWRITE) && (vp->v_mount->mnt_flag & MNT_RDONLY)) {
        if (vp->v_type == VDIR)
            return EISDIR;
        return EROFS;
    }

    /* Root can do anything except execute */
    if (priv_check_cred(cred, PRIV_VFS_ADMIN) == 0) {
        if ((accmode & VEXEC) && vp->v_type != VDIR && 
            (ep->finfo.attributes & EXFAT_ATTR_DIRECTORY) == 0) {
            return EACCES;
        }
        return 0;
    }

    /* Check if file is read-only */
    if ((accmode & VWRITE) && (ep->finfo.attributes & EXFAT_ATTR_READ_ONLY))
        return EACCES;

    /* Directory must be executable to be searchable */
    if (vp->v_type == VDIR && (accmode & VEXEC) == 0)
        return EACCES;

    return 0;
} 