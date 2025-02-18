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

/*
 * exfat_node_init: Initialize node subsystem
 *
 * Purpose:
 * - One-time initialization of node management
 * - Called during filesystem module load
 * - Currently a no-op as initialization happens per-mount
 *
 * Function calls:
 * - None
 */
int
exfat_node_init(void)
{
    return 0;
}

/*
 * exfat_node_uninit: Clean up node subsystem
 *
 * Purpose:
 * - One-time cleanup of node management
 * - Called during filesystem module unload
 * - Currently a no-op as cleanup happens per-mount
 *
 * Function calls:
 * - None
 */
void
exfat_node_uninit(void)
{
    /* Nothing to do - cleanup happens in unmount */
    return;
}

/*
 * exfat_read_node_info: Read node metadata from directory entry
 *
 * Purpose:
 * - Read file/directory metadata from disk
 * - Parse file and stream directory entries
 * - Fill in node information structure
 * - Handle special case for root directory
 *
 * Function calls:
 * - bread: Read from buffer cache
 *   https://man.freebsd.org/cgi/man.cgi?query=bread&sektion=9
 * - brelse: Release buffer
 *   https://man.freebsd.org/cgi/man.cgi?query=brelse&sektion=9
 */
int
exfat_read_node_info(struct exfat_mount *emp, struct exfat_node *ep)
{
    struct buf *bp;
    struct exfat_entry_file *file_entry;
    struct exfat_entry_stream *stream_entry;
    uint32_t sector;
    int error;

    if (bootverbose)
        printf("exfat: [exfat_read_node_info] reading info for cluster %u\n", ep->cluster);

    /* Special case for root directory */
    if (ep->cluster == emp->root_cluster) {
        ep->finfo.first_cluster = ep->cluster;
        ep->finfo.file_size = 0;  /* Size unknown for root dir */
        ep->finfo.valid_size = 0;
        ep->finfo.attributes = EXFAT_ATTR_DIRECTORY;
        return 0;
    }

    /* Calculate sector containing directory entry */
    sector = emp->boot.cluster_heap_offset + 
             ((ep->cluster - EXFAT_CLUSTER_FIRST) * (1 << emp->boot.sectors_per_cluster_shift));

    if (bootverbose)
        printf("exfat: [exfat_read_node_info] reading sector %u\n", sector);

    /* Read sector containing directory entry */
    error = bread(emp->devvp, sector, EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error) {
        printf("exfat: [exfat_read_node_info] bread failed: %d\n", error);
        return error;
    }

    /* Parse file entry */
    file_entry = (struct exfat_entry_file *)bp->b_data;
    if (file_entry->type != EXFAT_ENTRY_FILE) {
        printf("exfat: [exfat_read_node_info] invalid file entry type: %02x\n", file_entry->type);
        brelse(bp);
        return EINVAL;
    }

    /* Parse stream entry */
    stream_entry = (struct exfat_entry_stream *)(file_entry + 1);
    if (stream_entry->type != EXFAT_ENTRY_STREAM) {
        printf("exfat: [exfat_read_node_info] invalid stream entry type: %02x\n", stream_entry->type);
        brelse(bp);
        return EINVAL;
    }

    /* Fill in file info structure */
    ep->finfo.first_cluster = stream_entry->first_cluster;
    ep->finfo.file_size = stream_entry->data_length;
    ep->finfo.valid_size = stream_entry->valid_data_length;
    ep->finfo.attributes = file_entry->file_attributes;

    if (bootverbose)
        printf("exfat: [exfat_read_node_info] node info read successfully\n");

    brelse(bp);
    return 0;
}

/*
 * exfat_init_nodes: Initialize per-mount node management
 *
 * Purpose:
 * - Initialize node hash table for mount
 * - Set up node mutex
 * - Called during filesystem mount
 *
 * Function calls:
 * - hashinit: Create hash table
 *   https://man.freebsd.org/cgi/man.cgi?query=hashinit&sektion=9
 * - mtx_init: Initialize mutex
 *   https://man.freebsd.org/cgi/man.cgi?query=mtx_init&sektion=9
 */
