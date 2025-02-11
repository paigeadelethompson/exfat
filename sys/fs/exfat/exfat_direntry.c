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
#include <sys/vnode.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/endian.h>
#include <sys/time.h>
#include <sys/timespec.h>

#include "exfat.h"
#include "exfat_dir.h"
#include "exfat_node.h"

/* Time conversion constants */
#define SECS_PER_MIN    60
#define SECS_PER_HOUR   (60 * SECS_PER_MIN)
#define SECS_PER_DAY    (24 * SECS_PER_HOUR)
#define DAYS_PER_YEAR   365
#define DAYS_PER_LEAP   366

/* Calculate if year is leap year */
static int
is_leap_year(int year)
{
    return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

/* Calculate days in month */
static int
days_in_month(int year, int month)
{
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return (month == 2 && is_leap_year(year)) ? 29 : days[month-1];
}

/* Calculate seconds in year */
static int
secs_per_year(int year)
{
    return is_leap_year(year) ? (DAYS_PER_LEAP * SECS_PER_DAY) : 
                                (DAYS_PER_YEAR * SECS_PER_DAY);
}

/* Calculate seconds in month */
static int
secs_per_month(int year, int month)
{
    return days_in_month(year, month) * SECS_PER_DAY;
}

/* Convert Unix timestamp to ExFAT date/time */
void unix_time_to_exfat(const struct timespec *ts, uint32_t *date, uint32_t *time)
{
    int year, month, day, hour, min, sec;
    time_t t = ts->tv_sec;

    /* Convert seconds since 1970 to date/time */
    year = 1970;
    while (t >= secs_per_year(year)) {
        t -= secs_per_year(year);
        year++;
    }

    month = 1;
    while (t >= secs_per_month(year, month)) {
        t -= secs_per_month(year, month);
        month++;
    }

    day = 1 + (t / SECS_PER_DAY);
    t = t % SECS_PER_DAY;

    hour = t / SECS_PER_HOUR;
    t = t % SECS_PER_HOUR;
    min = t / SECS_PER_MIN;
    sec = t % SECS_PER_MIN;

    *date = EXFAT_DATE(year, month, day);
    *time = EXFAT_TIME(hour, min, sec);
}

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
 * Write directory entry set to directory
 */
int
exfat_write_direntry(struct vnode *dvp, struct exfat_direntry_set *es, off_t offset)
{
    struct exfat_mount *emp = VTOVFSMP(dvp);
    struct buf *bp;
    uint32_t sector;
    uint8_t *entry;
    int error;

    /* Calculate sector number */
    sector = emp->boot.cluster_heap_offset +
             ((VTOE(dvp)->finfo.first_cluster - 2) << emp->boot.sectors_per_cluster_shift) +
             (offset >> EXFAT_SECTOR_BITS);

    /* Read sector */
    error = bread(VTOVFSMP(dvp)->devvp, sector, EXFAT_SECTOR_SIZE, NOCRED, &bp);
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
    if (bootverbose)
        printf("exfat: [%s] creating directory entry '%s' (len %d) in vnode %p\n", __func__, name, namelen, dvp);

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
    unix_time_to_exfat(ctime, &es->file.create_timestamp, &es->file.create_timestamp);
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
    if (bootverbose)
        printf("exfat: [%s] writing directory entry at offset %jd\n", __func__, (intmax_t)offset);

    return exfat_write_direntry(dvp, es, offset);
}

/*
 * Remove a directory entry
 */
int
exfat_remove_entry(struct vnode *dvp, struct exfat_direntry_set *es, off_t offset)
{
    if (bootverbose)
        printf("exfat: [%s] removing directory entry at offset %jd\n", __func__, (intmax_t)offset);

    struct buf *bp;
    uint32_t sector;
    uint8_t *entry;
    int error;

    /* Calculate sector number */
    sector = VTOVFSMP(dvp)->boot.cluster_heap_offset +
             ((VTOE(dvp)->finfo.first_cluster - 2) << 
              VTOVFSMP(dvp)->boot.sectors_per_cluster_shift) +
             (offset >> EXFAT_SECTOR_BITS);

    /* Read sector */
    error = bread(VTOVFSMP(dvp)->devvp, sector, EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error) {
        if (bootverbose)
            printf("exfat: [%s] failed to read sector: %d\n", __func__, error);
        brelse(bp);
        return error;
    }

    /* Mark entries as deleted */
    entry = (uint8_t *)bp->b_data + (offset & (EXFAT_SECTOR_SIZE - 1));
    memset(entry, EXFAT_ENTRY_DELETED,
           (1 + es->file.secondary_count) * sizeof(struct exfat_entry_file));

    if (bootverbose)
        printf("exfat: [%s] marked %d entries as deleted\n", __func__, 1 + es->file.secondary_count);

    /* Write back sector */
    error = bwrite(bp);
    if (error && bootverbose)
        printf("exfat: [%s] failed to write sector: %d\n", __func__, error);

    return error;
} 