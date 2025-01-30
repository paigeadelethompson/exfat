#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/vnode.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/mutex.h>

#include "exfat.h"
#include "exfat_node.h"

static struct exfat_node_hash exfat_node_hash[EXFAT_NODES_HASH_SIZE];
static struct mtx exfat_node_hash_mtx;

/*
 * Initialize the node hash table
 */
void
exfat_node_init(void)
{
    int i;

    mtx_init(&exfat_node_hash_mtx, "exfat_node_hash", NULL, MTX_DEF);
    for (i = 0; i < EXFAT_NODES_HASH_SIZE; i++) {
        LIST_INIT(&exfat_node_hash[i].lh_list);
        exfat_node_hash[i].lh_count = 0;
    }
}

/*
 * Clean up the node hash table
 */
void
exfat_node_uninit(void)
{
    mtx_destroy(&exfat_node_hash_mtx);
}

/*
 * Get a node from hash table or create new one
 */
int
exfat_get_node(struct mount *mp, uint32_t cluster, enum vtype type, struct vnode **vpp)
{
    struct exfat_mount *emp = VFSTOEXFAT(mp);
    struct exfat_node *ep;
    struct vnode *vp;
    int error;

    /* Check hash table first */
    mtx_lock(&exfat_node_hash_mtx);
    LIST_FOREACH(ep, &exfat_node_hash[EXFAT_NODE_HASH(cluster)].lh_list, node) {
        if (ep->cluster == cluster && ETOV(ep)->v_mount == mp) {
            vp = ETOV(ep);
            VI_LOCK(vp);
            mtx_unlock(&exfat_node_hash_mtx);
            error = vget(vp, LK_EXCLUSIVE | LK_RETRY, curthread);
            if (error)
                return error;
            *vpp = vp;
            return 0;
        }
    }
    mtx_unlock(&exfat_node_hash_mtx);

    /* Allocate new node */
    error = getnewvnode("exfat", mp, &exfat_vnodeops, &vp);
    if (error)
        return error;

    ep = malloc(sizeof(*ep), M_EXFAT, M_WAITOK | M_ZERO);
    ep->cluster = cluster;
    ep->vp = vp;
    vp->v_data = ep;
    vp->v_type = type;

    /* Read file/directory information */
    error = exfat_read_node_info(emp, ep);
    if (error) {
        free(ep, M_EXFAT);
        vput(vp);
        return error;
    }

    /* Add to hash table */
    mtx_lock(&exfat_node_hash_mtx);
    LIST_INSERT_HEAD(&exfat_node_hash[EXFAT_NODE_HASH(cluster)].lh_list, ep, node);
    exfat_node_hash[EXFAT_NODE_HASH(cluster)].lh_count++;
    mtx_unlock(&exfat_node_hash_mtx);

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
    error = bread(EXFAT_DEV(emp->mp), sector, EXFAT_SECTOR_SIZE, NOCRED, &bp);
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