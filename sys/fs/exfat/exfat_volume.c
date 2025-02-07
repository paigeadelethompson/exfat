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
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/endian.h>

#include "exfat.h"
#include "exfat_volume.h"
#include "exfat_fat.h"

/* Forward declarations */
static int exfat_read_boot_sector(struct exfat_mount *emp);

/*
 * Read volume label from root directory
 */
int
exfat_read_volume_label(struct exfat_mount *emp)
{
    struct buf *bp;
    struct exfat_entry_label *entry;
    uint32_t sector;
    int error;
    size_t outused;

    /* Read first sector of root directory */
    sector = emp->boot.cluster_heap_offset +
             ((emp->root_cluster - 2) << emp->boot.sectors_per_cluster_shift);

    error = bread(emp->devvp, sector, EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error) {
        brelse(bp);
        return error;
    }

    /* Look for volume label entry */
    entry = (struct exfat_entry_label *)bp->b_data;
    while (entry->type != EXFAT_ENTRY_LABEL && entry->type != EXFAT_ENTRY_EOD) {
        entry++;
        if ((uint8_t *)entry >= (uint8_t *)bp->b_data + EXFAT_SECTOR_SIZE) {
            /* No label found - use empty string */
            emp->volume_label[0] = '\0';
            emp->volume_label_len = 0;
            brelse(bp);
            return 0;
        }
    }

    if (entry->type == EXFAT_ENTRY_EOD) {
        /* No label found - use empty string */
        emp->volume_label[0] = '\0';
        emp->volume_label_len = 0;
        brelse(bp);
        return 0;
    }

    /* Convert UTF-16 label to UTF-8 */
    error = exfat_utf16_to_utf8(entry->unicode_label, entry->character_count,
                               emp->volume_label, sizeof(emp->volume_label) - 1,
                               &outused);
    if (error) {
        /* On conversion error, use empty string */
        emp->volume_label[0] = '\0';
        emp->volume_label_len = 0;
    } else {
        emp->volume_label[outused] = '\0';
        emp->volume_label_len = outused;
    }

    brelse(bp);
    return 0;
}

/*
 * Update volume label in root directory
 */
int
exfat_write_volume_label(struct exfat_mount *emp, const char *label, size_t len)
{
    struct buf *bp;
    struct exfat_entry_label *entry;
    uint32_t sector;
    int error;
    size_t outused;

    /* Validate label */
    error = exfat_validate_label(label, len);
    if (error)
        return error;

    /* Read first sector of root directory */
    sector = emp->boot.cluster_heap_offset +
             ((emp->root_cluster - 2) << emp->boot.sectors_per_cluster_shift);

    error = bread(emp->devvp, sector, EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error) {
        brelse(bp);
        return error;
    }

    /* Look for volume label entry or free space */
    entry = (struct exfat_entry_label *)bp->b_data;
    while (entry->type != EXFAT_ENTRY_LABEL && 
           entry->type != EXFAT_ENTRY_EOD &&
           entry->type != EXFAT_ENTRY_DELETED) {
        entry++;
        if ((uint8_t *)entry >= (uint8_t *)bp->b_data + EXFAT_SECTOR_SIZE) {
            brelse(bp);
            return ENOSPC;
        }
    }

    /* Initialize entry */
    memset(entry, 0, sizeof(*entry));
    entry->type = EXFAT_ENTRY_LABEL;

    /* Convert label to UTF-16 */
    error = exfat_utf8_to_utf16(label, entry->unicode_label, 11, &outused);
    if (error) {
        brelse(bp);
        return error;
    }
    entry->character_count = outused;

    /* Write back */
    memcpy(bp->b_data, &emp->boot, sizeof(struct exfat_boot_record));
    exfat_update_sector_checksum(bp);
    error = bwrite(bp);

    /* Update mount structure */
    if (error == 0) {
        strlcpy(emp->volume_label, label, sizeof(emp->volume_label));
        emp->volume_label_len = len;
    }

    return error;
}

/*
 * Initialize allocation bitmap
 */
