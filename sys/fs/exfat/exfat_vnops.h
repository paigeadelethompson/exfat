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

#ifndef _FS_EXFAT_VNODEOPS_H_
#define _FS_EXFAT_VNODEOPS_H_

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

#endif /* _FS_EXFAT_VNODEOPS_H_ */  