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

#ifndef _MOUNT_EXFAT_EXFAT_H_
#define _MOUNT_EXFAT_EXFAT_H_

#include <sys/types.h>
#include <sys/mount.h>

/* Mount flags */
#define EXFAT_MNT_UID             0x00000001
#define EXFAT_MNT_GID             0x00000002
#define EXFAT_MNT_MASK            0x00000004
#define EXFAT_MNT_DMASK           0x00000008
#define EXFAT_MNT_FMASK           0x00000010
#define EXFAT_MNT_NOACLS          0x00000020  /* Don't support ACLs */
#define EXFAT_MNT_FORCE          0x00000040  /* Force mount even if dirty */

/* Additional mount flags */
#define EXFAT_MNT_NOFAIL         0x00000080  /* Don't fail if unclean */
#define EXFAT_MNT_NOATIME        0x00000100  /* Don't update access times */
#define EXFAT_MNT_RECOVER        0x00000200  /* Recover dangling clusters */

/* Mount error codes */
#define EXFAT_ERR_IO             1   /* I/O error */
#define EXFAT_ERR_INVALID        2   /* Invalid filesystem */
#define EXFAT_ERR_DIRTY          3   /* Filesystem is dirty */
#define EXFAT_ERR_UNSUPPORTED    4   /* Unsupported feature */

/* Default mount options */
#define EXFAT_DEFUID             0   /* Default UID */
#define EXFAT_DEFGID             0   /* Default GID */
#define EXFAT_DEFMASK            0755 /* Default mask */

/* Mount arguments structure */
struct exfat_args {
    char    *fspec;         /* Block special device to mount */
    uid_t    uid;          /* Owner of files */
    gid_t    gid;          /* Group of files */
    mode_t   mask;         /* Mask for all files */
    mode_t   dmask;        /* Mask for directories */
    mode_t   fmask;        /* Mask for regular files */
    uint32_t flags;        /* Mount flags */
};

#endif /* _MOUNT_EXFAT_EXFAT_H_ */ 