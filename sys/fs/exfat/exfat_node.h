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

/* Add before the exfat_node struct definition */
struct exfat_fileinfo {
    uint32_t first_cluster;    /* First cluster of file/directory */
    uint64_t file_size;        /* File size in bytes */
    uint64_t valid_size;       /* Valid data size */
    uint16_t attributes;       /* File attributes */
    struct timespec create_time;     /* Creation time */
    struct timespec modify_time;     /* Last modification time */
    struct timespec access_time;     /* Last access time */
};

/* Node structure */
struct exfat_node {
    struct mount *mp;          /* Mount point */
    uint32_t cluster;         /* Starting cluster */
    uint32_t type;           /* Node type */
    struct exfat_fileinfo finfo;  /* File information */
    LIST_ENTRY(exfat_node) next;  /* Hash chain */
    struct vnode *vnode;      /* Associated vnode */
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

/* List head type for node hash table */
LIST_HEAD(exfat_node_list, exfat_node);

/* Node types */
#define EXFAT_TYPE_FILE    1
#define EXFAT_TYPE_DIR     2

/* Function prototypes */
int exfat_get_node(struct mount *mp, uint32_t cluster, int type, struct vnode **vpp);
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