int
exfat_init_bitmap(struct exfat_mount *emp)
{
    struct buf *bp;
    struct exfat_entry_bitmap *entry;
    uint32_t sector;
    int error;

    /* Read first sector of root directory */
    sector = emp->boot.cluster_heap_offset +
             ((emp->root_cluster - 2) << emp->boot.sectors_per_cluster_shift);

    error = bread(emp->devvp, sector, EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error) {
        brelse(bp);
        return error;
    }

    /* Look for bitmap entry */
    entry = (struct exfat_entry_bitmap *)bp->b_data;
    while (entry->type != EXFAT_ENTRY_BITMAP && entry->type != EXFAT_ENTRY_EOD) {
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

    /* Store bitmap information in mount structure */
    emp->bitmap_cluster = le32toh(entry->first_cluster);
    emp->bitmap_size = le64toh(entry->data_length);

    brelse(bp);
    return 0;
}

/*
 * Clean up bitmap resources
 */
void
exfat_cleanup_bitmap(struct exfat_mount *emp)
{
    /* Nothing to clean up yet */
}

/*
 * Read a bitmap block
 */
static int
exfat_read_bitmap_block(struct exfat_mount *emp, uint32_t block, struct buf **bpp)
{
    uint32_t sector;

    sector = emp->boot.cluster_heap_offset +
             ((emp->bitmap_cluster - 2) << emp->boot.sectors_per_cluster_shift) +
             block;

    return bread(emp->devvp, sector, EXFAT_SECTOR_SIZE, NOCRED, bpp);
}

/*
 * Get bitmap status for a cluster
 */
int
exfat_get_cluster_status(struct exfat_mount *emp, uint32_t cluster, int *used)
{
    struct buf *bp;
    uint32_t byte_offset, bit_offset, block;
    uint8_t *bitmap;
    int error;

    if (cluster < 2 || cluster >= emp->boot.cluster_count + 2)
        return EINVAL;

    /* Calculate offsets */
    cluster -= 2;  /* Clusters start at 2 */
    byte_offset = cluster >> 3;
    bit_offset = cluster & 7;
    block = byte_offset >> EXFAT_SECTOR_BITS;

    /* Read the bitmap block */
    error = exfat_read_bitmap_block(emp, block, &bp);
    if (error)
        return error;

    /* Get cluster status */
    bitmap = (uint8_t *)bp->b_data;
    *used = (bitmap[byte_offset & (EXFAT_SECTOR_SIZE - 1)] >> bit_offset) & 1;

    brelse(bp);
    return 0;
}

/*
 * Set bitmap status for a cluster
 */
static int
exfat_set_cluster_status(struct exfat_mount *emp, uint32_t cluster, int used)
{
    struct buf *bp;
    uint32_t byte_offset, bit_offset, block;
    uint8_t *bitmap, mask;
    int error;

    if (cluster < 2 || cluster >= emp->boot.cluster_count + 2)
        return EINVAL;

    /* Calculate offsets */
    cluster -= 2;  /* Clusters start at 2 */
    byte_offset = cluster >> 3;
    bit_offset = cluster & 7;
    block = byte_offset >> EXFAT_SECTOR_BITS;

    /* Read the bitmap block */
    error = exfat_read_bitmap_block(emp, block, &bp);
    if (error)
        return error;

    /* Update cluster status */
    bitmap = (uint8_t *)bp->b_data;
    byte_offset &= (EXFAT_SECTOR_SIZE - 1);
    mask = 1 << bit_offset;
    
    if (used)
        bitmap[byte_offset] |= mask;
    else
        bitmap[byte_offset] &= ~mask;

    /* Write the block back */
    return bwrite(bp);
}

/*
 * Find a free cluster in bitmap
 */
int
exfat_find_free_cluster(struct exfat_mount *emp, uint32_t start, uint32_t *cluster)
{
    struct buf *bp;
    uint32_t i, j, block;
    uint8_t *bitmap;
    int error;

    /* Start searching from the given cluster */
    i = (start < 2) ? 0 : (start - 2);
    block = i >> (EXFAT_SECTOR_BITS + 3);

    while (block * EXFAT_SECTOR_SIZE * 8 < emp->boot.cluster_count) {
        /* Read bitmap block */
        error = exfat_read_bitmap_block(emp, block, &bp);
        if (error)
            return error;

        bitmap = (uint8_t *)bp->b_data;

        /* Search this block */
        for (j = 0; j < EXFAT_SECTOR_SIZE && (block * EXFAT_SECTOR_SIZE + j) * 8 < emp->boot.cluster_count; j++) {
            if (bitmap[j] != 0xFF) {
                /* Found a byte with a free bit */
                int bit;
                for (bit = 0; bit < 8; bit++) {
                    if ((bitmap[j] & (1 << bit)) == 0) {
                        uint32_t found = ((block * EXFAT_SECTOR_SIZE + j) * 8 + bit) + 2;
                        if (found < emp->boot.cluster_count + 2) {
                            *cluster = found;
                            brelse(bp);
                            return 0;
                        }
                    }
                }
            }
        }

        brelse(bp);
        block++;
    }

    return ENOSPC;
}

/*
 * Update cluster allocation in bitmap
 */
int
exfat_update_bitmap(struct exfat_mount *emp, uint32_t cluster, int allocated)
{
    return exfat_set_cluster_status(emp, cluster, allocated);
}

/*
 * Validate volume label characters
 */
int
exfat_validate_label(const char *label, size_t len)
{
    size_t i, j;

    /* Check length */
    if (len > EXFAT_LABEL_MAX_LEN)
        return EINVAL;

    /* Check for invalid characters */
    for (i = 0; i < len; i++) {
        uint8_t c = (uint8_t)label[i];
        
        /* Check against invalid character table */
        for (j = 0; j < sizeof(exfat_invalid_chars); j++) {
            if (c == exfat_invalid_chars[j])
                return EINVAL;
        }

        /* Only allow ASCII printable characters for now */
        if (c < 0x20 || c > 0x7E)
            return EINVAL;
    }

    return 0;
}

/*
 * Generate a new volume serial number
 */
static uint32_t
exfat_generate_serial(void)
{
    struct timespec ts;
    uint32_t date, time;

    /* Get current time */
    getnanotime(&ts);

    /* Use date and time to generate serial */
    date = ts.tv_sec / 86400;  /* Days since epoch */
    time = ts.tv_sec % 86400;  /* Seconds in day */

    return ((date << EXFAT_SERIAL_DATE_SHIFT) & EXFAT_SERIAL_DATE_MASK) |
           (time & EXFAT_SERIAL_TIME_MASK);
}

/*
 * Update volume serial number
 */
int
exfat_update_serial(struct exfat_mount *emp)
{
    struct buf *bp;
    struct exfat_boot_record *boot;
    int error;

    /* Check for write access */
    if (emp->mp->mnt_flag & MNT_RDONLY)
        return EROFS;

    /* Read boot sector */
    error = bread(emp->devvp, 0, EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error) {
        brelse(bp);
        return error;
    }

    /* Update serial number */
    boot = (struct exfat_boot_record *)bp->b_data;
    boot->volume_serial = htole32(exfat_generate_serial());

    /* Write back */
    error = bwrite(bp);
    if (error)
        return error;

    /* Update mount structure */
    emp->boot.volume_serial = boot->volume_serial;

    return 0;
}

/*
 * Get volume serial number as string
 */
void
exfat_serial_to_string(uint32_t serial, char *str)
{
    snprintf(str, 12, "%04X-%04X",
             (serial >> 16) & 0xFFFF,
             serial & 0xFFFF);
}

/*
 * Update volume flags in boot sector
 */
static int
exfat_update_volume_flags(struct exfat_mount *emp, uint16_t flags)
{
    struct buf *bp;
    struct exfat_boot_record *boot;
    int error;

    /* Check for write access */
    if (emp->mp->mnt_flag & MNT_RDONLY)
        return EROFS;

    /* Read boot sector */
    error = bread(emp->devvp, 0, EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error) {
        brelse(bp);
        return error;
    }

    /* Update flags */
    boot = (struct exfat_boot_record *)bp->b_data;
    boot->volume_flags = htole16(flags);

    /* Write back */
    error = bwrite(bp);
    if (error)
        return error;

    /* Update mount structure */
    emp->boot.volume_flags = flags;

    return 0;
}

/*
 * Mark volume as dirty/clean
 */
int
exfat_set_volume_dirty(struct exfat_mount *emp, int dirty)
{
    if (bootverbose)
        printf("exfat: marking volume %s\n", dirty ? "dirty" : "clean");

    struct buf *bp;
    int error;

    /* Read boot sector */
    error = bread(emp->devvp, 0, EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error) {
        if (bootverbose)
            printf("exfat: failed to read boot sector: %d\n", error);
        brelse(bp);
        return error;
    }

    /* Update volume state */
    struct exfat_boot_record *bs = (struct exfat_boot_record *)bp->b_data;
    bs->volume_state = dirty ? EXFAT_STATE_DIRTY : EXFAT_STATE_CLEAN;

    /* Write back boot sector */
    error = bwrite(bp);
    if (error) {
        if (bootverbose)
            printf("exfat: failed to write boot sector: %d\n", error);
        return error;
    }

    if (bootverbose)
        printf("exfat: volume marked %s successfully\n", 
               dirty ? "dirty" : "clean");

    return 0;
}

/*
 * Select active FAT
 */
int
exfat_set_active_fat(struct exfat_mount *emp, int second_fat)
{
    uint16_t flags = emp->boot.volume_flags;

    if (second_fat)
        flags |= EXFAT_VOL_ACTIVE_FAT;
    else
        flags &= ~EXFAT_VOL_ACTIVE_FAT;

    return exfat_update_volume_flags(emp, flags);
}

/*
 * Check if volume is dirty
 */
int
exfat_volume_is_dirty(struct exfat_mount *emp)
{
    return (emp->boot.volume_flags & EXFAT_VOL_DIRTY) != 0;
}

/*
 * Get active FAT number (0 = first FAT, 1 = second FAT)
 */
int
exfat_get_active_fat(struct exfat_mount *emp)
{
    return (emp->boot.volume_flags & EXFAT_VOL_ACTIVE_FAT) ? 1 : 0;
}

/*
 * Convert Unix time to ExFAT time
 */
void
exfat_unix2exfat(struct timespec *ts, struct exfat_timespec *extime)
{
    uint32_t days, secs, year, month, day, hour, min, sec;

    /* Convert seconds since epoch to date/time */
    days = ts->tv_sec / 86400;
    secs = ts->tv_sec % 86400;

    if (days < 365) {  /* Before 1981 */
        year = 0;
        month = 1;
        day = 1;
    } else {
        /* Simple conversion - could be optimized */
        year = 1970;
        while (days >= 365) {
            days -= 365;
            if ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)
                days--;
            year++;
        }
        /* Convert remaining days to month/day */
        month = 1;
        while (days > 28) {
            static const int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
            if (days < mdays[month-1])
                break;
            days -= mdays[month-1];
            month++;
        }
        day = days + 1;
    }

    hour = secs / 3600;
    secs %= 3600;
    min = secs / 60;
    sec = secs % 60;

    extime->date = EXFAT_DATE(year, month, day);
    extime->time = EXFAT_TIME(hour, min, sec);
    extime->time_ms = ts->tv_nsec / 1000000;
    extime->tz_offset = 0;  /* UTC */
}

