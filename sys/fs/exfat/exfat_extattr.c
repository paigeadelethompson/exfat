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
    struct vnode *vp = ap->a_vp;
    int attrnamespace = ap->a_attrnamespace;
    const char *name = ap->a_name;
    struct uio *uio = ap->a_uio;
    size_t *size = ap->a_size;
    struct ucred *cred = ap->a_cred;
    struct thread *td = ap->a_td;

    /* We don't support extended attributes yet */
    return EOPNOTSUPP;
}

/*
 * Set extended attribute
 */
static int
exfat_setextattr(struct vop_setextattr_args *ap)
{
    struct vnode *vp = ap->a_vp;
    int attrnamespace = ap->a_attrnamespace;
    const char *name = ap->a_name;
    struct uio *uio = ap->a_uio;
    struct ucred *cred = ap->a_cred;
    struct thread *td = ap->a_td;

    /* We don't support extended attributes yet */
    return EOPNOTSUPP;
}

/*
 * List extended attributes
 */
static int
exfat_listextattr(struct vop_listextattr_args *ap)
{
    struct vnode *vp = ap->a_vp;
    int attrnamespace = ap->a_attrnamespace;
    struct uio *uio = ap->a_uio;
    size_t *size = ap->a_size;
    struct ucred *cred = ap->a_cred;
    struct thread *td = ap->a_td;

    /* We don't support extended attributes yet */
    if (size)
        *size = 0;
    return 0;
} 