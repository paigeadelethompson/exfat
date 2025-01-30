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
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/namei.h>
#include <sys/dirent.h>

#include "exfat.h"
#include "exfat_node.h"
#include "exfat_dir.h"

/*
 * Lookup a file in a directory
 */
int
exfat_lookup_node(struct vnode *dvp, struct componentname *cnp, struct vnode **vpp)
{
    struct exfat_mount *emp = VTOVFSMP(dvp);
    struct exfat_scan_ctx ctx;
    struct exfat_direntry_set es;
    int error;

    /* Initialize directory scanning */
    error = exfat_scan_directory(dvp, &ctx);
    if (error)
        return error;

    /* Scan directory entries */
    while ((error = exfat_next_dirent(&ctx, &es)) == 0) {
        /* Check if name matches */
        if (exfat_name_match(emp, &es, cnp->cn_nameptr, cnp->cn_namelen)) {
            /* Found matching entry */
            error = exfat_get_node(dvp->v_mount, es.stream.first_cluster,
                                 (es.file.file_attributes & EXFAT_ATTR_DIRECTORY) ? EXFAT_VDIR : EXFAT_VREG,
                                 vpp);
            exfat_scan_cleanup(&ctx);
            return error;
        }
    }

    exfat_scan_cleanup(&ctx);
    return (error == ENOENT) ? ENOENT : error;
} 