/*
 * Convert ExFAT time to Unix time
 */
void
exfat_exfat2unix(struct exfat_timespec *extime, struct timespec *ts)
{
    uint32_t year, month, day, hour, min, sec;
    uint32_t days = 0;
    
    year = EXFAT_YEAR(extime->date);
    month = EXFAT_MONTH(extime->date);
    day = EXFAT_DAY(extime->date);
    hour = EXFAT_HOUR(extime->time);
    min = EXFAT_MINUTE(extime->time);
    sec = EXFAT_SECOND(extime->time);

    /* Convert to days since epoch */
    if (year > 1970) {
        days = (year - 1970) * 365;
        days += (year - 1969) / 4;  /* Leap years */
        days -= (year - 1901) / 100;
        days += (year - 1601) / 400;
    }

    /* Add days for months */
    while (--month > 0) {
        static const int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
        days += mdays[month-1];
        if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
            days++;
    }

    days += day - 1;

    ts->tv_sec = days * 86400 + hour * 3600 + min * 60 + sec;
    ts->tv_nsec = extime->time_ms * 1000000;
}

/*
 * Update volume creation time
 */
int
exfat_update_volume_time(struct exfat_mount *emp)
{
    struct buf *bp;
    struct timespec ts;
    struct exfat_timespec extime;
    int error;

    /* Check for write access */
    if (emp->mp->mnt_flag & MNT_RDONLY)
        return EROFS;

    /* Get current time */
    getnanotime(&ts);
    exfat_unix2exfat(&ts, &extime);

    /* Read boot sector */
    error = bread(emp->devvp, 0, EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error) {
        brelse(bp);
        return error;
    }

    /* Write back */
    error = bwrite(bp);

    return error;
}

