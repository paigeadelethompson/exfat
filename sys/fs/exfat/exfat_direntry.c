#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/endian.h>
#include <sys/time.h>

#include "exfat.h"
#include "exfat_dir.h"

/*
 * Calculate checksum for directory entry set
 */
static uint16_t
exfat_checksum_direntry(struct exfat_direntry_set *es)
{
    uint16_t checksum = 0;
    uint8_t *p = (uint8_t *)&es->file;
    int i;

    for (i = 0; i < sizeof(struct exfat_entry_file); i++) {
        if (i != 2 && i != 3) /* Skip checksum field */
            checksum = ((checksum << 15) | (checksum >> 1)) + p[i];
    }

    p = (uint8_t *)&es->stream;
    for (i = 0; i < sizeof(struct exfat_entry_stream); i++)
        checksum = ((checksum << 15) | (checksum >> 1)) + p[i];

    for (i = 0; i < es->name_count; i++) {
        p = (uint8_t *)&es->name[i];
        for (int j = 0; j < sizeof(struct exfat_entry_name); j++)
            checksum = ((checksum << 15) | (checksum >> 1)) + p[j];
    }

    return checksum;
}

/*
 * Convert time_t to ExFAT timestamp
 */
static uint32_t
exfat_time_unix2exfat(time_t t)
{
    struct tm tm;

    gmtime_r(&t, &tm);
    return ((tm.tm_year - 80) << 25) |
           ((tm.tm_mon + 1) << 21) |
           (tm.tm_mday << 16) |
           (tm.tm_hour << 11) |
           (tm.tm_min << 5) |
           (tm.tm_sec >> 1);
}

/*
 * Write directory entry set to directory
 */
static int
exfat_write_direntry(struct vnode *dvp, struct exfat_direntry_set *es, off_t offset)
{
    struct exfat_mount *emp = VTOVFSMP(dvp);
    struct buf *bp;
    uint32_t sector;
    uint32_t sec_offset;
    uint8_t *entry;
    int error;

    /* Calculate sector number */
    sector = emp->boot.cluster_heap_offset +
             ((VTOE(dvp)->finfo.first_cluster - 2) << emp->boot.sectors_per_cluster_shift) +
             (offset >> EXFAT_SECTOR_BITS);

    /* Read sector */
    error = bread(dvp->v_mount->mnt_dev, sector, EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error) {
        brelse(bp);
        return error;
    }

    /* Write entries */
    entry = (uint8_t *)bp->b_data + (offset & (EXFAT_SECTOR_SIZE - 1));
    
    /* File entry */
    memcpy(entry, &es->file, sizeof(struct exfat_entry_file));
    entry += sizeof(struct exfat_entry_file);
    
    /* Stream entry */
    memcpy(entry, &es->stream, sizeof(struct exfat_entry_stream));
    entry += sizeof(struct exfat_entry_stream);
    
    /* Name entries */
    for (int i = 0; i < es->name_count; i++) {
        memcpy(entry, &es->name[i], sizeof(struct exfat_entry_name));
        entry += sizeof(struct exfat_entry_name);
    }

    return bwrite(bp);
}

/*
 * Create a new directory entry
 */
int
exfat_create_entry(struct vnode *dvp, const char *name, int namelen,
                  uint16_t attr, struct timespec *ctime,
                  struct exfat_direntry_set *es)
{
    struct exfat_mount *emp = VTOVFSMP(dvp);
    struct exfat_scan_ctx ctx;
    off_t offset = 0;
    int error;
    int i;

    /* Initialize directory entry set */
    memset(es, 0, sizeof(*es));

    /* File entry */
    es->file.type = EXFAT_ENTRY_FILE;
    es->file.secondary_count = 2; /* Stream entry + at least one name entry */
    es->file.file_attributes = attr;
    es->file.create_timestamp = exfat_time_unix2exfat(ctime->tv_sec);
    es->file.last_modified_timestamp = es->file.create_timestamp;
    es->file.last_access_timestamp = es->file.create_timestamp;

    /* Stream entry */
    es->stream.type = EXFAT_ENTRY_STREAM;
    es->stream.name_length = namelen;

    /* Name entries */
    es->name_count = (namelen + 14) / 15;
    es->file.secondary_count += es->name_count;

    for (i = 0; i < namelen; i++) {
        es->name[i / 15].type = EXFAT_ENTRY_NAME;
        es->name[i / 15].name[i % 15] = htole16((uint16_t)name[i]);
    }

    /* Calculate checksum */
    es->file.checksum = exfat_checksum_direntry(es);

    /* Calculate and set name hash */
    uint16_t name_hash = exfat_calc_name_hash(emp, es->name[0].name, namelen);
    es->stream.name_hash = htole16(name_hash);

    /* Find space in directory */
    error = exfat_scan_directory(dvp, &ctx);
    if (error)
        return error;

    /* Look for a deleted entry or end of directory */
    while (1) {
        struct exfat_direntry_set tmp;
        error = exfat_next_dirent(&ctx, &tmp);
        if (error == ENOENT) {
            /* End of directory - need to extend it */
            if (offset >= emp->bytes_per_cluster) {
                uint32_t cluster = VTOE(dvp)->finfo.first_cluster;
                error = exfat_cluster_extend(emp, &cluster);
                if (error) {
                    exfat_scan_cleanup(&ctx);
                    return error;
                }
            }
            break;
        }
        if (error) {
            exfat_scan_cleanup(&ctx);
            return error;
        }
        offset = ctx.offset;
    }

    exfat_scan_cleanup(&ctx);

    /* Write the new entry */
    return exfat_write_direntry(dvp, es, offset);
}

/*
 * Remove a directory entry
 */
int
exfat_remove_entry(struct vnode *dvp, struct exfat_direntry_set *es, off_t offset)
{
    struct buf *bp;
    uint32_t sector;
    uint8_t *entry;
    int count = es->file.secondary_count + 1;
    int error;

    /* Calculate sector number */
    sector = VTOVFSMP(dvp)->boot.cluster_heap_offset +
             ((VTOE(dvp)->finfo.first_cluster - 2) << 
              VTOVFSMP(dvp)->boot.sectors_per_cluster_shift) +
             (offset >> EXFAT_SECTOR_BITS);

    /* Read sector */
    error = bread(dvp->v_mount->mnt_dev, sector, EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error) {
        brelse(bp);
        return error;
    }

    /* Mark entries as deleted */
    entry = (uint8_t *)bp->b_data + (offset & (EXFAT_SECTOR_SIZE - 1));
    while (count-- > 0) {
        *entry = EXFAT_ENTRY_DELETED;
        entry += sizeof(struct exfat_entry_file); /* All entries are same size */
    }

    return bwrite(bp);
} 