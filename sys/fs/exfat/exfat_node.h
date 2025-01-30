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

/* In-memory node structure */
struct exfat_node {
    struct vnode    *vnode;         /* Associated vnode */
    uint32_t        cluster;        /* First cluster */
    struct timespec create_time;    /* Creation time */
    struct timespec modify_time;    /* Last modification time */
    struct timespec access_time;    /* Last access time */
    struct {
        uint32_t    first_cluster;  /* First cluster of file */
        uint64_t    file_size;      /* File size in bytes */
        uint64_t    valid_size;     /* Valid data size */
        uint16_t    attributes;     /* File attributes */
    } finfo;
    LIST_ENTRY(exfat_node) hash;    /* Hash chain */
};

#define VTOE(vp)     ((struct exfat_node *)(vp)->v_data)
#define ETOV(ep)     ((ep)->vnode)

/* Node hash table size */
#define EXFAT_NODES_HASH_SIZE   64

/* Node hash function */
#define EXFAT_NODE_HASH(c)     ((c) % EXFAT_NODES_HASH_SIZE)

LIST_HEAD(exfat_node_list, exfat_node);

struct exfat_node_hash {
    LIST_HEAD(, exfat_node) lh_list;
    int lh_count;
};

/* Node hash table */
extern struct exfat_node_hash exfat_node_hash[EXFAT_NODES_HASH_SIZE];
extern struct mtx exfat_node_hash_mtx;

/* Function prototypes */
void exfat_node_init(void);
void exfat_node_uninit(void);
int exfat_read_node_info(struct exfat_mount *emp, struct exfat_node *ep);

#endif /* _FS_EXFAT_NODE_H_ */ 