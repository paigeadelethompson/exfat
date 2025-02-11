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
#include <sys/namei.h>
#include <sys/vnode.h>
#include <sys/types.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/mutex.h>
#include <sys/hash.h>
#include <sys/stat.h>

#include "exfat.h"
#include "exfat_node.h"

/* Map exfat node type to vnode type */
static int
exfat_type_to_vtype(int type)
{
    int vt;

    switch (type) {
    case EXFAT_TYPE_FILE:
        vt = VREG;
        break;
    case EXFAT_TYPE_DIR:
        vt = VDIR;
        break;
    default:
        vt = VNON;
        break;
    }
    return vt;
}

/*
 * Initialize the node hash table
 */
int
exfat_node_init(void)
{
    return 0;
}

/*
 * Clean up the node hash table
 */
void
exfat_node_uninit(void)
{
    /* Nothing to do - cleanup happens in unmount */
    return;
}

/*
 * Get a node from hash table or create new one
 */
int
exfat_get_node(struct mount *mp, uint32_t cluster, int type, struct vnode **vpp)
{
    struct exfat_mount *emp = VFSTOEXFAT(mp);
    struct exfat_node *ep;
    struct vnode *vp;
    int error;

    if (bootverbose) {
        printf("exfat: [exfat_get_node] getting node for cluster %u, type %d\n", 
               cluster, type);
        printf("exfat: [exfat_get_node] mount point: %p\n", emp);
    }

    /* Initialize node before getting vnode */
    ep = malloc(sizeof(*ep), M_EXFAT, M_WAITOK | M_ZERO);
    if (ep == NULL) {
        printf("exfat: [exfat_get_node] failed to allocate node\n");
        return ENOMEM;
    }

    ep->cluster = cluster;
    ep->type = type;
    ep->mp = mp;

    /* Initialize locks before getting vnode */
    mtx_init(&ep->lock, "exfat_node", NULL, MTX_DEF);

    /* Get vnode */
    error = getnewvnode("exfat", mp, &exfat_vnodeops, &vp);
    if (error) {
        printf("exfat: [exfat_get_node] failed to get new vnode: %d\n", error);
        mtx_destroy(&ep->lock);
        free(ep, M_EXFAT);
        return error;
    }

    vp->v_data = ep;
    ep->vnode = vp;

    /* Initialize vnode type */
    vp->v_type = IFTOVT(type);

    if (bootverbose) {
        printf("exfat: [exfat_get_node] created vnode %p with data %p\n", vp, ep);
    }

    /* Initialize vnode lock */
    lockinit(&vp->v_lock, PVFS, "exfat", VLKTIMEOUT, LK_CANRECURSE);

    /* Read file/directory information */
    error = exfat_read_node_info(emp, ep);
    if (error) {
        free(ep, M_EXFAT);
        vput(vp);
        return error;
    }

    /* Add to hash table */
    mtx_lock(&emp->hash_mtx.mtx);
    exfat_hash_insert(emp, ep);
    mtx_unlock(&emp->hash_mtx.mtx);

    *vpp = vp;
    return 0;
}

/*
 * Read node information from directory entry
 */
int
exfat_read_node_info(struct exfat_mount *emp, struct exfat_node *ep)
{
    struct buf *bp;
    struct exfat_entry_file *file_entry;
    struct exfat_entry_stream *stream_entry;
    uint32_t sector;
    int error;

    /* Calculate sector containing directory entry */
    sector = emp->boot.cluster_heap_offset +
             ((ep->cluster - 2) << emp->boot.sectors_per_cluster_shift);

    /* Read sector containing directory entry */
    error = bread(emp->devvp, sector, EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error) {
        brelse(bp);
        return error;
    }

    /* Parse file entry */
    file_entry = (struct exfat_entry_file *)bp->b_data;
    if (file_entry->type != EXFAT_ENTRY_FILE) {
        brelse(bp);
        return EINVAL;
    }

    /* Parse stream entry */
    stream_entry = (struct exfat_entry_stream *)(file_entry + 1);
    if (stream_entry->type != EXFAT_ENTRY_STREAM) {
        brelse(bp);
        return EINVAL;
    }

    /* Fill in file info structure */
    ep->finfo.first_cluster = stream_entry->first_cluster;
    ep->finfo.file_size = stream_entry->data_length;
    ep->finfo.valid_size = stream_entry->valid_data_length;
    ep->finfo.attributes = file_entry->file_attributes;

    brelse(bp);
    return 0;
}

