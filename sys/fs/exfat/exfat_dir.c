#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/endian.h>

#include "exfat.h"
#include "exfat_node.h"
#include "exfat_dir.h"

/*
 * Initialize directory scanning
 */
int
exfat_scan_directory(struct vnode *vp, struct exfat_scan_ctx *ctx)
{
    struct exfat_mount *emp = VTOVFSMP(vp);
    struct exfat_node *ep = VTOE(vp);

    ctx->emp = emp;
    ctx->cluster = ep->finfo.first_cluster;
    ctx->offset = 0;
    ctx->bp = NULL;
    ctx->entry = NULL;

    return 0;
}

/*
 * Read next directory entry set
 */
int
exfat_next_dirent(struct exfat_scan_ctx *ctx, struct exfat_direntry_set *es)
{
    struct buf *bp;
    uint32_t sector;
    int error;

    /* If we need a new sector */
    if (ctx->bp == NULL || ctx->offset >= EXFAT_SECTOR_SIZE) {
        if (ctx->bp != NULL)
            brelse(ctx->bp);

        /* Calculate sector number */
        sector = ctx->emp->boot.cluster_heap_offset +
                ((ctx->cluster - 2) << ctx->emp->boot.sectors_per_cluster_shift) +
                (ctx->offset >> EXFAT_SECTOR_BITS);

        /* Read the sector */
        error = bread(ctx->emp->mp->mnt_dev, sector, EXFAT_SECTOR_SIZE, NOCRED, &bp);
        if (error) {
            ctx->bp = NULL;
            return error;
        }

        ctx->bp = bp;
        ctx->entry = (uint8_t *)bp->b_data;
        ctx->offset &= (EXFAT_SECTOR_SIZE - 1);
    }

    /* Check for end of directory */
    if (ctx->entry[ctx->offset] == EXFAT_ENTRY_EOD) {
        brelse(ctx->bp);
        ctx->bp = NULL;
        return ENOENT;
    }

    /* Skip deleted entries */
    while (ctx->entry[ctx->offset] == EXFAT_ENTRY_DELETED) {
        ctx->offset += sizeof(struct exfat_entry_file);
        if (ctx->offset >= EXFAT_SECTOR_SIZE) {
            /* Need to read next sector */
            return exfat_next_dirent(ctx, es);
        }
    }

    /* Read file entry */
    if (ctx->entry[ctx->offset] != EXFAT_ENTRY_FILE)
        return EINVAL;

    memcpy(&es->file, ctx->entry + ctx->offset, sizeof(struct exfat_entry_file));
    ctx->offset += sizeof(struct exfat_entry_file);

    /* Read stream entry */
    if (ctx->offset >= EXFAT_SECTOR_SIZE) {
        error = exfat_next_dirent(ctx, es);
        if (error)
            return error;
    }
    if (ctx->entry[ctx->offset] != EXFAT_ENTRY_STREAM)
        return EINVAL;

    memcpy(&es->stream, ctx->entry + ctx->offset, sizeof(struct exfat_entry_stream));
    ctx->offset += sizeof(struct exfat_entry_stream);

    /* Read name entries */
    es->name_count = 0;
    while (es->name_count < es->file.secondary_count - 1) {
        if (ctx->offset >= EXFAT_SECTOR_SIZE) {
            error = exfat_next_dirent(ctx, es);
            if (error)
                return error;
        }
        if (ctx->entry[ctx->offset] != EXFAT_ENTRY_NAME)
            return EINVAL;

        memcpy(&es->name[es->name_count], ctx->entry + ctx->offset,
               sizeof(struct exfat_entry_name));
        es->name_count++;
        ctx->offset += sizeof(struct exfat_entry_name);
    }

    return 0;
}

/*
 * Clean up directory scanning context
 */
void
exfat_scan_cleanup(struct exfat_scan_ctx *ctx)
{
    if (ctx->bp != NULL) {
        brelse(ctx->bp);
        ctx->bp = NULL;
    }
}

/*
 * Compare filename with directory entry
 */
int
exfat_name_match(struct exfat_mount *emp, const struct exfat_direntry_set *es,
                const char *name, size_t len)
{
    uint16_t uname[256];
    size_t ulen, i;

    /* Convert ASCII name to UTF-16 */
    for (i = 0; i < len && i < 255; i++)
        uname[i] = (uint16_t)name[i];
    ulen = i;

    /* Check length first */
    if (ulen != es->stream.name_length)
        return 0;

    /* Compare names using upcase table */
    return (exfat_name_compare(emp, uname, es->name[0].name, ulen) == 0);
}

/*
 * Calculate name hash
 */
static uint16_t
exfat_calc_name_hash(struct exfat_mount *emp, const uint16_t *name, size_t len)
{
    uint16_t hash = 0;
    size_t i;

    for (i = 0; i < len; i++) {
        uint16_t c = exfat_upcase(emp, le16toh(name[i]));
        hash = ((hash << 15) | (hash >> 1)) + (c & 0xFF);
        hash = ((hash << 15) | (hash >> 1)) + (c >> 8);
    }

    return hash;
} 