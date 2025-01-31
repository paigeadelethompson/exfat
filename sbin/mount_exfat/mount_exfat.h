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

#ifndef _MOUNT_EXFAT_H_
#define _MOUNT_EXFAT_H_

#include <sys/types.h>

/* Mount flags */
#define EXFAT_MNT_UID      0x00000001  /* Set owner of files */
#define EXFAT_MNT_GID      0x00000002  /* Set group of files */
#define EXFAT_MNT_MASK     0x00000004  /* Set mask for all files */
#define EXFAT_MNT_DMASK    0x00000008  /* Set mask for directories */
#define EXFAT_MNT_FMASK    0x00000010  /* Set mask for regular files */

/* Mount arguments structure */
struct exfat_args {
    char    *fspec;         /* Block special device to mount */
    uid_t   uid;           /* Owner of files */
    gid_t   gid;           /* Group of files */
    mode_t  mask;          /* Mask for all files */
    mode_t  dmask;         /* Mask for directories */
    mode_t  fmask;         /* Mask for regular files */
    u_int   flags;         /* Mount flags */
};

#endif /* _MOUNT_EXFAT_H_ */ 