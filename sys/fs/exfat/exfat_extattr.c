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
int
exfat_getextattr(struct vnode *vp, int attrnamespace, const char *name,
                 struct uio *uio, size_t *size, struct ucred *cred)
{
    if (bootverbose)
        printf("exfat: getting extended attribute '%s' (namespace %d) for vnode %p\n",
               name, attrnamespace, vp);

    struct exfat_node *ep = VTOE(vp);
    struct exfat_extattr *eap;
    int error = 0;

    /* Look up attribute */
    LIST_FOREACH(eap, &ep->extattrs, link) {
        if (eap->namespace == attrnamespace &&
            strcmp(eap->name, name) == 0) {
            if (bootverbose)
                printf("exfat: found attribute, size %zu\n", eap->size);

            /* Return size if requested */
            if (size)
                *size = eap->size;

            /* Copy data if buffer provided */
            if (uio) {
                if (uio->uio_offset >= eap->size) {
                    if (bootverbose)
                        printf("exfat: offset %jd beyond attribute size\n",
                               (intmax_t)uio->uio_offset);
                    return 0;
                }
                error = uiomove((char *)eap->data + uio->uio_offset,
                              MIN(uio->uio_resid, eap->size - uio->uio_offset),
                              uio);
            }
            return error;
        }
    }

    if (bootverbose)
        printf("exfat: attribute not found\n");
    return ENOATTR;
}

/*
 * Set extended attribute
 */
int
exfat_setextattr(struct vnode *vp, int attrnamespace, const char *name,
                 struct uio *uio, struct ucred *cred)
{
    if (bootverbose)
        printf("exfat: setting extended attribute '%s' (namespace %d) for vnode %p\n",
               name, attrnamespace, vp);

    struct exfat_node *ep = VTOE(vp);
    struct exfat_extattr *eap;
    void *data;
    size_t size;
    int error;

    /* Check if mounted read-only */
    if (vp->v_mount->mnt_flag & MNT_RDONLY) {
        if (bootverbose)
            printf("exfat: cannot set attribute - filesystem is read-only\n");
        return EROFS;
    }

    /* Get attribute size */
    size = uio->uio_resid;
    if (size > EXFAT_EXTATTR_MAX_SIZE) {
        if (bootverbose)
            printf("exfat: attribute size %zu exceeds maximum %d\n",
                   size, EXFAT_EXTATTR_MAX_SIZE);
        return EFBIG;
    }

    /* Allocate buffer for attribute data */
    data = malloc(size, M_EXFAT, M_WAITOK);
    error = uiomove(data, size, uio);
    if (error) {
        if (bootverbose)
            printf("exfat: failed to copy attribute data: %d\n", error);
        free(data, M_EXFAT);
        return error;
    }

    /* Look for existing attribute */
    LIST_FOREACH(eap, &ep->extattrs, link) {
        if (eap->namespace == attrnamespace &&
            strcmp(eap->name, name) == 0) {
            if (bootverbose)
                printf("exfat: replacing existing attribute\n");
            /* Replace existing attribute */
            free(eap->data, M_EXFAT);
            eap->data = data;
            eap->size = size;
            return 0;
        }
    }

    /* Create new attribute */
    if (bootverbose)
        printf("exfat: creating new attribute\n");
    eap = malloc(sizeof(*eap), M_EXFAT, M_WAITOK);
    eap->namespace = attrnamespace;
    strlcpy(eap->name, name, sizeof(eap->name));
    eap->data = data;
    eap->size = size;
    LIST_INSERT_HEAD(&ep->extattrs, eap, link);

    return 0;
}

/*
 * List extended attributes
 */
int
exfat_listextattr(struct vnode *vp, int attrnamespace,
                  struct uio *uio, size_t *size, struct ucred *cred)
{
    if (bootverbose)
        printf("exfat: listing extended attributes (namespace %d) for vnode %p\n",
               attrnamespace, vp);

    struct exfat_node *ep = VTOE(vp);
    struct exfat_extattr *eap;
    size_t total = 0;
    int error = 0;

    /* Calculate total size first */
    LIST_FOREACH(eap, &ep->extattrs, link) {
        if (eap->namespace == attrnamespace)
            total += strlen(eap->name) + 1;
    }

    if (bootverbose)
        printf("exfat: found %zu bytes of attribute names\n", total);

    /* Return size if requested */
    if (size)
        *size = total;

    /* Copy names if buffer provided */
    if (uio) {
        LIST_FOREACH(eap, &ep->extattrs, link) {
            if (eap->namespace == attrnamespace) {
                size_t len = strlen(eap->name) + 1;
                if (uio->uio_resid < len)
                    break;
                error = uiomove(eap->name, len, uio);
                if (error)
                    break;
            }
        }
    }

    return error;
} 