/*
 * Update volume percent-in-use value
 */
int
exfat_update_percent_in_use(struct exfat_mount *emp)
{
    if (bootverbose)
        printf("exfat: updating percent-in-use\n");

    struct buf *bp;
    uint32_t cluster;
    uint32_t used_clusters = 0;
    int error, used;

    /* Count used clusters from bitmap */
    for (cluster = 2; cluster < emp->boot.cluster_count + 2; cluster++) {
        error = exfat_get_cluster_status(emp, cluster, &used);
        if (error)
            return error;
        if (used)
            used_clusters++;
    }

    /* Calculate percentage (0-100) */
    uint8_t percent = (used_clusters * 100) / emp->boot.cluster_count;

    /* Read current boot sector */
    error = exfat_read_boot_sector(emp);
    if (error)
        return error;

    /* Update percent in use */
    emp->boot.percent_in_use = percent;

    /* Write back */
    error = bread(emp->devvp, 0, EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error) {
        if (bootverbose)
            printf("exfat: failed to read boot sector: %d\n", error);
        brelse(bp);
        return error;
    }

    memcpy(bp->b_data, &emp->boot, sizeof(struct exfat_boot_record));
    exfat_update_sector_checksum(bp);
    error = bwrite(bp);
    if (error) {
        if (bootverbose)
            printf("exfat: failed to write boot sector: %d\n", error);
        return error;
    }

    if (bootverbose)
        printf("exfat: percent-in-use updated to %u%%\n", percent);

    return 0;
}

