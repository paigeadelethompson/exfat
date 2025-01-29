#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/endian.h>

#include "exfat.h"
#include "exfat_upcase.h"

/* In-memory upcase table */
struct exfat_upcase {
    uint16_t *table;
    uint32_t size;       /* Number of entries */
    uint32_t checksum;
};

/*
 * Read upcase table from disk
 */
static int
exfat_read_upcase_table(struct exfat_mount *emp, struct exfat_upcase *upcase)
{
    struct buf *bp;
    uint32_t cluster, sector, remaining;
    uint16_t *table;
    int error;

    /* Allocate memory for table */
    table = malloc(upcase->size * sizeof(uint16_t), M_EXFAT, M_WAITOK);
    upcase->table = table;

    /* Read table data */
    cluster = emp->upcase_cluster;
    remaining = upcase->size * sizeof(uint16_t);

    while (remaining > 0 && cluster != EXFAT_CLUSTER_END) {
        uint32_t sectors = emp->boot.sectors_per_cluster_shift;
        
        sector = emp->boot.cluster_heap_offset +
                 ((cluster - 2) << emp->boot.sectors_per_cluster_shift);

        while (sectors-- > 0 && remaining > 0) {
            size_t count = MIN(remaining, EXFAT_SECTOR_SIZE);

            error = bread(emp->mp->mnt_dev, sector++, EXFAT_SECTOR_SIZE, NOCRED, &bp);
            if (error) {
                brelse(bp);
                free(upcase->table, M_EXFAT);
                return error;
            }

            memcpy(table, bp->b_data, count);
            brelse(bp);

            table += count / sizeof(uint16_t);
            remaining -= count;
        }

        if (remaining > 0)
            cluster = exfat_cluster_next(emp, cluster);
    }

    return 0;
}

/*
 * Initialize upcase table
 */
int
exfat_init_upcase(struct exfat_mount *emp)
{
    struct buf *bp;
    struct exfat_entry_upcase *entry;
    struct exfat_upcase *upcase;
    uint32_t sector;
    int error;

    /* Read first sector of root directory */
    sector = emp->boot.cluster_heap_offset +
             ((emp->root_cluster - 2) << emp->boot.sectors_per_cluster_shift);

    error = bread(emp->mp->mnt_dev, sector, EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error) {
        brelse(bp);
        return error;
    }

    /* Look for upcase table entry */
    entry = (struct exfat_entry_upcase *)bp->b_data;
    while (entry->type != EXFAT_ENTRY_UPCASE && entry->type != EXFAT_ENTRY_EOD) {
        entry++;
        if ((uint8_t *)entry >= (uint8_t *)bp->b_data + EXFAT_SECTOR_SIZE) {
            brelse(bp);
            return ENOENT;
        }
    }

    if (entry->type == EXFAT_ENTRY_EOD) {
        brelse(bp);
        return ENOENT;
    }

    /* Allocate upcase structure */
    upcase = malloc(sizeof(*upcase), M_EXFAT, M_WAITOK | M_ZERO);
    upcase->size = le64toh(entry->data_length) / sizeof(uint16_t);
    upcase->checksum = le32toh(entry->checksum);

    /* Store cluster number */
    emp->upcase_cluster = le32toh(entry->first_cluster);
    emp->upcase = upcase;

    brelse(bp);

    /* Read the actual table */
    return exfat_read_upcase_table(emp, upcase);
}

/*
 * Clean up upcase table
 */
void
exfat_cleanup_upcase(struct exfat_mount *emp)
{
    struct exfat_upcase *upcase = emp->upcase;

    if (upcase) {
        if (upcase->table)
            free(upcase->table, M_EXFAT);
        free(upcase, M_EXFAT);
        emp->upcase = NULL;
    }
}

/*
 * Convert character to uppercase
 */
uint16_t
exfat_upcase(struct exfat_mount *emp, uint16_t unicode)
{
    struct exfat_upcase *upcase = emp->upcase;

    if (unicode < upcase->size)
        return le16toh(upcase->table[unicode]);
    return unicode;
}

/*
 * Compare names case-insensitively
 */
int
exfat_name_compare(struct exfat_mount *emp, const uint16_t *name1,
                  const uint16_t *name2, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        uint16_t c1 = exfat_upcase(emp, le16toh(name1[i]));
        uint16_t c2 = exfat_upcase(emp, le16toh(name2[i]));
        
        if (c1 < c2)
            return -1;
        if (c1 > c2)
            return 1;
    }

    return 0;
} 