int
exfat_init_nodes(struct exfat_mount *emp)
{
    if (bootverbose)
        printf("exfat: [exfat_init_nodes] initializing node hash table for mount %p\n", emp);

    /* Validate mount structure */
    if (emp == NULL) {
        printf("exfat: [exfat_init_nodes] null mount structure in init_nodes\n");
        return EINVAL;
    }

    /* Allocate hash table */
    emp->node_hash = hashinit(EXFAT_HASH_SIZE, M_EXFAT, &emp->node_hash_mask);
    if (emp->node_hash == NULL) {
        printf("exfat: [exfat_init_nodes] failed to allocate hash table\n");
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

/*
 * exfat_destroy_nodes: Clean up per-mount node management
 *
 * Purpose:
 * - Free all nodes in hash table
 * - Destroy hash table
 * - Clean up node mutex
 * - Called during filesystem unmount
 *
 * Function calls:
 * - hashdestroy: Free hash table
 *   https://man.freebsd.org/cgi/man.cgi?query=hashdestroy&sektion=9
 * - mtx_destroy: Clean up mutex
 *   https://man.freebsd.org/cgi/man.cgi?query=mtx_destroy&sektion=9
 * - free: Free memory
 *   https://man.freebsd.org/cgi/man.cgi?query=free&sektion=9
 */
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

/*
 * exfat_hash_lookup: Look up node in hash table
 *
 * Purpose:
 * - Find node by cluster number
 * - Used to locate existing nodes
 * - Called with node mutex held
 *
 * Function calls:
 * - LIST_FOREACH: Traverse hash chain
 *   https://man.freebsd.org/cgi/man.cgi?query=queue&sektion=3
 */
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

/*
 * exfat_hash_insert: Insert node into hash table
 *
 * Purpose:
 * - Add new node to hash table
 * - Called with node mutex held
 * - Used when creating new nodes
 *
 * Function calls:
 * - LIST_INSERT_HEAD: Add to hash chain
 *   https://man.freebsd.org/cgi/man.cgi?query=queue&sektion=3
 */
void
exfat_hash_insert(struct exfat_mount *emp, struct exfat_node *ep)
{
    u_long hash_idx = ep->cluster & emp->node_hash_mask;
    LIST_INSERT_HEAD(&emp->node_hash[hash_idx], ep, next);
}

/*
 * exfat_hash_remove: Remove node from hash table
 *
 * Purpose:
 * - Remove node from hash table
 * - Called with node mutex held
 * - Used when freeing nodes
 *
 * Function calls:
 * - LIST_REMOVE: Remove from hash chain
 *   https://man.freebsd.org/cgi/man.cgi?query=queue&sektion=3
 */
void
exfat_hash_remove(struct exfat_mount *emp, struct exfat_node *ep)
{
    LIST_REMOVE(ep, next);
}

/*
 * exfat_find_node: Look up existing node in hash table
 *
 * Purpose:
 * - Find node by cluster number in hash table
 * - Get reference to associated vnode
 * - Lock vnode for exclusive access
 * - Used for accessing existing filesystem objects
 *
 * Function calls:
 * - mtx_lock/mtx_unlock: Lock hash table mutex
 *   https://man.freebsd.org/cgi/man.cgi?query=mtx_lock&sektion=9
 * - exfat_hash_lookup: Find node in hash (local)
 * - vref: Increment vnode reference
 *   https://man.freebsd.org/cgi/man.cgi?query=vref&sektion=9
 * - vget: Get and lock vnode
 *   https://man.freebsd.org/cgi/man.cgi?query=vget&sektion=9
 */
static int
exfat_find_node(struct exfat_mount *emp, uint32_t cluster, struct vnode **vpp)
{
    struct exfat_node *ep;
    struct vnode *vp;
    int error;

    if (bootverbose)
        printf("exfat: [exfat_find_node] looking up cluster %u\n", cluster);

    mtx_lock(&emp->hash_mtx.mtx);
    if (bootverbose)
        printf("exfat: [exfat_find_node] searching hash table\n");

    ep = exfat_hash_lookup(emp, cluster);
    if (ep == NULL) {
        if (bootverbose)
            printf("exfat: [exfat_find_node] cluster %u not found\n", cluster);
        mtx_unlock(&emp->hash_mtx.mtx);
        return ENOENT;
    }
    
    if (bootverbose)
        printf("exfat: [exfat_find_node] found node for cluster %u\n", cluster);

    vp = ETOV(ep);
    vref(vp);
    mtx_unlock(&emp->hash_mtx.mtx);

    if (bootverbose)
        printf("exfat: [exfat_find_node] getting exclusive lock on vnode\n");

    error = vget(vp, LK_EXCLUSIVE | LK_NOWITNESS);
    if (error) {
        if (bootverbose)
            printf("exfat: [exfat_find_node] vget failed: %d\n", error);
        vrele(vp);
        return error;
    }
    
    if (bootverbose)
        printf("exfat: [exfat_find_node] returning locked vnode\n");

    *vpp = vp;
    return 0;
}

/*
 * exfat_create_node: Create and initialize new node and vnode
 *
 * Purpose:
 * - Allocate new exFAT node structure
 * - Create associated vnode
 * - Initialize both structures
 * - Read node metadata from disk
 * - Used when creating new filesystem objects
 *
 * Function calls:
 * - malloc: Allocate node memory
 *   https://man.freebsd.org/cgi/man.cgi?query=malloc&sektion=9
 * - getnewvnode: Create new vnode
 *   https://man.freebsd.org/cgi/man.cgi?query=getnewvnode&sektion=9
 * - vn_lock: Lock vnode
 *   https://man.freebsd.org/cgi/man.cgi?query=vn_lock&sektion=9
 * - vn_unlock: Release vnode lock
 *   https://man.freebsd.org/cgi/man.cgi?query=vn_unlock&sektion=9
 * - vrele: Release vnode reference
 *   https://man.freebsd.org/cgi/man.cgi?query=vrele&sektion=9
 * - exfat_read_node_info: Read node metadata (local)
 */
static int
exfat_create_node(struct exfat_mount *emp, uint32_t cluster, int type,
                  struct vnode **vpp)
{
    struct exfat_node *ep;
    struct vnode *vp;
    int error;

    if (bootverbose)
        printf("exfat: [exfat_create_node] creating node for cluster %u, type %d\n", 
               cluster, type);

    /* Allocate and initialize node */
    if (bootverbose)
        printf("exfat: [exfat_create_node] allocating node structure\n");

    ep = malloc(sizeof(*ep), M_EXFAT, M_WAITOK | M_ZERO);
    if (ep == NULL) {
        if (bootverbose)
            printf("exfat: [exfat_create_node] malloc failed\n");
        return ENOMEM;
    }

    ep->cluster = cluster;
    ep->type = type;
    ep->mp = emp->mp;

    /* Create and lock vnode */
    if (bootverbose)
        printf("exfat: [exfat_create_node] creating new vnode\n");

    error = getnewvnode("exfat", emp->mp, &exfat_vnodeops, &vp);
    if (error) {
        if (bootverbose)
            printf("exfat: [exfat_create_node] getnewvnode failed: %d\n", error);
        free(ep, M_EXFAT);
        return error;
    }

    if (bootverbose)
        printf("exfat: [exfat_create_node] locking new vnode\n");

    error = vn_lock(vp, LK_EXCLUSIVE | LK_RETRY | LK_NOWITNESS);
    if (error) {
        if (bootverbose)
            printf("exfat: [exfat_create_node] vnode lock failed: %d\n", error);
        vrele(vp);
        free(ep, M_EXFAT);
        return error;
    }

    if (bootverbose)
        printf("exfat: [exfat_create_node] linking node and vnode\n");

    /* Link structures */
    vp->v_data = ep;
    ep->vnode = vp;
    vp->v_type = (type == EXFAT_TYPE_FILE) ? VREG : VDIR;

    /* Read metadata */
    if (bootverbose)
        printf("exfat: [exfat_create_node] reading node metadata\n");

    error = exfat_read_node_info(emp, ep);
    if (error) {
        if (bootverbose)
            printf("exfat: [exfat_create_node] read node info failed: %d\n", error);
        vrele(vp);
        free(ep, M_EXFAT);
        return error;
    }

    if (bootverbose)
        printf("exfat: [exfat_create_node] node creation complete\n");

    *vpp = vp;
    return 0;
}

/*
 * exfat_get_node: Main entry point to get or create node
 *
 * Purpose:
 * - Validate request parameters
 * - Try to find existing node first
 * - Create new node if not found
 * - Add new nodes to hash table
 * - Return locked vnode for filesystem operations
 * - Core function for all filesystem object access
 *
 * Function calls:
 * - exfat_find_node: Look up existing node (local)
 * - exfat_create_node: Create new node (local)
 * - exfat_hash_lookup: Check for races (local)
 * - exfat_hash_insert: Add to hash table (local)
 * - mtx_lock/mtx_unlock: Lock hash table mutex
 *   https://man.freebsd.org/cgi/man.cgi?query=mtx_lock&sektion=9
 * - vn_unlock: Release vnode lock
 *   https://man.freebsd.org/cgi/man.cgi?query=vn_unlock&sektion=9
 * - vrele: Release vnode reference
 *   https://man.freebsd.org/cgi/man.cgi?query=vrele&sektion=9
 * - vn_set_state: Set vnode state
 *   https://man.freebsd.org/cgi/man.cgi?query=vn_set_state&sektion=9
 *
 * Returns: 0 with locked vnode in *vpp on success, error code on failure.
 * Note: Caller is responsible for unlocking the returned vnode.
 */
int
exfat_get_node(struct mount *mp, uint32_t cluster, int type, struct vnode **vpp)
{
    struct exfat_mount *emp;
    struct vnode *vp;
    int error;

    if (bootverbose)
        printf("exfat: [exfat_get_node] request for cluster %u, type %d\n", 
               cluster, type);

    /* Validate parameters */
    if (mp == NULL || vpp == NULL) {
        if (bootverbose)
            printf("exfat: [exfat_get_node] invalid parameters\n");
        return EINVAL;
    }
    emp = VFSTOEXFAT(mp);
    if (emp == NULL) {
        if (bootverbose)
            printf("exfat: [exfat_get_node] invalid mount\n");
        return EINVAL;
    }
    if (cluster < EXFAT_CLUSTER_FIRST && cluster != emp->root_cluster) {
        if (bootverbose)
            printf("exfat: [exfat_get_node] invalid cluster %u\n", cluster);
        return EINVAL;
    }

    /* Try to find existing node first */
    error = exfat_find_node(emp, cluster, &vp);
    if (error != ENOENT) {
        if (error == 0) {
            if (bootverbose)
                printf("exfat: [exfat_get_node] found existing node\n");
            *vpp = vp;
        } else if (bootverbose)
            printf("exfat: [exfat_get_node] error finding node: %d\n", error);
        return error;
    }

    /* No existing node, create new one */
    error = exfat_create_node(emp, cluster, type, &vp);
    if (error)
        return error;

    /* Add to hash table */
    mtx_lock(&emp->hash_mtx.mtx);
    exfat_hash_insert(emp, vp->v_data);
    mtx_unlock(&emp->hash_mtx.mtx);

    vn_set_state(vp, VSTATE_CONSTRUCTED);

    if (bootverbose)
        printf("exfat: [exfat_get_node] node setup complete\n");

    *vpp = vp;
    return 0;
}