static int
exfat_read_boot_sector(struct exfat_mount *emp)
{
    if (bootverbose)
        printf("exfat: reading boot sector\n");

    struct buf *bp;
    int error;

    /* Read boot sector */
    error = bread(emp->devvp, 0, EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error) {
        if (bootverbose)
            printf("exfat: failed to read boot sector: %d\n", error);
        brelse(bp);
        return error;
    }

    /* Copy and validate boot sector */
    memcpy(&emp->boot, bp->b_data, sizeof(struct exfat_boot_record));
    
    /* Verify sector checksum */
    error = exfat_verify_sector(bp);
    if (error) {
        if (bootverbose)
            printf("exfat: boot sector checksum verification failed\n");
        brelse(bp);
        return error;
    }
    
    brelse(bp);

    /* Check signature */
    if (memcmp(emp->boot.fs_name, "EXFAT   ", 8) != 0) {
        if (bootverbose)
            printf("exfat: invalid filesystem signature\n");
        return EINVAL;
    }

    if (bootverbose) {
        printf("exfat: filesystem parameters:\n");
        printf("  bytes per sector: %u\n", 1 << emp->boot.bytes_per_sector_shift);
        printf("  sectors per cluster: %u\n", 1 << emp->boot.sectors_per_cluster_shift);
        printf("  number of FATs: %u\n", emp->boot.number_of_fats);
        printf("  volume serial: %08x\n", emp->boot.volume_serial);
        printf("  FAT offset: %u\n", emp->boot.fat_offset);
        printf("  FAT length: %u\n", emp->boot.fat_length);
        printf("  cluster heap offset: %u\n", emp->boot.cluster_heap_offset);
        printf("  cluster count: %u\n", emp->boot.cluster_count);
        printf("  root dir cluster: %u\n", emp->boot.root_dir_cluster);
    }

    return 0;
}

/*
 * Verify sector checksum
 */
int
exfat_verify_sector(struct buf *bp)
{
    uint32_t checksum = 0;
    uint8_t *data = bp->b_data;
    int i;

    /* Calculate checksum of sector data */
    for (i = 0; i < EXFAT_SECTOR_SIZE - 4; i++)
        checksum = ((checksum << 31) | (checksum >> 1)) + data[i];

    /* Compare with stored checksum */
    uint32_t stored = le32dec(data + EXFAT_SECTOR_SIZE - 4);
    return (checksum == stored) ? 0 : EIO;
}

/*
 * Update sector checksum
 */
void
exfat_update_sector_checksum(struct buf *bp)
{
    uint32_t checksum = 0;
    uint8_t *data = bp->b_data;
    int i;

    /* Calculate new checksum */
    for (i = 0; i < EXFAT_SECTOR_SIZE - 4; i++)
        checksum = ((checksum << 31) | (checksum >> 1)) + data[i];

    /* Store checksum */
    le32enc(data + EXFAT_SECTOR_SIZE - 4, checksum);
}

/*
 * Handle bad sector
 */