void
exfat_node_put(struct exfat_node *ep)
{
    struct mount *mp = ETOV(ep)->v_mount;
    struct exfat_mount *emp = VFSTOEXFAT(mp);

    if (bootverbose)
        printf("exfat: [exfat_node_put] releasing node at cluster %u\n", ep->cluster);

    mtx_lock(&emp->hash_mtx.mtx);
    LIST_REMOVE(ep, next);
    mtx_unlock(&emp->hash_mtx.mtx);

    mtx_destroy(&ep->mtx);
    free(ep, M_EXFAT);
    if (bootverbose)
        printf("exfat: [exfat_node_put] node released\n");
}

int
exfat_init_nodes(struct exfat_mount *emp)
{
    if (bootverbose)
        printf("exfat: [exfat_init_nodes] initializing node hash table for mount %p\n", emp);

    /* Validate mount structure */
    if (emp == NULL) {
        printf("exfat: null mount structure in init_nodes\n");
        return EINVAL;
    }

    /* Allocate hash table */
    emp->node_hash = hashinit(EXFAT_HASH_SIZE, M_EXFAT, &emp->node_hash_mask);
    if (emp->node_hash == NULL) {
        printf("exfat: failed to allocate hash table\n");
        return ENOMEM;
    }

    if (bootverbose)
        printf("exfat: [exfat_init_nodes] initializing mutex at %p\n", &emp->hash_mtx.mtx);

    /* Initialize mutex */
    mtx_init(&emp->hash_mtx.mtx, "exfat_node_hash", NULL, MTX_DEF);

    if (bootverbose)
        printf("exfat: [exfat_init_nodes] node hash table initialized at %p, mutex at %p\n", emp->node_hash, &emp->hash_mtx.mtx);

    return 0;
}

void
exfat_destroy_nodes(struct exfat_mount *emp)
{
    if (bootverbose)
        printf("exfat: [exfat_destroy_nodes] destroying node hash table\n");

    if (emp->node_hash) {
        /* Free any remaining entries */
        for (u_long i = 0; i <= emp->node_hash_mask; i++) {
            struct exfat_node *node;
            while ((node = LIST_FIRST(&emp->node_hash[i])) != NULL) {
                LIST_REMOVE(node, next);
                free(node, M_EXFAT);
            }
        }
        /* Free hash table */
        hashdestroy(emp->node_hash, M_EXFAT, emp->node_hash_mask);
        mtx_destroy(&emp->hash_mtx.mtx);
        emp->node_hash = NULL;
    }
}

/* Node lookup function */
struct exfat_node *
exfat_hash_lookup(struct exfat_mount *emp, uint32_t cluster)
{
    struct exfat_node *ep;
    u_long hash_idx = cluster & emp->node_hash_mask;

    LIST_FOREACH(ep, &emp->node_hash[hash_idx], next) {
        if (ep->cluster == cluster)
            return ep;
    }
    return NULL;
}

/* Node insert function */
void
exfat_hash_insert(struct exfat_mount *emp, struct exfat_node *ep)
{
    u_long hash_idx = ep->cluster & emp->node_hash_mask;
    LIST_INSERT_HEAD(&emp->node_hash[hash_idx], ep, next);
}

/* Node remove function */
void
exfat_hash_remove(struct exfat_mount *emp, struct exfat_node *ep)
{
    LIST_REMOVE(ep, next);
}

