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
 
#ifndef _FS_EXFAT_NODE_H_
#define _FS_EXFAT_NODE_H_

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/timespec.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/vnode.h>

/* Forward declarations */
struct exfat_mount;

/* Node structure */
struct exfat_node {
    struct vnode *vnode;           /* Associated vnode */
    uint32_t cluster;             /* First cluster */
    struct exfat_node *next;      /* Next in hash chain */
    int type;                     /* Node type (file/directory) */
    struct mount *mp;             /* Mount point */
    struct {
        uint32_t first_cluster;   /* First cluster of file */
        uint64_t file_size;       /* File size in bytes */
        uint64_t valid_size;      /* Valid data size */
        uint16_t attributes;      /* File attributes */
        struct timespec create_time;    /* Creation time */
        struct timespec modify_time;    /* Last modification time */
        struct timespec access_time;    /* Last access time */
    } finfo;
    LIST_ENTRY(exfat_node) hash;    /* Hash chain */
};

#define VTOE(vp)     ((struct exfat_node *)(vp)->v_data)
#define ETOV(ep)     ((ep)->vnode)

/* Per-mount hash table mutex */
struct exfat_hash_mtx {
    struct mtx mtx;
};

/* Hash table entry */
struct exfat_hash_entry {
    LIST_ENTRY(exfat_hash_entry) next;  /* Hash chain */
    struct exfat_node *node;            /* Node data */
    uint32_t cluster;                   /* Cluster number (key) */
};

#define EXFAT_HASH_SIZE 64
#define EXFAT_HASH_MASK (EXFAT_HASH_SIZE - 1)

/* Function prototypes */
int exfat_node_init(void);
void exfat_node_uninit(void);
int exfat_read_node_info(struct exfat_mount *emp, struct exfat_node *ep);
void exfat_node_put(struct exfat_node *ep);
int exfat_init_nodes(struct exfat_mount *emp);
void exfat_destroy_nodes(struct exfat_mount *emp);
struct exfat_node *exfat_hash_lookup(struct exfat_mount *emp, uint32_t cluster);
void exfat_hash_insert(struct exfat_mount *emp, struct exfat_node *node);
void exfat_hash_remove(struct exfat_mount *emp, struct exfat_node *node);

#endif /* _FS_EXFAT_NODE_H_ */ 