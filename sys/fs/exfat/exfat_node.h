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
#define EXFAT_NODES_HASH_SIZE 64

/* Node hash function */
#define EXFAT_NODE_HASH(cluster) \
    ((cluster) & (EXFAT_NODES_HASH_SIZE - 1))

LIST_HEAD(exfat_node_list, exfat_node);

struct exfat_node_hash {
    struct exfat_node_list *lh_list;
    u_long                  lh_count;
};

int exfat_read_node_info(struct exfat_mount *emp, struct exfat_node *ep);

#endif /* _FS_EXFAT_NODE_H_ */ 