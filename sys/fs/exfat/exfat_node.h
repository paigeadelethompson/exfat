#ifndef _FS_EXFAT_NODE_H_
#define _FS_EXFAT_NODE_H_

#include <sys/param.h>
#include <sys/types.h>
#include <sys/queue.h>

/* In-memory node structure */
struct exfat_node {
    struct vnode *vp;                /* Associated vnode */
    uint32_t cluster;                /* First cluster */
    struct exfat_file_info finfo;    /* File information */
    uint32_t diroffset;              /* Offset in directory */
    LIST_ENTRY(exfat_node) node;     /* Hash chain */
};

#define VTOE(vp)     ((struct exfat_node *)(vp)->v_data)
#define ETOV(ep)     ((ep)->vp)

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