int
exfat_handle_bad_sector(struct exfat_mount *emp, daddr_t sector)
{
    if (bootverbose)
        printf("exfat: handling bad sector %jd\n", (intmax_t)sector);

    /* Convert sector to cluster */
    uint32_t cluster;
    if (sector >= emp->boot.cluster_heap_offset) {
        cluster = 2 + ((sector - emp->boot.cluster_heap_offset) >>
                      emp->boot.sectors_per_cluster_shift);
        
        /* Mark cluster as bad */
        int error = exfat_mark_cluster_bad(emp, cluster);
        if (error && bootverbose)
            printf("exfat: failed to mark cluster %u as bad: %d\n", 
                   cluster, error);

        /* Update error statistics */
        emp->error_count++;
        vfs_timestamp(&emp->last_error_time);
        emp->mount_flags |= EXFAT_MNT_ERRORS;

        return error;
    }

    /* Critical sector (boot/FAT/etc) - mark filesystem for check */
    emp->mount_flags |= EXFAT_MNT_FSCK;
    return EIO;
}

/*
 * Check if a sector is bad by attempting to read it
 */
static int
exfat_check_sector(struct exfat_mount *emp, daddr_t sector)
{
    struct buf *bp;
    int error;

    error = bread(emp->devvp, sector, EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error) {
        brelse(bp);
        return 1;  /* Bad sector */
    }

    brelse(bp);
    return 0;  /* Good sector */
}

/*
 * Scan a cluster for bad sectors
 */
int
exfat_scan_cluster(struct exfat_mount *emp, uint32_t cluster)
{
    daddr_t sector;
    int i, bad_sectors = 0;

    sector = emp->boot.cluster_heap_offset +
             ((cluster - 2) << emp->boot.sectors_per_cluster_shift);

    /* Check each sector in the cluster */
    for (i = 0; i < (1 << emp->boot.sectors_per_cluster_shift); i++) {
        if (exfat_check_sector(emp, sector + i)) {
            bad_sectors++;
            if (bootverbose)
                printf("exfat: bad sector %jd in cluster %u\n",
                       (intmax_t)(sector + i), cluster);
        }
    }

    /* If any sectors are bad, mark the whole cluster as bad */
    if (bad_sectors > 0) {
        if (bootverbose)
            printf("exfat: marking cluster %u as bad (%d bad sectors)\n",
                   cluster, bad_sectors);
        return exfat_mark_cluster_bad(emp, cluster);
    }

    return 0;
}

/*
 * Scan a range of clusters for bad sectors
 */
int
exfat_scan_clusters(struct exfat_mount *emp, uint32_t start, uint32_t count)
{
    uint32_t cluster;
    int error = 0;

    for (cluster = start; cluster < start + count; cluster++) {
        error = exfat_scan_cluster(emp, cluster);
        if (error && bootverbose)
            printf("exfat: error scanning cluster %u: %d\n", cluster, error);
    }

    return error;
}

/*
 * Clean up upcase table resources
 */
void
exfat_cleanup_upcase(struct exfat_mount *emp)
{
    if (bootverbose)
        printf("exfat: cleaning up upcase table\n");

    /* Free upcase table if allocated */
    if (emp->upcase) {
        free(emp->upcase, M_EXFAT);
        emp->upcase = NULL;
    }
}

/*
 * Initialize upcase table
 */
int
exfat_init_upcase(struct exfat_mount *emp)
{
    struct buf *bp;
    struct exfat_entry_upcase *entry;
    uint32_t sector;
    int error;

    if (bootverbose)
        printf("exfat: initializing upcase table\n");

    /* Read first sector of root directory */
    sector = emp->boot.cluster_heap_offset +
             ((emp->root_cluster - 2) << emp->boot.sectors_per_cluster_shift);

    error = bread(emp->devvp, sector, EXFAT_SECTOR_SIZE, NOCRED, &bp);
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

    /* Store upcase table information */
    emp->upcase_cluster = le32toh(entry->first_cluster);
    uint64_t upcase_size = le64toh(entry->data_length);

    /* Allocate memory for upcase table */
    emp->upcase = malloc(upcase_size, M_EXFAT, M_WAITOK);
    if (emp->upcase == NULL) {
        brelse(bp);
        return ENOMEM;
    }

    /* Read upcase table data */
    sector = emp->boot.cluster_heap_offset +
             ((emp->upcase_cluster - 2) << emp->boot.sectors_per_cluster_shift);

    error = bread(emp->devvp, sector, upcase_size, NOCRED, &bp);
    if (error) {
        free(emp->upcase, M_EXFAT);
        emp->upcase = NULL;
        brelse(bp);
        return error;
    }

    /* Copy upcase table data */
    memcpy(emp->upcase, bp->b_data, upcase_size);
    brelse(bp);

    if (bootverbose)
        printf("exfat: upcase table initialized (%ju bytes)\n", 
               (uintmax_t)upcase_size);

    return 0;
} 