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
#include <sys/malloc.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/extattr.h>

#include "exfat.h"

/*
 * Get extended attribute
 */
static int
exfat_getextattr(struct vop_getextattr_args *ap)
{
    (void)ap->a_vp;
    (void)ap->a_attrnamespace;
    (void)ap->a_name;
    (void)ap->a_uio;
    (void)ap->a_size;
    (void)ap->a_cred;
    (void)ap->a_td;

    /* We don't support extended attributes yet */
    return EOPNOTSUPP;
}

/*
 * Set extended attribute
 */
static int
exfat_setextattr(struct vop_setextattr_args *ap)
{
    (void)ap->a_vp;
    (void)ap->a_attrnamespace;
    (void)ap->a_name;
    (void)ap->a_uio;
    (void)ap->a_cred;
    (void)ap->a_td;

    /* We don't support extended attributes yet */
    return EOPNOTSUPP;
}

/*
 * List extended attributes
 */
static int
exfat_listextattr(struct vop_listextattr_args *ap)
{
    (void)ap->a_vp;
    (void)ap->a_attrnamespace;
    (void)ap->a_uio;
    (void)ap->a_size;
    (void)ap->a_cred;
    (void)ap->a_td;

    /* We don't support extended attributes yet */
    return EOPNOTSUPP;
} 